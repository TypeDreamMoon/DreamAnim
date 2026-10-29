# Writing tools for DreamAnim sources

For editors, highlighters, node graph views and other tools that read or write `.dab`, `.dai`, `.dat` and
`.dcr` files: which grammar a file is written in, what the languages know, and how to check a text from a
running editor without starting another one.

- [The grammar version](#the-grammar-version)
- [The vocabulary](#the-vocabulary)
- [Checking from a running editor](#checking-from-a-running-editor)

## The grammar version

[Grammar.md](Grammar.md) states the grammar version, and [Changelog.md](Changelog.md) says what each version
changed: the syntax, what a file builds, what write-back writes, how node GUIDs are made. A tool that parses
or writes the text records the version it follows; when the plugin's is higher, the changelog lists what to
catch up with. The vocabulary below reports it as `grammarVersion`.

## The vocabulary

```
Tools/dab.ps1 vocab -Out Saved/DreamAnim/vocabulary.json
Tools/dab.ps1 vocab -Out Saved/DreamAnim/vocabulary.json -RigUnits
Tools/dab.ps1 vocab -Stdout
```

`vocab` writes what the languages know as one JSON object: the words of each kind of document, and every
anim node type with its pins, read from the engine and the project's plugins the way the checker reads
them. `-RigUnits` adds the RigVM units and dispatches a `.dcr` can call. `-Out` is relative to the project;
`-Stdout` prints the text instead. It runs the project's editor headless like the other commands, so
the node types are those of the plugins the project enables.

| Field | What it is |
|---|---|
| `format` | `1`, the layout of this object |
| `grammarVersion` | the grammar version |
| `pluginVersion`, `engineVersion` | the plugin's version name and the engine's version |
| `documents` | one entry for each kind of document |
| `animNodes` | every anim node type, sorted by name |
| `rigUnits` | with `-RigUnits`: every unit and dispatch, sorted by struct name |

### documents

`{ "kind": "AnimBlueprint", "extension": ".dab", "words": [ ... ] }`, for `AnimBlueprint`,
`AnimLayerInterface`, `AnimTemplates` and `ControlRig`. Each word is
`{ "text", "role", "context", "summary" }`:

| Role | Words |
|---|---|
| `document` | the word of the header: `AnimBlueprint`, `ControlRig`, ... |
| `headerArgument` | `Name`, `Root` |
| `section` | `Name = { ... }` at the document level: `Settings`, `Graph`, `Hierarchy`, ... |
| `block` | a word that starts a named block: `StateMachine`, `State`, `Event`, `Bone`, ... |
| `key` | a key with a fixed meaning in a block: `Output`, `Entry`, a state's `Pose`, an event's `Forward` |
| `statement` | a word that starts a statement of `.dcr` code: `let`, `if`, `for`, `in`, ... |
| `value` | a name with a fixed meaning as a value: `true`, `None`, `Self`, `Auto`, ... |
| `function` | a function of expressions: `Clamp`, `StateWeight`, `Path`, ... |
| `builtin` | a call that makes a node of its own kind: `Input`, `Slot`, `Call`, `Injected`, `RunEvent` |
| `setting` | a key of `Settings`: `Parent`, `Skeleton`, `PreviewMesh`, ... |
| `metadata` | a metadata key with a fixed meaning: `Cached`, a transition's `Blend`, a function's `Public` |
| `directive` | `#include`, `#Region`, `#EndRegion` |

`context` says where the word has that meaning (`document`, `StateMachine`, `State`, `code`, ...). A word may
have more than one role. Words whose meaning comes from the engine (node types, node fields, class settings,
state machine and transition properties, Control Rig units) are not in `documents`: the node types are in
`animNodes` and `rigUnits`, the rest are the engine's properties of the same name.

### animNodes

| Field | What it is |
|---|---|
| `name` | the node type as the text writes it: `SequencePlayer`, `GameplayTagsBlend` |
| `class`, `struct` | the editor node class and its runtime node struct, by path |
| `writtenAs` | for the few node types the text writes otherwise: `Output = Pose;` for the root, `Input(Name, ...)`, `Call(...)`, a state machine's name, a cached pose |
| `deprecated` | the class is deprecated |
| `sink` | the node has no output pose (the graph's result) |
| `outputSpace` | `local` or `component`: the space of the output pose |
| `keyedBy` | `enum` or `tag`: `BlendPose` (and a per-pose array) may be a block keyed by enum entry or by tag |
| `inputsByName` | the node's poses are named after its target's input poses: `Inputs = { Name = Pose; }` |
| `customPins` | the node also takes pins of another class (a linked instance's variables, a rig's inputs) |
| `pins` | the node's pins, in field order |

A pin is one of these:

| `kind` | Fields |
|---|---|
| `pose` | `name`, `space` (`local` or `component`) |
| `poseArray` | `name`, `space`, `editorPins`: the editor's pin names, `BlendPose_{index}`. The text writes the poses as a list, `BlendPose = [ P0, P1 ]` |
| `value` | a field of the runtime node: see below |
| `setting` | a setting of the editor node (`Tag`, `BoundEnum`): see below |

A `value` or `setting` pin has:

| Field | What it is |
|---|---|
| `name` | the argument's name |
| `type` | the type as the text spells it: `float`, `Vector`, `AnimSequenceBase`, `TArray<float>` |
| `cppType` | the C++ type |
| `array` | an array |
| `perPose` | an array with one entry per pose of the node's pose array; a single value stands for every pose (`BlendTime = 0`) |
| `pin` | how the editor shows it: `always`, `shown`, `hidden` (a pin, shown or hidden by default), `never` or `none` (a detail only) |
| `bindable` | it can be bound to an expression (`pin` is `always`, `shown` or `hidden`); otherwise it takes a constant |
| `editorOnly` | the field exists in the editor only; the compiler folds it into the class |
| `category`, `owner`, `tooltip` | the field's category, the struct or class that declares it, its tooltip |
| `default` | the node's default as the text writes a value (`1`, `Linear`, `(Axis = (Y = 1))`, `[ 0.1 ]`); left out when the text cannot write it |
| `enum`, `values` | for an enum: its name and the entries the text writes values by |

### rigUnits

| Field | What it is |
|---|---|
| `name` | the unit or dispatch as `.dcr` code calls it: `GetTransform`, `If`, `AlsRigVMFunction_IsGameWorld` |
| `struct` | the struct, by path |
| `kind` | `unit` or `dispatch` |
| `deprecated` | the struct is deprecated; a current unit of the same name hides it |
| `displayName`, `category`, `keywords` | the editor's name, category and keywords |
| `mutable` | the node runs: it has execute pins and is a statement of its own |
| `executeIn`, `executeOut` | its execute pins, by name |
| `continuesBy` | the execute output the statements after the call run from |
| `blocks` | the other execute outputs: the blocks a call can have, `IsGameWorld() { True { ... } }` |
| `pins` | `{ name, direction (input, output or io), type, cppType, wildcard, aggregate, default }`; a dispatch's `wildcard` pin takes a type the call picks, `default` is the engine's export text |

## Checking from a running editor

`dab.ps1 check` starts the editor headless, which takes a while. A tool that talks to a running editor (for
example over a Python bridge) can ask the editor to check a text instead:

```python
import unreal
subsystem = unreal.get_editor_subsystem(unreal.DreamAnimEditorSubsystem)
diagnostics = subsystem.check_source("DAnim/Characters/AB_Hero.dab", text)
```

`CheckSource(FilePath, Text)` (`UDreamAnimEditorSubsystem`, BlueprintCallable) checks as `dab.ps1 check` does:
parse, includes, templates and names, for every kind of source; nothing is built and nothing is saved.

- `Text` is the source to check, an unsaved buffer for instance. Empty reads `FilePath` from disk.
- `FilePath` names the source in the diagnostics and resolves its includes. A relative path is relative to
  the project.
- A library (`.dat`) read from disk is checked through the documents that include it, as `dab.ps1 check`
  does; given as `Text`, it is checked on its own.
- Bare asset names resolve as in a build: against the asset registry, and against the assets other sources
  make.

It returns the diagnostics as the JSON `-Json` writes, `[]` when the source checks clean:

```json
[{"file":"I:/Project/DAnim/Characters/AB_Hero.dab", "line":3, "column":24, "length":10, "severity":"error",
  "code":"DAB3001", "message":"There is no anim node type `NoSuchNode`.", "hint":"..."}]
```

Line and column are 1-based; `length` counts characters from there. `severity` is `error`, `warning` or
`info`. The codes are in [Diagnostics.md](Diagnostics.md).
