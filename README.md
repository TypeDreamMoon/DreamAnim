# DreamAnim

Animation Blueprints written as text, for Unreal Engine 5.8.

All logic stays in the C++ anim instance a Blueprint derives from. The anim graph, its state machines,
cached poses, layers and linked graphs are written in `.dab` files. A file only reads C++ state and calls
C++ functions; the build writes whatever Blueprint glue that needs. The text is the source of truth: a
build turns it into an ordinary Animation Blueprint, and each later build replaces that Blueprint's
graphs with what the text says.

| File | Document | Builds |
|---|---|---|
| `.dab` | `AnimBlueprint(...)` | an Animation Blueprint |
| `.dai` | `AnimLayerInterface(...)` | an Animation Layer Interface |
| `.dat` | `AnimTemplates(...)` | nothing; a library of templates other files include |

## Example

ALS-Refactored's head Blueprint, complete:

```dab
AnimBlueprint(Name="ALS/Character/AnimationInstances/AB_Als_Head", Root="Plugin.ALS")
{
    Settings = {
        Parent                                = AlsLinkedAnimationInstance;
        Skeleton                              = SK_Als;
        bEnableLinkedAnimLayerInstanceSharing = true;
    }

    Aliases = { Main = Self.GetParent(); }

    Graph = {
        Additive = {
            BlendSpaceEvaluator(
                BlendSpace     = BS_Als_Look,
                NormalizedTime = Main.HeadState.YawAmount,
                X              = Main.HeadState.PitchAngle
            );
            Call(Self.RefreshHead);
            Call(Self.InitializeHead, On = BecomeRelevant);
        };

        Output = ApplyMeshSpaceAdditive(
            AlphaBoolBlend = (BlendOption = Linear),
            Alpha          = Main.ViewState.HeadBlendAmount,
            Base           = Input("View Input"),
            Additive       = Additive
        );
    }
}
```

`Main.HeadState.YawAmount` binds a pin to C++ state. `Call(...)` calls a C++ function while the graph
runs. `Additive` is a named pose: a stack of three nodes.

## Status

Measured by the automated tests against ALS-Refactored:

- All 26 of its Animation Blueprints (25 Blueprints and the layer interface) decompile, check, and
  regenerate to the same model.
- A character built entirely from regenerated Blueprints moves bone for bone like the original over a
  420-frame script: every bone, every frame, within 0.01 cm and 0.01 degrees.
- Animation sets swap at run time. A running instance takes the new set's animations at its next update,
  and gives the first pose again when swapped back.

Also in place: the canonical formatter, templates and `#include`, and coded diagnostics with hints.

- In the editor, a saved source rebuilds its Blueprint, and a change made to the Blueprint is written
  back into its source: values in place, structure by rewriting the block it is in. When both changed,
  the editor lists the conflict and merges on request. Node positions survive rebuilds. See
  [WriteBack.md](Docs/WriteBack.md).

Not yet:

- **The VS Code extension is built but not published.** It gives highlighting, an outline, navigation
  across includes, diagnostics and formatting through `dab.ps1`.

The plugin is beta (version 0.1.0).

## Requirements

- Unreal Engine 5.8.
- Windows and PowerShell for `Tools/dab.ps1`.
- The plugin is not enabled by default. Enable it in the project's `.uproject`. It enables
  `PropertyAccessEditor`, which the generated bindings use.

## Modules

| Module | Type | What it holds |
|---|---|---|
| `DreamAnim` | Runtime | Animation sets (`UDreamAnimSet`) and `UDreamAnimLibrary`, the run-time support generated Blueprints call. The only module a packaged game loads. |
| `DreamAnimLang` | UncookedOnly | Lexer, parser, canonical printer, `#include` and template expansion, the model. |
| `DreamAnimCompiler` | UncookedOnly | The checker, the generator, the decompiler, write-back, the driver the command line and the editor share, and the `-run=DreamAnim` commandlet. |
| `DreamAnimEditor` | Editor | Keeping Blueprints and their sources in step (rebuild on save, write-back, conflicts), the Content Browser and Tools menu entries, the Dream Anim message log, and the Dream Anim Set asset type with its slot editing. |
| `DreamAnimTests` | UncookedOnly | Automation tests, including the ALS-Refactored round trip. UncookedOnly so they also load under `-game`. |

## Quick start

Sources live in the project's `DAnim/` folder, or in `DAnim/` of an enabled plugin.

1. Write `DAnim/Characters/AB_Hero.dab`. The names below are from the UE5 third-person template; use
   your own skeleton and animation:

   ```dab
   AnimBlueprint(Name="Characters/AB_Hero", Root="Game")
   {
       Settings = {
           Parent   = AnimInstance;
           Skeleton = SK_Mannequin;
       }

       Graph = { Output = SequencePlayer(Sequence = MM_Idle); }
   }
   ```

2. Check it, then build it into `/Game/Characters/AB_Hero`:

   ```
   Tools/dab.ps1 check -Project D:/MyGame/MyGame.uproject
   Tools/dab.ps1 build -Project D:/MyGame/MyGame.uproject -File DAnim/Characters/AB_Hero.dab
   ```

   With the editor open you can skip the command line: saving the file rebuilds the Blueprint, and
   what you change in the Blueprint is written back into the file ([WriteBack.md](Docs/WriteBack.md)).

3. Turn an existing Blueprint into text, with **Write as DreamAnim Text** on it in the Content Browser or
   from the command line. The file lands in `DAnim/`, mirroring the package path:

   ```
   Tools/dab.ps1 decompile -Project D:/MyGame/MyGame.uproject -Asset /Game/Characters/ABP_Hero
   ```

   Read the comment at the top of the result: it lists anything the text cannot hold, such as event
   graph logic, which belongs in C++. See [Decompiling.md](Docs/Decompiling.md) before rebuilding a
   decompiled Blueprint in place.

## Command line

`Tools/dab.ps1 <command> [options]`. `make` builds C++; `build` compiles text into assets.

| Command | What it does |
|---|---|
| `make` | Builds the test host's editor target: the plugin's C++. |
| `test [-Preset P] [-Filter F] [-NoBuild]` | Runs `make`, then the automation tests headless in the test host. Presets: `Quick` (the default: language, checker corpus, driver, write-back, editor), `RoundTrip` (every ALS Blueprint decompiled, checked, generated and compared, also with an animation set), `Behavior` (the ALS character and generated Blueprints run, poses compared), `All`. `-Filter` replaces the preset's tests. |
| `build [-File F] [-Root R] [-NoSave] [-Json J]` | Compiles the `.dab` and `.dai` sources into Animation Blueprint assets and saves them. `-NoSave` keeps them in memory only. |
| `check [-File F] [-Root R] [-Json J]` | Parses and checks: includes, templates, names. Nothing is generated. |
| `fmt [-File F] [-Root R] [-Write \| -Verify] [-Json J]` | Prints the canonical text. `-Write` rewrites the files that differ, keeping their line ends. `-Verify` writes nothing and fails on any file that is not canonical. |
| `decompile (-Asset A[,B] \| -Under /Game/Folder) [-AnimSet S] [-Out O] [-Stdout] [-Force] [-Json J]` | Animation Blueprints to text, under the project's `DAnim/` unless `-Out`. `-Stdout` prints instead. A file that exists and differs is replaced only with `-Force`. `-AnimSet` moves their animations into slots of that animation set. |

- `build`, `check`, `fmt` and `decompile` run `UnrealEditor-Cmd <project> -run=DreamAnim`. The project
  is `-Project`, else the project above `-File`, else the plugin's test host. `make` and `test` use the
  test host unless `-Project` is given.
- Without `-File`, these commands cover every source under the source roots: the project's `DAnim/`
  and the `DAnim/` of every enabled plugin. `-Root` replaces those roots. `-File` is absolute, relative
  to the current folder, or relative to the project. A `.dat` given as `-File` stands for the documents
  that include it.
- `build` orders the sources: interfaces first, and every document after the DreamAnim assets it names.
- `#include` resolves against the including file's folder first, then the source roots.
- Diagnostics print as `<file>(<line>,<col>): error DABnnnn: message`, with an indented `hint:` line when
  there is one. `-Json` writes them all as JSON. `-Out` and `-Json` are relative to the project.
- A build writes packages. Close any editor that has the project open; the script warns when one does.
- The engine log of each run is kept in `<project>/Saved/DreamAnim/`. `-Raw` shows the whole log.

| Exit code | `build`, `check`, `fmt`, `decompile` | `test` | `make` |
|---|---|---|---|
| 0 | no errors | green | built |
| 1 | errors (for `fmt -Verify`, a file not canonical) | red: a test failed or an ensure fired | |
| 2 | the run did not finish: a crash, a timeout, no summary | not judged: the build failed, the editor crashed or timed out, or too few tests ran | failed |

Test results land in `<host>/Saved/DreamAnimTests/<time>-<preset>/`: the engine's `index.json`, the run
log and `summary.md`.

## Documentation

| Page | Contents |
|---|---|
| [Language.md](Docs/Language.md) | Writing `.dab` files: every section, node, state machine rule and value. |
| [AnimSet.md](Docs/AnimSet.md) | Animation sets: slots, the set asset, swapping at run time. |
| [Decompiling.md](Docs/Decompiling.md) | Turning existing Blueprints into text, and migrating a character. |
| [WriteBack.md](Docs/WriteBack.md) | Working in the editor: rebuilds, write-back, conflicts, node positions, settings. |
| [Diagnostics.md](Docs/Diagnostics.md) | Every `DABnnnn` code and how to fix it. |
| [Grammar.md](Docs/Grammar.md) | The formal grammar and the canonical format. |
| [Mapping.md](Docs/Mapping.md) | How the text maps onto the Blueprint model. |

## Layout

| Path | What |
|---|---|
| `Source/` | The five modules. |
| `Docs/` | The pages above. |
| `Tests/Corpus/` | Parser, formatter and template cases, and the decompiled ALS-Refactored Blueprints the tests compare against. |
| `Tools/dab.ps1` | The command line. |
| `Tools/TestHost/` | Creates the host project the tests run in. |
| `Tools/Survey/` | Scripts that export Animation Blueprints to T3D and summarise them. |
