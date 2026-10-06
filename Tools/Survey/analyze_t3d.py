import os, re, sys, json, glob, collections

D = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("DREAMANIM_T3D_DIR", ".")

RE_BEGIN = re.compile(r'^(\s*)Begin Object (?:Class=(\S+) )?Name="([^"]+)" ExportPath="([^\']+)\'([^\']+)\'"')
RE_END = re.compile(r'^(\s*)End Object')


class Obj:
    __slots__ = ("path", "cls", "props", "pins")

    def __init__(self, path, cls):
        self.path = path
        self.cls = cls
        self.props = []
        self.pins = []


def parse(fn):
    objs = {}
    stack = []
    with open(fn, "rb") as fb:
        bom = fb.read(2)
    enc = "utf-16" if bom in (b"\xff\xfe", b"\xfe\xff") else "utf-8"
    with open(fn, encoding=enc, errors="replace") as f:
        for line in f:
            m = RE_BEGIN.match(line)
            if m:
                cls = m.group(4).split(".")[-1]
                path = m.group(5)
                o = objs.get(path)
                if o is None:
                    o = objs[path] = Obj(path, cls)
                stack.append(o)
                continue
            if RE_END.match(line):
                if stack:
                    stack.pop()
                continue
            if stack:
                s = line.strip()
                if s.startswith("CustomProperties Pin "):
                    stack[-1].pins.append(s)
                elif s:
                    stack[-1].props.append(s)
    return objs


def local(path):
    # "/ALS/X.AB_Als:AnimGraph.Node_1" -> ["AnimGraph", "Node_1"]
    if ":" not in path:
        return []
    return path.split(":", 1)[1].split(".")


def prop(o, key):
    for p in o.props:
        if p.startswith(key + "="):
            return p[len(key) + 1:]
    return None


SCHEMA_KIND = {
    "EdGraphSchema_K2": "k2",
    "AnimationGraphSchema": "anim",
    "AnimationStateMachineSchema": "statemachine",
    "AnimationStateGraphSchema": "state",
    "AnimationTransitionSchema": "transition",
    "AnimationConduitGraphSchema": "conduit",
    "AnimationCustomTransitionSchema": "customtransition",
}


def graph_kind(o):
    sch = prop(o, "Schema") or ""
    for k, v in SCHEMA_KIND.items():
        if k in sch:
            return v
    return "?" + o.cls


def link_count(pin):
    m = re.search(r'LinkedTo=\(([^)]*)\)', pin)
    if not m:
        return 0
    return len([x for x in m.group(1).split(",") if x.strip()])


def pin_name(pin):
    m = re.search(r'PinName="([^"]*)"', pin)
    return m.group(1) if m else "?"


def pin_dir_out(pin):
    return 'Direction="EGPD_Output"' in pin


def pin_cat(pin):
    m = re.search(r'PinType\.PinCategory="([^"]*)"', pin)
    return m.group(1) if m else ""


def func_ref(o):
    v = prop(o, "FunctionReference") or ""
    m = re.search(r'MemberName="([^"]+)"', v)
    mp = re.search(r"MemberParent=\"[^']*'([^']+)'\"", v)
    owner = mp.group(1).split(".")[-1] if mp else ("self" if "bSelfContext=True" in v else "")
    return (owner + "::" if owner else "") + (m.group(1) if m else "?")


def var_ref(o):
    v = prop(o, "VariableReference") or ""
    m = re.search(r'MemberName="([^"]+)"', v)
    return m.group(1) if m else "?"


RE_GRAPHREF = re.compile(r"^(UbergraphPages|FunctionGraphs|MacroGraphs|DelegateSignatureGraphs)\(\d+\)=\"[^']*'([^']+)'\"")


def analyze(fn):
    objs = parse(fn)
    asset = os.path.basename(fn)[:-4].split("__")[-1]
    # keep only authored graphs: those the blueprint lists, plus everything nested under them
    bp = [o for o in objs.values() if ":" not in o.path and o.cls.endswith("Blueprint")]
    allowed = set()
    if bp:
        for p in bp[0].props:
            m = RE_GRAPHREF.match(p)
            if m:
                allowed.add(m.group(2))
    objs = {p: o for p, o in objs.items() if ":" not in p or (local(p) and local(p)[0] in allowed)}
    graphs = {}
    nodes = []
    for o in objs.values():
        if o.cls.endswith("Graph") and prop(o, "Schema") is not None:
            graphs[o.path] = o
    gk = {p: graph_kind(g) for p, g in graphs.items()}

    out = {
        "asset": asset,
        "graphs": collections.Counter(gk.values()),
        "nodes_by_graphkind": collections.defaultdict(collections.Counter),
        "k2_in_anim": collections.Counter(),
        "functions": {},
        "transitions": [],
        "node_funcs": collections.Counter(),
        "bindings": collections.Counter(),
        "state_events": collections.Counter(),
        "anim_nodes": collections.Counter(),
        "events": collections.Counter(),
        "calls_in_k2": collections.Counter(),
        "parent": "",
    }
    root = [o for o in objs.values() if ":" not in o.path and o.cls.endswith("Blueprint")]
    if root:
        pc = prop(root[0], "ParentClass") or ""
        out["parent"] = pc.split(".")[-1].rstrip("'\"")

    # index children nodes per graph (direct children only)
    for o in objs.values():
        loc = local(o.path)
        if len(loc) < 2:
            continue
        parent_path = o.path.rsplit(".", 1)[0]
        if parent_path not in graphs:
            continue
        kind = gk[parent_path]
        if o.cls in ("EdGraphNode_Comment",):
            continue
        out["nodes_by_graphkind"][kind][o.cls] += 1
        if kind in ("anim", "state") and o.cls.startswith("K2Node"):
            out["k2_in_anim"][o.cls] += 1
        if o.cls.startswith("AnimGraphNode") or o.cls.startswith("AnimGraphNode_"):
            out["anim_nodes"][o.cls] += 1
            for key in ("InitialUpdateFunction", "BecomeRelevantFunction", "UpdateFunction"):
                v = prop(o, key)
                if v:
                    m = re.search(r'MemberName="([^"]+)"', v)
                    if m:
                        out["node_funcs"][key + ":" + m.group(1)] += 1
        if o.cls == "AnimStateNode":
            for key in ("StateEntered", "StateLeft", "StateFullyBlended"):
                v = prop(o, key)
                if v and "NotifyName" in v:
                    out["state_events"][key] += 1
        if o.cls in ("K2Node_CallFunction", "K2Node_CallFunctionOnMember", "K2Node_CallParentFunction"):
            out["calls_in_k2"][kind + ":" + func_ref(o)] += 1
        if o.cls in ("K2Node_Event", "K2Node_CustomEvent"):
            ev = prop(o, "CustomFunctionName") or func_ref(o)
            out["events"][ev.strip('"')] += 1

    # property access bindings
    for o in objs.values():
        if o.cls.startswith("AnimGraphNodeBinding"):
            for p in o.props:
                if p.startswith("PropertyBindings="):
                    for m in re.finditer(r'PathAsText="([^"]*)"', p):
                        out["bindings"][m.group(1)] += 1

    # function graphs (k2 kind, not EventGraph)
    for p, g in graphs.items():
        k = gk[p]
        if k == "k2":
            name = local(p)[-1]
            cnt = collections.Counter()
            calls = []
            for o in objs.values():
                if o.path.rsplit(".", 1)[0] == p and o.cls != "EdGraphNode_Comment":
                    cnt[o.cls] += 1
                    if o.cls.startswith("K2Node_CallFunction"):
                        calls.append(func_ref(o))
            out["functions"][name] = {"n": sum(cnt.values()), "classes": dict(cnt), "calls": calls}

    # transitions
    for p, g in graphs.items():
        if gk[p] not in ("transition", "conduit"):
            continue
        tnode_path = p.rsplit(".", 1)[0]
        tnode = objs.get(tnode_path)
        cnt = collections.Counter()
        calls, vars_, ops = [], [], []
        for o in objs.values():
            if o.path.rsplit(".", 1)[0] == p and o.cls != "EdGraphNode_Comment":
                cnt[o.cls] += 1
                if o.cls.startswith("K2Node_CallFunction") or o.cls in ("K2Node_CommutativeAssociativeBinaryOperator", "K2Node_PromotableOperator"):
                    calls.append(func_ref(o))
                elif o.cls in ("K2Node_VariableGet",):
                    vars_.append(var_ref(o))
                elif o.cls == "K2Node_PropertyAccess":
                    pa = prop(o, "Path") or prop(o, "TextPath") or ""
                    vars_.append("PA:" + pa[:60])
        rec = {
            "kind": gk[p],
            "n": sum(cnt.values()),
            "classes": dict(cnt),
            "calls": calls,
            "vars": vars_,
            "auto": bool(tnode and (prop(tnode, "bAutomaticRuleBasedOnSequencePlayerInState") == "True")),
            "custom_blend": bool(tnode and prop(tnode, "CustomTransitionGraph")),
        }
        if tnode is not None:
            rec["blend_mode"] = prop(tnode, "BlendMode")
            rec["logic"] = prop(tnode, "LogicType")
            rec["xfade"] = prop(tnode, "CrossfadeDuration")
        out["transitions"].append(rec)
    return out


def main():
    files = sorted(glob.glob(os.path.join(D, "*.t3d")))
    results = []
    for fn in files:
        if "CR_Als" in fn:
            continue
        r = analyze(fn)
        results.append(r)
    # serialise
    def conv(x):
        if isinstance(x, (collections.Counter, collections.defaultdict)):
            return {k: conv(v) for k, v in x.items()}
        if isinstance(x, dict):
            return {k: conv(v) for k, v in x.items()}
        if isinstance(x, list):
            return [conv(v) for v in x]
        return x
    with open(os.path.join(D, "_analysis.json"), "w", encoding="utf-8") as f:
        json.dump([conv(r) for r in results], f, ensure_ascii=False, indent=1)
    print("written", len(results))


if __name__ == "__main__":
    main()
