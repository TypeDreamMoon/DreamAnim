# Text and model

How a `.dab` file maps onto the model (`Source/DreamAnimLang/Public/Model/DabModel.h`). The semantic
pass applies these rules from text to model; the lowering pass (the second half of the decompiler)
applies them backwards. The two must be inverses: lowering a model and checking the result gives the
same model back (`DumpDabModel` equal), up to the name normalisation described at the end.

Syntax and diagnostics of the front end are in `Grammar.md`. This file adds codes DAB3001–DAB6099.

## Document

| Text | Model |
|---|---|
| `AnimBlueprint(Name="A/B", Root=R)` | `Kind = AnimBlueprint`, `AssetPath = /<mount>/A/B.B` where `Root="Game"` → `/Game/`, `Root="Plugin.X"` → `/X/`, `Root="Engine"` → `/Engine/` (no Root means Game) |
| `AnimLayerInterface(...)` | `Kind = AnimLayerInterface`; every `Layer X [meta];` → a declaration-only layer with the metadata as its graph's ResultProperties |
| `Settings = { Parent = C; }` | `ParentClass`: a native class by name (`AlsAnimationInstance`, with or without the `U`) or `Path(...)` to an Animation Blueprint. Must derive from UAnimInstance (DAB3003) |
| `Skeleton = Path(...)` | `Skeleton` (object path) |
| `Implements = [ I, ... ]` | `Interfaces`: a DreamAnim interface by asset name, or `Path(...)` |
| `DefaultAnimSet = Path(...)` | `DefaultAnimSet` |
| any other `Settings` key | `ClassSettings` (a UAnimBlueprint property name, DAB4001 if it has none) |
| `Defaults = { Settings = DA_Als; }` | `ClassDefaults`: editable properties of the parent class that this Blueprint's class defaults change (DAB4014 when the parent has no such property) |
| `Aliases = { Main = Self.GetParent(); }` | nothing; every later `Main.X` is replaced by the expansion before it reaches the model |
| `Properties = { float X = 0.3 [ Group="G" ]; }` | `Variables` |
| `Assets = { AnimSequence X; }` | `AssetSlots` |
| `Notifies = { X = Call; }` or `X = { Call; Call; };` | `Notifies` (name without `AnimNotify_`) |
| `Graph = { ...; Output = Pose; }` | `AnimGraph`; `Output` is the result |
| `Inputs = [ "Standing Input", Crouching ];` in `Graph` or a layer | the graph's `InputPoses` in that order (a linked graph node feeds its pins to them by position); inputs not listed follow in the order `Input(...)` first appears |
| `Layer X = Pose;` / `Layer X [meta] { ...; Output = Pose; }` | a layer; metadata → its graph's ResultProperties |
| `StateMachine X [meta] { ... }` | a machine; metadata → its Properties |
| `Template ...` | nothing (expanded before this pass) |

## Poses

A pose expression becomes nodes and pose links in the graph it is written in.

| Text | Model node |
|---|---|
| `Type(args)` | a node whose class is `AnimGraphNode_<Type>`, or any loaded UAnimGraphNode_Base subclass named `<Prefix>AnimGraphNode_<Type>` (DAB3002 when two match: write the full class name `AlsAnimGraphNode_GameplayTagsBlend`); DAB3001 when none |
| `Input(Name)` | AnimGraphNode_LinkedInputPose, InputPoseName |
| `Slot("Name", ...)` | AnimGraphNode_Slot, property `SlotName`; further named arguments are ordinary properties |
| `Call(Target.Function, On = Site)` | AnimGraphNode_CallFunction, Call; `On` → property `CallSite = On<Site>` (`Update` is the default and is not recorded) |
| a state machine's name | AnimGraphNode_StateMachine, StateMachineName (each machine referenced exactly once, DAB6002) |
| a named pose marked `[ Cached ]` | at its definition an AnimGraphNode_SaveCachedPose (CacheName = the name) fed by the pose; at every use an AnimGraphNode_UseCachedPose |
| a named pose without `[ Cached ]` | its nodes, placed at its single use (a second use is DAB6005; a named pose never used still makes its nodes, unlinked, with warning DAB6004) |
| `{ First; Step; Step; }` | First, then each Step's only pose input fed by the previous one (DAB6001 when a step has no or several pose inputs) |

### Arguments of a node

Resolved against the runtime struct of the node (the editor node's `Node` property) and then the editor node.

| Argument | Model |
|---|---|
| a pose pin (`A = Pose`) | PoseInputs `A` |
| an array of pose pins (`Poses = [ P0, P1 ]`) | PoseInputs `Poses_0`, `Poses_1` |
| a pose array keyed by enum or tag (`BlendPose = { Default = P; LeftBackward = Q; }`) | BlendListByEnum: `BoundEnum = EnumName` names the enum (an editor-node property; it may be left out when `ActiveEnumValue` is bound to a path of that enum type); `Default` is pin 0, each enum entry gets the next pin in written order, and the editor node's `VisibleEnumEntries` (full `EEnum::Entry` names) is recorded as a property. `BoundEnum` is recorded as the enum's `/Script/...` path. (`EnumToPoseIndex` is filled only in the compiler's copy of the node and is not recorded.) GameplayTagsBlend: `Default` is pin 0, each tag in written order → pin 1.., and property `Tags` |
| a constant (`PlayRate = 1.5`, `Method = SyncGroup`, `Path(...)`, a tuple) | Properties: a struct as a named tuple of its non-default fields in declaration order, an array as one list (`X = [...]`); a value equal to the default is not recorded |
| a path into C++ (`Self.X`, an alias), an asset slot (`Assets.X`), a tuning value (`Props.X`) | Bindings (pin = the argument name) |
| any other expression | Bindings with the expression (compiled to a generated thread-safe function) |
| `Inputs = { Name = Pose; }` on LinkedAnimGraph / LinkedAnimLayer | PoseInputs by the target's input pose names |
| `Tag = "X"` | the editor node's `Tag` |

Conveniences, applied on the way in and recovered on the way out:

- `ExplicitFrame = N` on SequenceEvaluator also sets `bUseExplicitFrame = true` (recorded explicitly in the model).
- A per-pose array (`BlendTime`, `DesiredAlphas` …) given a single value gets that value for every pose pin of the node.
- A per-pose array given a block keyed like `BlendPose` (`BlendTime = { Default = 0; RightBackward = 0.1; }`) maps the same keys to the same indices.
- `ModifyCurve(Name = Value, ...)`: an argument that is not a property of the struct is a curve: `CurveNames` gets the name, `CurveValues` the value (a constant into the property, anything else a binding on `CurveValues_<i>`).
- `bones(a, b, "c", Depth = n)` inside `LayerSetup = [ ... ]` is one `FInputBlendPose` whose `BranchFilters` are `(BoneName = a, BlendDepth = n)` for each bone.

### Node metadata

On the statement that names a pose (`X = Node(...) [ meta ];`), applied to the pose's root node:
`Cached` (above), `OnInitialUpdate` / `OnBecomeRelevant` / `OnUpdate = Call` → NodeFunctions.

## State machines

| Text | Model |
|---|---|
| `Entry = S;` | Entry |
| `State S [meta] { ... }` | a state; metadata → Properties; `OnEnter` / `OnExit` / `OnBlended = Call` → the calls; named poses local to the state; `Pose = P;` → the graph's result (DAB6012 when missing) |
| `Conduit C = Condition;` / `Conduit C;` | a conduit (no condition = `true`) |
| `Alias A = [ S1, S2 ];` / `Alias A = All;` | an alias (`All` → bGlobal) |
| `Rules = { R = Condition; }` | shared rules, used below |
| `From -> To : Condition [ options ];` | a transition |

Transition condition: `Auto` → ConditionKind Auto; a bare rule name and nothing else → SharedRuleName = the rule and Condition = the rule's expression; any other expression → Condition, with rule names inside it replaced by their expressions.

Transition options: `Blend = s` → `CrossfadeDuration`; `Curve = W` → `BlendMode = W` (an EAlphaBlendOption name); `Inertialize` → `LogicType = TLT_Inertialization`, `Inertialize = s` also sets `CrossfadeDuration`; `Priority = n` → `PriorityOrder`; anything else → the transition node property of that name. Options equal to the engine default (CrossfadeDuration 0.2, BlendMode HermiteCubic, PriorityOrder 1, LogicType standard) are not recorded.

## Calls

`Target.Function(args)` where Target is `Self`, an alias, or a Self-rooted path. Arguments are constants, `Props.X` or `Assets.X` (DAB5002 otherwise). In the model the target is Self-rooted and the alias is gone.

## Values in the model

- Numbers through `FormatDabNumber`.
- Enum values as bare names (`Linear`, `SyncGroup`), validated against the property's enum (DAB4002).
- Gameplay tags as strings, validated against the tag table (DAB4003).
- Asset references as `Path(Root, "Rel")` with Root `Game`, `Engine` or `Plugin.<Mount>`; a Blueprint class by its Blueprint asset's Path.
- Structs as named tuples of their non-default fields; arrays as lists.

## Lowering (model → text)

The decompiler's choices, in order of preference:

1. `Aliases = { Main = Self.GetParent(); }` when any value of the model starts with `Self.GetParent()`; every such path then starts with `Main.`.
2. A SaveCachedPose becomes a named pose `<CacheName> = ... [ Cached ];` in the graph that holds it, uses become its name.
3. A chain of single-pose-input nodes becomes a stack `{ Source; Step; Step; }`.
4. Other nodes print inline as arguments. A pose argument that is a stack, or whose printed form would nest more than three calls deep, is pulled out into a named pose, named after the pin it feeds (`LocomotionInput` → `Locomotion`), else the node's `Tag`, else `<Type><n>`.
5. An asset whose name no other registered asset has is written by that name (`A_Als_Idle`) instead of `Path(...)`.
6. The conveniences above are recovered where the model allows it exactly (curve arguments, keyed blend maps, a single value for an array whose entries are all equal, `ExplicitFrame` without `bUseExplicitFrame`).
7. Machine, state, conduit, alias, rule, cache and layer names (and the `Layer` of linked layer nodes that name a layer of this Blueprint) become identifiers: characters other than letters, digits and `_` are dropped and the next letter is upper-cased (`Rotate Left` → `RotateLeft`, `(N) RB Movement` → `NRBMovement`); a clash gets `_2`. The original names are listed in a comment at the top of the file. Input-pose names are not renamed: a linked graph's pins are found by them at run time, so they are written as strings when they are not identifiers (`Input("Standing Input")`, `Inputs = { "Standing Input" = Standing; }`).

`NormalizeDabModelNames` applies rule 7 to a model; round-trip comparisons normalise the extracted model first.

## Diagnostics

Raised by the semantic pass (`Source/DreamAnimCompiler/Private/Semantic/`). Severity is Error unless marked.

| Code | Severity | Meaning |
|---|---|---|
| DAB3001 | Error | No anim node type of that name (or the class is abstract or has no runtime node) |
| DAB3002 | Error | A node type name that several node classes end in; write the full class name |
| DAB3003 | Error | `Parent` does not derive from AnimInstance (AnimLayerInterface for an interface) |
| DAB3004 | Error | No asset, Blueprint or class of that bare name |
| DAB3005 | Error | An asset slot's type is not an animation asset class |
| DAB3006 | Error | A malformed `Path(Root, "Rel")`, or a header `Root` that is not Game, Engine or Plugin.X |
| DAB3007 | Error | A bare asset name more than one registered asset has; write `Path(...)` |
| DAB3008 | Warning | A tuning value's type is not understood; its default is kept as written |
| DAB4001 | Error | A `Settings` key that is not a UAnimBlueprint class setting |
| DAB4002 | Error | Not a value of the enum (a property value, a BlendPose key, a BlendTime key), or no such enum |
| DAB4003 | Warning | A gameplay tag that is not registered |
| DAB4004 | Error | A node has no argument of that name, or the argument is compiler bookkeeping |
| DAB4005 | Error | Unknown metadata key or transition option; metadata where none is taken (Graph, asset slots, settings) |
| DAB4006 | Error | A value that does not fit the property's type |
| DAB4007 | Error | A struct has no such field (or more positional values than fields) |
| DAB4008 | Error | A positional argument where arguments are named |
| DAB4009 | Error | A builtin written wrong: `Input` without its name, `Call` without its function, `On` without a site, `bones(...)` outside LayerSetup or with a non-bone argument |
| DAB4010 | Error | A value read at run time where only a constant fits (metadata, options, settings, editor-node properties, defaults) |
| DAB4011 | Error | The same field, key, option or node function given twice, or a value given both directly and by the convenience that implies it |
| DAB4012 | Error | BlendListByEnum whose enum is neither written (`BoundEnum`) nor the type of the path bound to `ActiveEnumValue` |
| DAB4013 | Warning | `Inputs` names an input pose the linked graph or layer's target does not have |
| DAB4014 | Error | A `Defaults` entry that is not an editable property of the parent class |
| DAB5001 | Error | A call that is not `Target.Function(...)` with a Self-rooted target |
| DAB5002 | Error | A call argument that is computed (only constants, `Props.X` and `Assets.X`) |
| DAB5003 | Error | `Props.X` / `Assets.X` naming an undeclared tuning value or asset slot |
| DAB5004 | Warning | The call's target class has no such function or parameter |
| DAB5005 | Error | A block or a stack where a value is expected |
| DAB5006 | Error | `Props.X` used where only a constant fits, but X has no default |
| DAB5007 | Error | An alias that is not a path from Self, or that takes a reserved word |
| DAB6001 | Error | A stack step without exactly one free pose input (or whose linked target cannot be loaded to tell) |
| DAB6002 | Error | A state machine used more than once, or never |
| DAB6003 | Error | A name in a pose that is no named pose, cache or state machine visible there |
| DAB6004 | Warning | A named pose never used (its nodes are still made, unlinked) |
| DAB6005 | Error | A named pose without `[ Cached ]` used more than once |
| DAB6006 | Error | An Entry, transition end or alias member that is not a state, conduit or (as a source) alias of the machine |
| DAB6007 | Error | A state machine without `Entry` |
| DAB6008 | Error | A name declared twice (section, pose, cache, machine, member, rule, layer, alias, property, slot, notify, setting), or a cache named like a machine |
| DAB6009 | Error | A pose that uses itself |
| DAB6010 | Error | An expression that is not a pose |
| DAB6011 | Error | A statement or section that does not belong where it is written |
| DAB6012 | Error | A graph or state without its result (`Output = ...;` / `Pose = ...;`) |
| DAB6013 | Error | A shared rule that uses itself |
| DAB6014 | Error | Nothing to build: the source file cannot be read |
| DAB6015 | Error | Something an AnimLayerInterface cannot hold (a graph, a layer body, a state machine, notifies, asset slots) |
| DAB6016 | Error | Unknown metadata on a pose statement, `Cached` on the result, or a node function with no node to go on |
| DAB6017 | Error | A cached pose used outside the AnimGraph or layer that saves it and that graph's state machines |
