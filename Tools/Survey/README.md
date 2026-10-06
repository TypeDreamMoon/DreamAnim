# Survey tools

Scripts that export Animation Blueprints to T3D text and summarise what is in them. They were used to
survey ALS-Refactored before the language was designed, and stay here as a cross-check for the
decompiler: anything the decompiler reads from a graph can be compared against the T3D dump.

| Script | Runs in | Does |
|---|---|---|
| `export_t3d.py` | the editor's Python (`-run=pythonscript -script=…`) | Exports every Animation Blueprint and Control Rig under `DREAMANIM_T3D_ROOT` (default `/ALS`) to `DREAMANIM_T3D_OUT` (default `<project>/Saved/DreamAnimSurvey/t3d`) |
| `analyze_t3d.py <dir>` | plain Python 3 | Counts graphs, nodes, bindings, transitions per asset; writes `_analysis.json` |
| `report_t3d.py <dir>` | plain Python 3 | Prints the summary tables from `_analysis.json` |
| `dump_tree.py <file.t3d>` | plain Python 3 | Prints one Blueprint as a pose tree and state machine outline |

Things to know about the T3D:

- It is UTF-16 with a BOM; the scripts detect that.
- Every object appears twice: a declaration (`Begin Object Class=…`) and a definition with the properties.
- Loading a Blueprint compiles it, and the compiler's intermediate graphs are exported too
  (`ExecuteUbergraph_*`, `*_MERGED`, `AnimGraph__AnimFunc`, `AnimNode_CallFunction_EventStub*`). Only
  the graphs the Blueprint lists in `UbergraphPages` / `FunctionGraphs` / `ImplementedInterfaces` are
  authored; the scripts keep those and their nested graphs.
- A state's entry and exit functions are member references on `UAnimGraphNode_StateResult`; the copy
  inside `FAnimNode_StateResult` can hold stale names.
- `analyze_t3d.py` skips `CR_Als` and `report_t3d.py` skips `AB_Als_Monolithic`, both specific to ALS.
