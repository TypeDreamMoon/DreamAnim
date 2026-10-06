import json, collections, os, sys

D = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith("--") else os.environ.get("DREAMANIM_T3D_DIR", ".")
R = json.load(open(os.path.join(D, "_analysis.json"), encoding="utf-8"))
SKIP_MONO = "--mono" not in sys.argv
rs = [r for r in R if not (SKIP_MONO and r["asset"] == "AB_Als_Monolithic")]

def tot(c):
    return sum(c.values())

print("== per asset (Monolithic excluded from totals: %s)" % SKIP_MONO)
print("%-26s %-28s %4s %4s %4s %4s %4s | %5s %5s %5s %5s %5s | %4s %4s %4s" % (
    "asset", "parent", "anim", "sm", "st", "tr", "fn", "animN", "smN", "k2Anim", "k2Tr", "k2Fn", "bind", "nfun", "evt"))
for r in R:
    g = r["graphs"]; n = r["nodes_by_graphkind"]
    sm_nodes = sum(v for k, v in n.get("statemachine", {}).items())
    animN = sum(v for k, v in n.get("anim", {}).items() if k.startswith("AnimGraphNode")) + sum(v for k, v in n.get("state", {}).items() if k.startswith("AnimGraphNode"))
    k2anim = tot(r["k2_in_anim"])
    k2tr = sum(v for kind in ("transition", "conduit", "customtransition") for k, v in n.get(kind, {}).items() if k.startswith("K2Node"))
    k2fn = sum(v for k, v in n.get("k2", {}).items())
    print("%-26s %-28s %4d %4d %4d %4d %4d | %5d %5d %5d %5d %5d | %4d %4d %4d" % (
        r["asset"][:26], r["parent"][:28], g.get("anim", 0), g.get("statemachine", 0), g.get("state", 0),
        g.get("transition", 0) + g.get("conduit", 0), g.get("k2", 0), animN, sm_nodes, k2anim, k2tr, k2fn,
        tot(r["bindings"]), tot(r["node_funcs"]), tot(r["events"])))

def agg(key):
    c = collections.Counter()
    for r in rs:
        c.update(r[key])
    return c

print("\n== anim node classes (all graphs)")
for k, v in agg("anim_nodes").most_common():
    print("%5d  %s" % (v, k))

print("\n== K2 nodes inside AnimGraph/State graphs")
for k, v in agg("k2_in_anim").most_common():
    print("%5d  %s" % (v, k))

print("\n== node classes in K2 (event/function) graphs")
c = collections.Counter()
for r in rs:
    c.update(r["nodes_by_graphkind"].get("k2", {}))
for k, v in c.most_common():
    print("%5d  %s" % (v, k))

print("\n== events in EventGraph")
for k, v in agg("events").most_common():
    print("%5d  %s" % (v, k))

print("\n== function graphs (name: nodes, calls)")
fc = collections.Counter(); fsizes = collections.Counter()
for r in rs:
    for name, f in r["functions"].items():
        fc[name] += 1
        b = "1-2" if f["n"] <= 2 else "3-4" if f["n"] <= 4 else "5-9" if f["n"] <= 9 else "10+"
        fsizes[b] += 1
print("size buckets:", dict(fsizes))
for r in rs:
    for name, f in r["functions"].items():
        if f["n"] >= 5 or name == "EventGraph":
            print("  %-22s %-34s n=%-3d calls=%s" % (r["asset"][:22], name[:34], f["n"], ",".join(sorted(set(f["calls"])))[:150]))

print("\n== anim node functions bound (OnInitialUpdate/OnBecomeRelevant/OnUpdate)")
for k, v in agg("node_funcs").most_common():
    print("%5d  %s" % (v, k))

print("\n== property access binding paths (top 60)")
b = agg("bindings")
print("distinct=%d total=%d" % (len(b), tot(b)))
for k, v in b.most_common(60):
    print("%5d  %s" % (v, k))

print("\n== state events (notify on enter/leave)")
print(dict(agg("state_events")))

print("\n== transitions")
T = [t for r in rs for t in r["transitions"]]
print("count=%d  auto=%d  custom_blend=%d" % (len(T), sum(t["auto"] for t in T), sum(t["custom_blend"] for t in T)))
sz = collections.Counter()
for t in T:
    n = t["n"]
    sz["1(result only)" if n <= 1 else "2" if n == 2 else "3-4" if n <= 4 else "5-8" if n <= 8 else "9+"] += 1
print("node-count buckets:", dict(sz))
cls = collections.Counter()
for t in T:
    cls.update(t["classes"])
for k, v in cls.most_common():
    print("%5d  %s" % (v, k))
calls = collections.Counter()
for t in T:
    calls.update(t["calls"])
print("-- calls in transitions")
for k, v in calls.most_common(40):
    print("%5d  %s" % (v, k))
vars_ = collections.Counter()
for t in T:
    vars_.update(t["vars"])
print("-- vars/property access in transitions (top 30)")
for k, v in vars_.most_common(30):
    print("%5d  %s" % (v, k))
print("-- biggest transitions")
for r in rs:
    for t in r["transitions"]:
        if t["n"] >= 8:
            print("  %-22s n=%d calls=%s vars=%s" % (r["asset"][:22], t["n"], t["calls"][:8], t["vars"][:8]))
