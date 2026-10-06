"""Print an AnimBP T3D export as a readable pose tree / state machine outline.

usage: python dump_tree.py <file.t3d> [--all-props]
"""
import os, re, sys, collections

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import analyze_t3d as A

RE_PIN_NAME = re.compile(r'PinName="([^"]*)"')
RE_LINKED = re.compile(r'LinkedTo=\(([^)]*)\)')
RE_DEFAULT = re.compile(r'DefaultValue="((?:[^"\\]|\\.)*)"')
RE_DEFOBJ = re.compile(r'DefaultObject="([^"]*)"')
POSE_TYPES = ("PoseLink'", "ComponentSpacePoseLink'")


class Pin:
    def __init__(self, s):
        self.raw = s
        self.name = RE_PIN_NAME.search(s).group(1)
        self.out = 'Direction="EGPD_Output"' in s
        self.pose = any(t in s for t in POSE_TYPES)
        self.exec = 'PinCategory="exec"' in s
        self.hidden = "bHidden=True" in s
        m = RE_LINKED.search(s)
        self.links = []
        if m:
            for part in m.group(1).split(","):
                part = part.strip()
                if part:
                    node, pid = part.rsplit(" ", 1)
                    self.links.append((node, pid))
        m = RE_PIN_ID.search(s)
        self.id = m.group(1) if m else ""
        m = RE_DEFAULT.search(s)
        self.default = m.group(1) if m else None
        m = RE_DEFOBJ.search(s)
        self.defobj = m.group(1) if m else None


RE_PIN_ID = re.compile(r"PinId=([0-9A-F]+)")


def short_cls(c):
    for p in ("AnimGraphNode_", "AlsAnimGraphNode_", "K2Node_", "AnimState"):
        if c.startswith(p):
            return c[len(p):] if p != "AnimState" else c
    return c


def node_struct(o):
    v = A.prop(o, "Node")
    return v if v and v != "()" else ""


def bindings(objs, o):
    res = {}
    for p, sub in objs.items():
        if p.rsplit(".", 1)[0] == o.path and sub.cls.startswith("AnimGraphNodeBinding"):
            for pr in sub.props:
                if pr.startswith("PropertyBindings="):
                    for m in re.finditer(r'\("([^"]+)", \(', pr):
                        m2 = re.compile(r'PathAsText="([^"]*)"').search(pr, m.end())
                        if m2:
                            res[m.group(1)] = m2.group(1)
    return res


class Graph:
    def __init__(self, objs, gpath):
        self.objs = objs
        self.path = gpath
        self.nodes = {}
        for p, o in objs.items():
            if p.rsplit(".", 1)[0] == gpath and o.cls != "EdGraphNode_Comment" and A.prop(o, "Schema") is None:
                self.nodes[p.rsplit(".", 1)[1]] = o
        self.pins = {n: [Pin(s) for s in o.pins] for n, o in self.nodes.items()}
        self.pin_by_id = {}
        for n, ps in self.pins.items():
            for pin in ps:
                self.pin_by_id[(n, pin.id)] = pin

    def source_of(self, node, pin):
        # follow an input pin to the node feeding it, skipping knots
        if not pin.links:
            return None
        n, pid = pin.links[0]
        o = self.nodes.get(n)
        while o is not None and o.cls == "K2Node_Knot":
            ins = [p for p in self.pins[n] if not p.out]
            if not ins or not ins[0].links:
                return None
            n, pid = ins[0].links[0]
            o = self.nodes.get(n)
        return n

    def expr(self, n, depth=0):
        o = self.nodes.get(n)
        if o is None:
            return "?" + n
        c = o.cls
        ins = [p for p in self.pins[n] if not p.out and not p.exec and not p.hidden and p.name not in ("self", "__WorldContext")]
        if c == "K2Node_PropertyAccess":
            tp = A.prop(o, "TextPath") or A.prop(o, "Path") or ""
            m = re.search(r'"([^"]+)"', tp)
            return m.group(1) if m else tp
        if c == "K2Node_VariableGet":
            return A.var_ref(o)
        if c == "K2Node_AnimGetter":
            fr = A.func_ref(o).split("::")[-1]
            args = []
            for k in ("SourceStateName", "SourceMachineName"):
                pass
            return fr + "(" + (A.prop(o, "SourceStateName") or A.prop(o, "CachedTitle") or "") + ")"
        name = A.func_ref(o).split("::")[-1] if c.startswith("K2Node_Call") or c in ("K2Node_CommutativeAssociativeBinaryOperator", "K2Node_PromotableOperator") else short_cls(c)
        if c == "K2Node_PromotableOperator":
            name = (A.prop(o, "OperationName") or name).strip('"')
        args = []
        for p in ins:
            if p.links:
                src = self.source_of(n, p)
                args.append(self.expr(src, depth + 1) if src else "?")
            else:
                v = p.defobj if p.defobj and p.defobj != "None" else p.default
                args.append(repr_val(v))
        return "%s(%s)" % (name, ", ".join(args))

    def pose_tree(self, n, indent, out, seen):
        o = self.nodes.get(n)
        pad = "  " * indent
        if o is None:
            out.append(pad + "?" + n)
            return
        c = short_cls(o.cls)
        extra = []
        st = node_struct(o)
        if st:
            extra.append(st[:400])
        b = bindings(self.objs, o)
        for pin in self.pins[n]:
            if pin.out or pin.pose or pin.exec or pin.hidden:
                continue
            if pin.name in b:
                extra.append("%s <- %s" % (pin.name, b.pop(pin.name)))
            elif pin.links:
                src = self.source_of(n, pin)
                extra.append("%s <- {%s}" % (pin.name, self.expr(src) if src else "?"))
            elif pin.default not in (None, "") or pin.defobj not in (None, "None"):
                v = pin.defobj if pin.defobj and pin.defobj != "None" else pin.default
                extra.append("%s = %s" % (pin.name, v))
        for k, v in b.items():
            extra.append("%s <- %s" % (k, v))
        for key in ("CallSite", "Tag", "InitialUpdateFunction", "BecomeRelevantFunction", "UpdateFunction"):
            v = A.prop(o, key)
            if v:
                extra.append("%s=%s" % (key, v))
        if o.cls == "AnimGraphNode_CallFunction":
            inner = A.prop(o, "InnerGraph")
            for p2, o2 in self.objs.items():
                if p2.startswith(o.path + ".") and o2.cls.startswith("K2Node_CallFunction"):
                    extra.append("calls " + A.func_ref(o2))
        if o.cls == "AnimGraphNode_StateMachine":
            extra.append("machine")
        out.append(pad + "%s %s  %s" % (c, n, " | ".join(extra)))
        if n in seen:
            out.append(pad + "  (already printed)")
            return
        seen.add(n)
        for pin in self.pins[n]:
            if pin.out or not pin.pose:
                continue
            src = self.source_of(n, pin)
            out.append(pad + "  [%s]" % pin.name)
            if src:
                self.pose_tree(src, indent + 2, out, seen)


def repr_val(v):
    if v is None:
        return "?"
    return v


def dump_graph(objs, gpath, out, indent=0):
    g = Graph(objs, gpath)
    kind = A.graph_kind(objs[gpath])
    pad = "  " * indent
    name = gpath.rsplit(".", 1)[-1].split(":")[-1]
    if kind in ("anim", "state"):
        out.append(pad + "== %s graph '%s'" % (kind, name))
        roots = [n for n, o in g.nodes.items() if o.cls in ("AnimGraphNode_Root", "AnimGraphNode_StateResult", "AnimGraphNode_SaveCachedPose", "AnimGraphNode_CustomTransitionResult")]
        seen = set()
        for r in sorted(roots, key=lambda n: (g.nodes[n].cls != "AnimGraphNode_SaveCachedPose", n)):
            g.pose_tree(r, indent + 1, out, seen)
        # state machines nested
        for n, o in g.nodes.items():
            if o.cls == "AnimGraphNode_StateMachine":
                for p2, o2 in objs.items():
                    if p2.rsplit(".", 1)[0] == o.path and A.graph_kind(o2) == "statemachine" if o2.cls.endswith("Graph") and A.prop(o2, "Schema") else False:
                        dump_sm(objs, p2, out, indent + 1, n)
    elif kind == "k2":
        out.append(pad + "== k2 graph '%s'" % name)
        for n, o in g.nodes.items():
            line = "%s %s" % (short_cls(o.cls), n)
            if o.cls.startswith("K2Node_Call"):
                args = []
                for p in g.pins[n]:
                    if not p.out and not p.exec and not p.hidden and p.name not in ("self",):
                        if p.links:
                            src = g.source_of(n, p)
                            args.append("%s={%s}" % (p.name, g.expr(src) if src else "?"))
                        else:
                            v = p.defobj if p.defobj and p.defobj != "None" else p.default
                            args.append("%s=%s" % (p.name, v))
                line += "  " + A.func_ref(o) + "(" + ", ".join(args) + ")"
            elif o.cls in ("K2Node_Event", "K2Node_CustomEvent"):
                line += "  " + (A.prop(o, "CustomFunctionName") or A.func_ref(o))
            out.append(pad + "  " + line)


def rule_text(objs, gpath):
    g = Graph(objs, gpath)
    res = [n for n, o in g.nodes.items() if o.cls == "AnimGraphNode_TransitionResult"]
    if not res:
        return "?"
    n = res[0]
    b = bindings(objs, g.nodes[n])
    for pin in g.pins[n]:
        if pin.name == "bCanEnterTransition":
            if "bCanEnterTransition" in b:
                return b["bCanEnterTransition"]
            if pin.links:
                src = g.source_of(n, pin)
                return g.expr(src) if src else "?"
            return "const " + str(pin.default)
    return "?"


def dump_sm(objs, smpath, out, indent, owner):
    g = Graph(objs, smpath)
    pad = "  " * indent
    out.append(pad + "== state machine '%s' (node %s)" % (smpath.rsplit(".", 1)[-1], owner))
    names = {}
    for n, o in g.nodes.items():
        bg = A.prop(o, "BoundGraph")
        if bg:
            m = re.search(r"'([^']+)'", bg)
            names[n] = m.group(1) if m else n
        elif o.cls == "AnimStateAliasNode":
            names[n] = (A.prop(o, "StateAliasName") or n).strip('"')
        elif o.cls == "AnimStateEntryNode":
            names[n] = "<entry>"
    # entry
    for n, o in g.nodes.items():
        if o.cls == "AnimStateEntryNode":
            for pin in g.pins[n]:
                for ln, _ in pin.links:
                    out.append(pad + "  entry -> " + names.get(ln, ln))
    for n, o in g.nodes.items():
        if o.cls in ("AnimStateNode", "AnimStateConduitNode", "AnimStateAliasNode"):
            extra = []
            for key in ("StateEntryFunction", "StateExitFunction", "StateFullyBlendedFunction", "StateEntered", "StateLeft", "StateFullyBlended", "bAlwaysResetOnEntry", "bGlobalAlias", "AliasedStates"):
                v = A.prop(o, key)
                if v:
                    extra.append("%s=%s" % (key, v[:160]))
            out.append(pad + "  %s %s  %s" % (o.cls.replace("AnimState", "").replace("Node", "") or "State", names.get(n, n), " | ".join(extra)))
    for n, o in g.nodes.items():
        if o.cls != "AnimStateTransitionNode":
            continue
        frm = to = "?"
        for pin in g.pins[n]:
            if pin.links:
                if pin.out:
                    to = names.get(pin.links[0][0], pin.links[0][0])
                else:
                    frm = names.get(pin.links[0][0], pin.links[0][0])
        props = []
        for key in ("CrossfadeDuration", "BlendMode", "LogicType", "PriorityOrder", "Bidirectional", "bAutomaticRuleBasedOnSequencePlayerInState", "SharedRulesName", "bSharedRules", "BlendProfile", "bAllowInertializationForSelfTransitions"):
            v = A.prop(o, key)
            if v:
                props.append("%s=%s" % (key.replace("bAutomaticRuleBasedOnSequencePlayerInState", "auto"), v))
        bg = A.prop(o, "BoundGraph")
        rule = "?"
        if bg:
            m = re.search(r"'([^']+)'", bg)
            gp = o.path + "." + m.group(1) if m else None
            if gp and gp in objs:
                rule = rule_text(objs, gp)
        out.append(pad + "  %s -> %s  when %s  [%s]" % (frm, to, rule, ", ".join(props)))
    # dump each state's graph
    for n, o in g.nodes.items():
        if o.cls in ("AnimStateNode", "AnimStateConduitNode"):
            bg = A.prop(o, "BoundGraph")
            m = re.search(r"'([^']+)'", bg or "")
            if m:
                gp = o.path + "." + m.group(1)
                if gp in objs and A.graph_kind(objs[gp]) == "state":
                    dump_graph(objs, gp, out, indent + 1)


def main():
    fn = sys.argv[1]
    objs = A.parse(fn)
    bp = [o for o in objs.values() if ":" not in o.path and o.cls.endswith("Blueprint")][0]
    allowed = []
    for p in bp.props:
        m = A.RE_GRAPHREF.match(p)
        if m:
            allowed.append(m.group(2))
        if p.startswith("ImplementedInterfaces("):
            for m in re.finditer(r"AnimationGraph'([^']+)'", p):
                allowed.append(m.group(1))
    objs = {p: o for p, o in objs.items() if ":" not in p or (A.local(p) and A.local(p)[0] in allowed)}
    out = []
    out.append("asset: " + bp.path)
    for key in ("ParentClass", "TargetSkeleton", "ImplementedInterfaces", "bUseMultiThreadedAnimationUpdate", "BlueprintType"):
        for p in bp.props:
            if p.startswith(key):
                out.append("  " + p[:200])
    root = bp.path.split(".")[0] + "." + bp.path.split(".")[-1]
    for gname in allowed:
        gpath = bp.path + ":" + gname
        if gpath in objs:
            dump_graph(objs, gpath, out)
    sys.stdout.reconfigure(encoding="utf-8")
    print("\n".join(out))


if __name__ == "__main__":
    main()
