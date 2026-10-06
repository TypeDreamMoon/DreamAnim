# DreamAnim

Animation Blueprints authored as text.

- `.dab` -- an Animation Blueprint: its anim graph, state machines, cached poses, linked graphs and layers.
- `.dai` -- an Animation Layer Interface.
- `.dat` -- a library of pose templates, expanded at compile time.

All logic stays in the C++ anim instance the Blueprint derives from. A file only reads C++ state
(property paths) and calls C++ functions; the generator writes whatever Blueprint glue that needs.
The text is the source of truth, and edits made in the Animation Blueprint editor are written back to it.

Status: early development, Unreal Engine 5.8 only.

## Layout

| Path | What |
|---|---|
| `Source/DreamAnim` | Runtime module |
| `Source/DreamAnimTests` | Automation tests (UncookedOnly, so they also load under `-game`) |
| `Tools/dab.ps1` | Builds the test host and runs the automation tests |
| `Tools/TestHost` | Template of the host project the tests run in |
| `Tools/Survey` | Scripts that export Animation Blueprints to T3D and summarise them |
