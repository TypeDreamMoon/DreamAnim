# DreamAnim language reference

This page is for people writing `.dab` files. The exact grammar and the canonical format are in
[Grammar.md](Grammar.md). How every construct maps onto the model is in [Mapping.md](Mapping.md).
Error codes are in [Diagnostics.md](Diagnostics.md).

- [A first file](#a-first-file)
- [Files and the document header](#files-and-the-document-header)
- [Sections](#sections)
- [Settings](#settings) · [Defaults](#defaults) · [Aliases](#aliases)
- [Properties](#properties) · [Assets](#assets) · [Notifies](#notifies)
- [The graph](#the-graph) · [Named and cached poses](#named-and-cached-poses)
- [Nodes](#nodes) · [Stacks](#stacks) · [Input poses](#input-poses)
- [Slot and Call](#slot-and-call) · [Layers and linked graphs](#layers-and-linked-graphs)
- [State machines](#state-machines)
- [Conveniences](#conveniences)
- [Values](#values) · [Expressions](#expressions) · [Calls](#calls)
- [Templates and #include](#templates-and-include) · [Regions and comments](#regions-and-comments)
- [Names](#names) · [Canonical format](#canonical-format)

## A first file

This is the whole of ALS-Refactored's head Blueprint, as the decompiler writes it:

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

- The header names the asset: `/ALS/ALS/Character/AnimationInstances/AB_Als_Head`.
- `Settings` gives the C++ parent class, the skeleton and one class setting.
- `Aliases` makes `Main` stand for `Self.GetParent()`, the main anim instance.
- `Graph` is the AnimGraph. `Additive` is a named pose: a stack of three nodes. `Output` is the result.
- `Main.HeadState.YawAmount` is a path into C++ state. The pin is bound to it and reads it every update.
- `Call(...)` nodes call C++ functions of the instance while the graph runs.

The file never computes game state. It reads C++ properties and calls C++ functions; the logic stays in
the C++ anim instance the Blueprint derives from.

## Files and the document header

| Extension | Document | Builds |
|---|---|---|
| `.dab` | `AnimBlueprint(...)` | an Animation Blueprint |
| `.dai` | `AnimLayerInterface(...)` | an Animation Layer Interface |
| `.dat` | `AnimTemplates(...)` | nothing; other files `#include` it |

Text is UTF-8; a byte order mark is accepted. Lines may end in `\n`, `\r\n` or `\r`.

Sources live under the source roots: the project's `DAnim/` folder and the `DAnim/` folder of every
enabled plugin that has one. The command line finds them there.

A file holds optional `#include` lines, then exactly one document (DAB2002, DAB2003).

### The header

```dab
AnimBlueprint(Name="ALS/Character/AB_Als", Root="Plugin.ALS")
```

| Argument | Meaning |
|---|---|
| `Name` | The asset's path under the root, without an extension. The last segment is the asset's name. Required. |
| `Root` | Where the path starts: `"Game"` (the project's content, the default), `"Engine"`, or `"Plugin.<Name>"` (that plugin's content). |

| Header | Asset |
|---|---|
| `AnimBlueprint(Name="Characters/AB_Hero")` | `/Game/Characters/AB_Hero` |
| `AnimBlueprint(Name="ALS/Character/AB_Als", Root="Plugin.ALS")` | `/ALS/ALS/Character/AB_Als` |
| `AnimLayerInterface(Name="ALS/Character/ALI_Overlay", Root="Plugin.ALS")` | `/ALS/ALS/Character/ALI_Overlay` |

The header takes only `Name` and `Root`, both as named strings (DAB2005). Any other root is DAB3006.

A build creates the asset when it does not exist and rebuilds it in place when it does. Rebuilding
replaces the Blueprint's graphs, functions, variables and interface layers, and empties its event graph.
The asset object and its generated class stay, so references to them survive. Anything edited by hand
in a generated Blueprint is lost at its next build.

## Sections

A document's body holds sections. Their order does not matter.

| Section | Form | What it is |
|---|---|---|
| Settings | `Settings = { ... }` | parent class, skeleton, interfaces, default animation set, class settings |
| Defaults | `Defaults = { ... }` | class defaults of the parent class's properties |
| Aliases | `Aliases = { ... }` | short names for paths from `Self` |
| Properties | `Properties = { ... }` | tuning values: Blueprint variables the graph reads |
| Assets | `Assets = { ... }` | animation slots an animation set fills |
| Notifies | `Notifies = { ... }` | anim notify handlers |
| Graph | `Graph = { ... }` | the AnimGraph |
| Layer | `Layer Name ...` | an animation layer |
| StateMachine | `StateMachine Name { ... }` | a state machine |
| Template | `Template Name(...) = ...;` | a template, expanded where it is used |

- Each `Name = { ... }` section appears at most once (DAB6008) and is a block (DAB6011).
- Layers, state machines and templates may appear any number of times, each with its own name.
- Anything else at the document level is DAB6011.

An `AnimLayerInterface` holds `Settings` and layer declarations. A graph, notifies, asset slots, state
machines and layer bodies are DAB6015 there:

```dab
AnimLayerInterface(Name="ALS/Character/ALI_Overlay", Root="Plugin.ALS")
{
    Settings = { Parent = AnimLayerInterface; }

    Layer Overlay [ LayerGroup = "Default Shared Group" ];
}
```

An `AnimTemplates` library holds templates. It makes no asset. Checked on its own, it is checked as far as
its syntax, its includes and its templates; a document that includes it checks the rest.

## Settings

```dab
Settings = {
    Parent                                = AlsAnimationInstance;
    Skeleton                              = SK_Als;
    Implements                            = [ ALI_Overlay ];
    bEnableLinkedAnimLayerInstanceSharing = true;
}
```

| Key | Value |
|---|---|
| `Parent` | The class the Blueprint derives from. A C++ class by name, with or without its `U` prefix (`AlsAnimationInstance`), or an Animation Blueprint by name or `Path(...)`. It must derive from AnimInstance (DAB3003). Without `Parent`, the class is AnimInstance. In an AnimLayerInterface the parent is `AnimLayerInterface`. |
| `Skeleton` | The target skeleton, by name or `Path(...)`. Without a skeleton the Blueprint is made as a template Animation Blueprint. |
| `Implements` | Animation Layer Interfaces, by name or `Path(...)`: a list, or a single value. |
| `DefaultAnimSet` | The animation set the Blueprint starts with. See [AnimSet.md](AnimSet.md). |
| any other key | A class setting of UAnimBlueprint, as in the editor's Class Settings panel: `bUseMultiThreadedAnimationUpdate`, `bEnableLinkedAnimLayerInstanceSharing`, ... Anything else is DAB4001. |

- Settings are constants, fixed when the Blueprint compiles (DAB4010).
- Settings take no metadata (DAB4005). A key given twice is DAB6008.
- When the parent is another Animation Blueprint, the engine gives the child no AnimGraph of its own. A
  `Graph` section then fails to build (DAB7005).

## Defaults

```dab
Defaults = { Settings = AIS_Als_Default; }
```

Each entry sets an editable property of the parent class on this Blueprint's class defaults: what the
editor's Class Defaults panel changes. The value is a constant of the property's type. A name that is
not an editable property of the parent is DAB4014.

## Aliases

```dab
Aliases = { Main = Self.GetParent(); }
```

An alias is a short name for a path from `Self`. Every use is replaced by the path before anything else
reads it: `Main.HeadState.YawAmount` means `Self.GetParent().HeadState.YawAmount`. The alias does not
exist in the Blueprint.

- `Self` is the anim instance itself.
- The value is a path that starts at `Self` (DAB5007). It may pass through calls without arguments,
  such as `GetParent()`.
- `Self`, `Props`, `Assets` and `None` cannot be alias names (DAB5007).

In ALS-Refactored, the linked Blueprints derive from `AlsLinkedAnimationInstance`, whose `GetParent()`
returns the main instance. The decompiler calls that alias `Main`.

## Properties

Tuning values are Blueprint variables the graph reads.

```dab
Properties = { float DrawAmount = 0 [ Group = "State" ]; }

Graph = { Output = SequenceEvaluator(Sequence = A_Als_Bow_Draw, ExplicitTime = Props.DrawAmount); }
```

- Each entry is `Type Name;` or `Type Name = Default;`, optionally with metadata (DAB6011).
- Types: `bool`; `int` (or `int32`), `int64`, `uint8` (or `byte`); `float` (or `double`, `real`); `FName`,
  `FString`, `FText` (with or without the `F`); `GameplayTag`, `GameplayTagContainer`; the name of an
  enum, of a struct (with or without its `F`), or of a class for an object reference. A type the checker
  does not know is warning DAB3008, and the default is kept as written.
- The type is a single name, so container types (`TArray<float>`, `TMap<...>`) cannot be written.
- The default is a constant (DAB4010).
- Metadata: `Group = "Name"` (or `Category = "Name"`) puts the variable in that category. Other
  `Key = "Value"` entries become the variable's metadata. The decompiler reads back only the category,
  as `Group`.

The generated variable can be edited per instance in the details panel and is read-only in the graph:
the graph never writes it.

Read a tuning value as `Props.Name`:

- In a node argument, `Props.Name` is a binding: the pin reads the variable every update.
- Where only a constant fits (settings, metadata, transition options, editor-node settings),
  `Props.Name` stands for its default. It then needs a default (DAB5006).
- An undeclared name is DAB5003.

## Assets

Asset slots are animations the Blueprint plays but does not name. An animation set chooses them at run
time; see [AnimSet.md](AnimSet.md).

```dab
Assets = { BlendSpace Look; }

Graph = { Output = BlendSpaceEvaluator(BlendSpace = Assets.Look, X = Main.HeadState.PitchAngle); }
```

- Each entry is `Type Name;` (DAB6011). The type is an animation asset class: `AnimSequence`,
  `BlendSpace`, `AnimMontage`, ... (DAB3005).
- A slot takes no value (DAB6011) and no metadata (DAB4005).
- Read a slot as `Assets.Name`. The pin is bound to a generated getter that reads the slot from the
  active animation set.
- A slot is read at run time, so it cannot go where only a constant fits (DAB4010).
- An undeclared slot is DAB5003.

## Notifies

```dab
Notifies = {
    ActivatePivot = Main.ActivatePivot();
    StopQuick     = Main.PlayQuickStopAnimation();
}
```

Each entry handles the anim notify of that name: `ActivatePivot` handles the `AnimNotify_ActivatePivot`
event. The `AnimNotify_` prefix may be written or left out.

- The value is a call, `Name = Main.Function(...);`, or a stack of calls run in order,
  `Name = { Main.First(); Main.Second(); };`.
- The calls follow the rules in [Calls](#calls).
- A notify handled twice is DAB6008.

The generator writes one `AnimNotify_<Name>` event per entry in the event graph.

## The graph

```dab
Graph = {
    Inputs    = [ "Standing Input", "Crouching Input" ];
    Crouching = Input("Crouching Input") [ Cached ];
    Standing  = Input("Standing Input") [ Cached ];

    Output = {
        GroundedStates;
        Inertialization(bResetOnBecomingRelevant = true);
        ModifyCurve(PoseGrounded = 1, FootLeftIk = 1, FootRightIk = 1);
        Call(Self.RefreshGrounded);
        Call(Self.InitializeGrounded, On = BecomeRelevant);
    };
}
```

`Graph` is the AnimGraph. It holds:

- named poses, `Name = Pose;`;
- the result, `Output = Pose;` (DAB6012 when missing). Write `Output = None;` for a graph that outputs
  nothing;
- optionally `Inputs = [ ... ];`, the order of the graph's input poses (see [Input poses](#input-poses)).

Anything else is DAB6011. A state machine cannot be written inside a graph: write it at the document
level and name it where it runs.

`Graph` takes no metadata (DAB4005). Layer settings go on the layer.

## Named and cached poses

### Named poses

`Name = Pose;` names a pose. A named pose without `[ Cached ]`:

- is visible only in the graph it is written in: the AnimGraph, one layer, or one state;
- is placed where it is used, and may be used once (DAB6005);
- still makes its nodes when it is never used, unlinked, with warning DAB6004.

Named poses may be written in any order. A pose may use one written below it. A pose that uses itself
is DAB6009. A name used as a pose that is no named pose, cache or state machine is DAB6003.

### Cached poses

`Name = Pose [ Cached ];` saves the pose in a cache named `Name` (a Save Cached Pose node). Each use of
`Name` reads the cache (a Use Cached Pose node).

```dab
MovementDetails = {
    MovementDetailsStates;
    Inertialization(bResetOnBecomingRelevant = true);
    Call(Self.RefreshStandingMovement);
    Call(Self.InitializeStandingMovement, On = BecomeRelevant);
    Call(Self.RefreshGroundedMovement);
} [ Cached ];
```

- A cached pose may be used any number of times.
- It can be used in the AnimGraph or layer that saves it, and in the states of the machines that graph
  runs. Anywhere else (another layer, or the AnimGraph for a cache a layer saves) is DAB6017: the
  engine's compiler cannot place a cache read in another graph. Give a layer the pose as an input
  instead.
- Cache names are shared by the whole Blueprint: a second cache of the same name is DAB6008, and so is a
  cache named like a state machine.
- `Output` cannot be cached (DAB6016). Cache a named pose and output its name.

### Node functions on a pose

The statement that names a pose can attach node functions to the pose's root node:

```dab
Look = BlendSpaceEvaluator(
    BlendSpace = BS_Als_Look,
    X          = Main.HeadState.PitchAngle
) [ OnBecomeRelevant = Self.InitializeHead(); OnUpdate = Self.RefreshHead() ];
```

| Key | Engine node function |
|---|---|
| `OnInitialUpdate = Call` | Initial Update |
| `OnBecomeRelevant = Call` | Become Relevant |
| `OnUpdate = Call` | Update |

- The value is a call; see [Calls](#calls).
- On a cached pose the functions go on the pose's root node, not on the cache node.
- The same keys work on `Output = ... [ ... ];`.
- A pose's metadata is `Cached` and these three keys. Anything else is DAB6016, and so is `Cached` with
  a value. Each function once per node (DAB4011).

## Nodes

A node is written like a call: its type, then named arguments.

```dab
TwoWayBlend(
    AlphaBoolBlend = (BlendOption = Linear),
    Alpha          = Main.PoseState.InAirAmount,
    A              = MultiWayBlend1,
    B              = SequenceEvaluator(Sequence = A_Als_Default_Poses, ExplicitFrame = 1)
)
```

### Type names

The type is the editor node class without its `AnimGraphNode_` prefix: `SequencePlayer` is
`AnimGraphNode_SequencePlayer`. A class with a prefix of its own matches too: `GameplayTagsBlend`
finds `AlsAnimGraphNode_GameplayTagsBlend`.

- When two classes match, write the full class name (DAB3002).
- No match, an abstract class, or a class without a runtime node is DAB3001. The module that defines
  the node must be loaded.
- `Input`, `Slot`, `Call` and `bones` are built in; see below.

### Arguments

Node arguments are named (DAB4008). A name is either:

- a field of the node's runtime struct, `FAnimNode_TwoWayBlend` for `TwoWayBlend`; or
- a setting of the editor node, such as `Tag` or `BoundEnum`.

Anything else is DAB4004, and so are fields that the editor or the compiler fills in. An argument given
twice is DAB2011 or DAB4011.

What an argument becomes depends on its value:

| Value | Becomes |
|---|---|
| a pose, on a pose pin (`A = ...`) | a link into the pin |
| a list of poses, on a pose array (`Poses = [ P0, P1 ]`) | links into `Poses_0`, `Poses_1`, ...; the list sets the number of pins |
| a constant (`PlayRate = 1.5`, `Method = SyncGroup`, a tuple, a list of constants) | the value, stored on the node |
| a path into C++ (`Main.LocomotionState.Speed`, `Self.GetControlRigInput()`), `Props.X` or `Assets.X` | a binding: the pin reads the path every update through Property Access |
| any other expression (`Main.RotationMode == "Als.RotationMode.Aiming" ? 1 : 0`) | a binding to a generated function that computes the expression |

- A constant equal to the node's default is not recorded.
- Editor-node settings (`Tag`, `BoundEnum`, ...) take constants only (DAB4010).
- `Tag = "Name"` sets the editor node's tag. ALS uses tags to find its linked graphs.
- Generated functions are named `DreamAnimExpr_<hash>`. They are marked thread-safe: the graph calls
  them on the animation worker threads.

### Pins from another class

`LinkedAnimGraph` and `ControlRig` show pins that belong to another class: the linked Blueprint's
variables, the rig's inputs. An argument that is not a field of such a node is one of those pins:

```dab
ControlRig(
    ControlRigAssetReference = (BlueprintRigClass = CR_Als),
    AlphaInputType           = Bool,
    AlphaBoolBlend           = (BlendOption = Linear),
    bResetInputPoseToInitial = false,
    Tag                      = "ControlRig",
    RigInput                 = Self.GetControlRigInput()
)
```

A constant for such a pin is typed by the target class when that class can be loaded.

### Empty pose pins

`None` leaves a pose pin unlinked. In a list of poses, `None` keeps its place, so the number of pins
does not change.

## Stacks

```dab
Output = {
    StandingStates;
    Inertialization(FilteredCurves = [ "RotationYawSpeed" ], bResetOnBecomingRelevant = true);
    ModifyCurve(PoseStanding = 1);
    Call(Self.RefreshRotateInPlace);
};
```

A stack `{ First; Step; Step; }` is a chain. `First` is any pose. Each step after it is a node whose
one free pose input takes the pose above it. The stack's pose is its last step.

- A step needs exactly one pose pin not given as an argument (DAB6001). `TwoWayBlend(B = X)` can be a
  step: `A` is free.
- A name (a named pose, a cache or a state machine) can only be the first entry (DAB6001).
- For a `LinkedAnimGraph` or `LinkedAnimLayer` step, the free input is the one input pose of its target
  not given in `Inputs`. The target must be loadable at check time to tell (DAB6001).
- An empty stack is not a pose (DAB6010).

A stack and a block both start with `{`. A stack's entries are expressions, each followed by `;`. A
block's entries are statements such as `Key = Value;`. Mixing the two is DAB2010.

## Input poses

`Input(Name)` is an input pose of the graph it is written in: a Linked Input Pose node. The name is the
engine's input pose name. Write it as a string when it is not an identifier: `Input("View Input")`.
Further arguments are named settings of the node.

```dab
Graph = {
    Inputs = [ "Locomotion Input", "Overlay Input" ];
    // ...
    Locomotion = Input("Locomotion Input") [ Cached ];
    // ...
}
```

To use an input pose in several places, cache it, as above.

### Order

`Inputs = [ ... ];` in a graph or a layer body gives the order of its input poses. Inputs not listed
follow in the order `Input(...)` first appears in the text. `Inputs` takes identifiers or strings.

The order matters. A `LinkedAnimGraph` or `LinkedAnimLayer` node in another Blueprint feeds its pose
pins to the target's input poses by position. A Blueprint compiled against an older order feeds the
wrong poses until it is rebuilt. Keep `Inputs` when you edit a decompiled file, and rebuild the
Blueprints that link a graph after changing its inputs.

`Layer Name = Pose;` has no room for `Inputs`. Its inputs follow the written order.

### Feeding another graph's inputs

On `LinkedAnimGraph` and `LinkedAnimLayer`, `Inputs = { Name = Pose; }` gives the target's input poses
by name:

```dab
LinkedAnimGraph(
    InstanceClass = AB_Als_Grounded,
    Tag           = "Grounded",
    Inputs        = {
        "Standing Input"  = LinkedAnimGraph(InstanceClass = AB_Als_Standing, Tag = "Standing");
        "Crouching Input" = LinkedAnimGraph(InstanceClass = AB_Als_Crouching, Tag = "Crouching");
    }
);
```

- Names are the target's input pose names; strings when they are not identifiers.
- A name the target does not have is warning DAB4013, when the target can be loaded.
- An input not given stays unlinked. In a stack, the one input left takes the pose above.
- `Inputs` is a block (DAB4006). A list of poses on these nodes is DAB4004.

## Slot and Call

### Slot

`Slot("Name", ...)` is a montage slot node. The first argument is the slot name; the others are named
settings of the node (DAB4008). A slot node is usually a stack step:

```dab
Slot1 = {
    Overlay;
    Slot("ArmLeft");
};
```

### Call

`Call(Target.Function, ...)` is a Call Function node. It calls a function of the instance while the
graph runs, and passes its input pose through.

```dab
Call(Self.RefreshHead);
Call(Self.InitializeHead, On = BecomeRelevant);
```

- The first argument names the function: `Self.Function`, `Main.Function` through an alias, or with
  arguments, `Self.Function(1, Rate = 2)`. A missing function is DAB4009.
- Further positional arguments are arguments of the function.
- `On = Site` says when it runs. The sites are the engine's `EAnimFunctionCallSite` without the `On`
  prefix: `Update` (the default), `Initialize`, `BecomeRelevant`, `Evaluate`, `InitializePostRecursion`,
  `UpdatePostRecursion`, `BecomeRelevantPostRecursion`, `EvaluatePostRecursion`, `StartedBlendingIn`,
  `StartedBlendingOut`, `FinishedBlendingIn`.
- A named argument that is a parameter of the function is an argument of the call. One that is a setting
  of the Call Function node stays on the node.
- The arguments follow the rules in [Calls](#calls).

## Layers and linked graphs

### Layers of this Blueprint

```dab
Layer Overlay [ LayerGroup = "Default Shared Group" ] = LocalRefPose();
```

```dab
Layer Overlay [ LayerGroup = "Default Shared Group"; BlendOptions = (BlendInTime = 0.2, BlendOutTime = 0.2) ]
{
    // named poses ...
    Output = ApplyAdditive(
        Alpha          = 0.75,
        AlphaBoolBlend = (BlendOption = Linear),
        Base           = Base,
        Additive       = SequencePlayer(
            GroupName = "Secondary Motion",
            Method    = SyncGroup,
            Sequence  = A_Als_Idle
        )
    );
}
```

| Form | Meaning |
|---|---|
| `Layer Name = Pose;` | a layer whose graph is that pose |
| `Layer Name [ meta ] { ...; Output = Pose; }` | a layer with named poses and `Inputs`, like `Graph` |
| `Layer Name [ meta ];` | a declaration without a graph: how an interface declares its layers |

- A layer named like a layer of an implemented interface implements it. Any other layer is a new layer
  of this Blueprint.
- Metadata are settings of the layer's result node and graph: `LayerGroup = "Group"` and
  `BlendOptions = (...)`. The default group is not recorded.
- A layer's input poses are the `Input(...)` poses in its body.
- An interface's layers carry no input poses in the text: `Layer Name [ meta ];` declares none, and the
  decompiler does not read them from an interface.
- Two layers of the same name are DAB6008.

### Running a layer

`LinkedAnimLayer` runs a layer.

```dab
LinkedAnimLayer(Interface = ALI_Overlay, Layer = "Overlay", InstanceClass = AB_Als_Default, Tag = "Overlay")
```

```dab
LinkedAnimLayer(Layer = "StandingStance", Tag = "Standing")
```

- With `Interface`, the node runs that interface's layer. `InstanceClass` sets the class it links.
- Without `Interface`, it runs a layer of this Blueprint. `Layer` is then that layer's name in this file.
- `Layer` is a name; the decompiler writes it as a string.

### Running another Blueprint's graph

`LinkedAnimGraph` runs another Blueprint's AnimGraph:

```dab
LinkedAnimGraph(InstanceClass = AB_Als_Head, Tag = "View")
```

Both nodes take their target's input poses through `Inputs = { ... }`, and both can be stack steps.

## State machines

```dab
StateMachine GroundedStates [ MaxTransitionsPerFrame = 1; bAllowConduitEntryStates = true ]
{
    Entry = EntryConduit;

    State Standing
    {
        Pose = {
            Standing;
            Call(Self.ResetGroundedEntryMode, On = BecomeRelevant);
        };
    }

    State StandingToCrouching
    {
        OnEnter = Main.StopTransitionAndTurnInPlaceAnimations();

        Pose = SequencePlayer(Sequence = A_Als_StandToCrouch, PlayRate = 1.2, bLoopAnimation = false);
    }

    // more states ...

    Conduit EntryConduit;
    Alias StandingAlias = [ Crouching, CrouchingToStanding, StandingToCrouching ];

    Transitions = {
        StandingToCrouching -> Crouching : Auto                                 [ Blend = 0 ];
        EntryConduit        -> Standing  : Main.PoseState.StandingAmount >= 0.5 [ Priority = 2 ];
        // more transitions ...
    }
}
```

A state machine is written at the document level. It runs where its name is used as a pose:
`Output = GroundedStates;`, a stack's first entry, or a state's `Pose`. It runs in exactly one place
(DAB6002 when used twice or never). To use its pose in several places, cache it where it runs:

```dab
LookStates_2 = LookStates [ Cached ];
```

A machine holds `Entry`, states, conduits, aliases, `Rules` and `Transitions`; anything else is DAB6011.
States, conduits and aliases share one set of names (DAB6008).

### Machine settings

The machine's metadata are settings of the state machine node: `MaxTransitionsPerFrame`,
`bSkipFirstUpdateTransition`, `bAllowConduitEntryStates`, ... An unknown key is DAB4005. A key without
a value sets a bool setting to true.

### Entry

`Entry = Name;` names the state or conduit the machine starts in (DAB6006). It is required (DAB6007).

### States

```dab
State LockLeftFoot
{
    OnEnter = Main.PlayTransitionAnimation(
        Sequence  = A_Als_Stop_Left,
        PlayRate  = 1.5,
        StartTime = 0.4
    );

    Pose = {
        MovementDetails;
        ModifyCurve(FootLeftLock = 1);
    };
}

State Stop { Pose = StopStates; }
```

- `Pose = ...;` is the state's result (DAB6012).
- `OnEnter`, `OnExit` and `OnBlended` call a function when the state is entered, when it is left, and
  when it has fully blended in. Each is a call; see [Calls](#calls).
- A state may hold named poses of its own. Cached poses from anywhere in the file are visible.
- `State Name = Pose;` is a state that holds only its pose.
- Metadata are editable settings of the state node: `State Jump [ bAlwaysResetOnEntry = true ]`.

### Conduits

```dab
Conduit LandConduct = Main.LocomotionMode == "Als.LocomotionMode.Grounded";
Conduit EntryConduit;
```

A conduit without a condition is always open (`true`). The condition may use rules. A conduit has no
body (DAB6011).

### Aliases

```dab
Alias GroundedAlias = [ Fall, Jump ];
Alias Anywhere = All;
```

A transition from an alias leaves from any of the states it lists; `All` lists every state. An alias
lists states and conduits (DAB6006). It can be the source of a transition, never the target (DAB6006).

### Rules

```dab
Rules = {
    RotateLeft  = Main.RotateInPlaceState.bRotatingLeft;
    Move        = Main.LocomotionState.bMovingSmooth;
    RotateRight = Main.RotateInPlaceState.bRotatingRight;
}
```

A rule is a named condition for the machine's transitions and conduits.

- A transition whose condition is a rule's name and nothing else uses the engine's shared rule of that
  name. All such transitions share one rule graph.
- A rule name inside a larger condition is replaced by the rule's expression.
- Rules may use other rules. A rule that uses itself is DAB6013.
- Inside `StateWeight(...)` and `MachineWeight(...)` a name is a state or machine, never a rule:
  `StateWeight(Move)` reads the state `Move` even when a rule is named `Move`.

### Transitions

```dab
Transitions = {
    Idle        -> RotateLeft  : RotateLeft                                                    [ Priority = 2; Inertialize ];
    RotateLeft  -> Idle        : Auto                                                          [ Blend = 0 ];
    RotateLeft  -> Idle        : Move                                                          [ Blend = 0.4 ];
    Idle        -> Move        : Move;
    Move        -> Idle        : !Main.LocomotionState.bMovingSmooth                           [ Priority = 2; TransitionStart = (NotifyName = "StopQuick") ];
    Move        -> Stop        : !Main.LocomotionState.bMovingSmooth && StateWeight(Move) >= 1 [ Blend = 0.1 ];
    Stop        -> Idle        : StateWeight(Stop) >= 1                                        [ Blend = 0.5; BlendProfile = Path(Plugin.ALS, "ALS/Character/SK_Als:QuickFeetBlend") ];
}
```

Each transition is `From -> To : Condition [ options ];` (DAB6011).

- `From` is a state, conduit or alias of the machine; `To` is a state or conduit (DAB6006).
- Several transitions between the same two members are separate transitions.
- The condition is one of:
  - `Auto`: the engine's automatic rule. The transition starts when the most relevant animation of the
    source state is about to finish (`AutomaticRuleTriggerTime` adjusts when);
  - a rule's name alone: the shared rule;
  - any other boolean expression.

| Option | Meaning | Engine default |
|---|---|---|
| `Blend = s` | crossfade duration in seconds (`CrossfadeDuration`) | 0.2 |
| `Curve = C` | blend curve (`BlendMode`), an `EAlphaBlendOption` name: `Linear`, `Cubic`, `HermiteCubic`, `Sinusoidal`, `QuadraticInOut`, `CubicInOut`, `QuarticInOut`, `QuinticInOut`, `CircularIn`, `CircularOut`, `CircularInOut`, `ExpIn`, `ExpOut`, `ExpInOut`, `Custom` | `HermiteCubic` |
| `Inertialize` | blend by inertialization instead of a crossfade (`LogicType = TLT_Inertialization`) | standard blend |
| `Inertialize = s` | the same, and sets the duration | |
| `Priority = n` | priority order (`PriorityOrder`); when several transitions pass at once, the lowest number wins | 1 |
| `BlendProfile = Path(...)` | the transition's blend profile | none |
| any other key | the transition node's property of that name: `AutomaticRuleTriggerTime`, `CustomBlendCurve`, `TransitionStart`, `Bidirectional`, ... | |

- `Curve = Custom` reads `CustomBlendCurve`: `[ Blend = 0.5; Curve = Custom; CustomBlendCurve = CF_Als_StanceChange ]`.
- Inertialization needs an Inertialization node after the machine, as in the engine.
- Options are constants (DAB4010). A key without a value sets a bool option to true: `[ Bidirectional ]`.
- An option equal to its engine default is not recorded. Unknown options are DAB4005; an option given
  twice is DAB4011.

### Reading machine state

These functions read the state machine a condition belongs to. They are anim getters of the engine.

| Function | Reads | Default argument |
|---|---|---|
| `StateWeight(S)` | the blend weight of state `S` | the transition's source state |
| `StateTime()` | the time spent in the machine's current state | |
| `MachineWeight(M)` | the blend weight of machine `M` | this machine |
| `TimeRemaining(S)` | the time left on the most relevant asset player in state `S` | the transition's source state |
| `TimeRemainingRatio(S)` | the same, as a fraction of its length | the transition's source state |

`S` is a state of the condition's machine, else the only state of that name in the Blueprint.

In a node argument inside a state, there is no transition source: name the state for `StateWeight`,
`TimeRemaining` and `TimeRemainingRatio`. `StateTime()` and `MachineWeight()` without an argument read
the machine whose state holds the node.
<!-- check: the generator builds these getters into node arguments, but no test covers it, and whether the engine accepts the getter there is not shown by the code. -->

## Conveniences

These shorthands apply on the way in, and the decompiler writes them back where they fit exactly.

### Curves on ModifyCurve

```dab
ModifyCurve(FootLeftLock = 1, FootRightLock = 1, AllowTransitions = 1)
ModifyCurve(RotationYawSpeed = Main.TurnInPlaceState.PlayRate, ApplyMode = Scale)
```

An argument of `ModifyCurve` that is not a field of the node is a curve. Its name goes into
`CurveNames`, its value into `CurveValues`: a constant is stored, anything else is bound.

- Do not mix curve arguments with `CurveNames` / `CurveValues` (DAB4011).
- A curve named like a field of the node (`Alpha`, `ApplyMode`, ...) cannot be written this way; give
  `CurveNames` and `CurveValues` instead.
- A curve name that is not an identifier is written as a string: `"Curve Name" = 1`.

### Keyed blend maps

On `BlendListByEnum` and `GameplayTagsBlend`, `BlendPose` may be a block keyed by enum entry or by tag:

```dab
BlendListByEnum(
    BoundEnum       = EAlsHipsDirection,
    BlendType       = Linear,
    ActiveEnumValue = Main.GroundedState.HipsDirection,
    BlendPose       = {
        Default = SequenceEvaluator(
            Sequence      = A_Als_Walk_Right_Forward,
            ExplicitFrame = 7
        );
        RightBackward = SequenceEvaluator(
            Sequence      = A_Als_Walk_Right_Backward,
            ExplicitFrame = 7
        );
    },
    BlendTime       = { Default = 0; RightBackward = 0.1; }
)
```

```dab
GameplayTagsBlend(
    ActiveTag = Self.ViewMode,
    BlendPose = { Default = ModifyCurve3; "Als.ViewMode.FirstPerson" = ModifyCurve4; },
    BlendTime = 0.1
)
```

- `Default` is pin 0. Each other key, in written order, takes the next pin. Each key once (DAB4011).
- `BlendListByEnum`: keys are entries of the enum (DAB4002). The enum is `BoundEnum = EnumName` (a C++
  enum by name, a user enum by `Path(...)`), or the type of the path bound to `ActiveEnumValue`
  (DAB4012 when neither). The node's visible entries follow from the keys; do not write
  `VisibleEnumEntries` as well (DAB4011).
- `GameplayTagsBlend`: keys are tag strings. An unregistered tag is warning DAB4003. Do not write
  `Tags` as well (DAB4011).
- A per-pose array such as `BlendTime` may be keyed the same way. The keys map to the same pins
  (DAB4002 for a key `BlendPose` does not have). A key left out gets zero.
- A plain list still works: `BlendPose = [ P0, P1 ]`.

### One value for every pose

On a node with one pose array, a per-pose array given a single value gets that value for every pose:
`BlendTime = 0`. The poses must be given on the same node.

### bones()

```dab
LayerSetup = [ bones(thigh_l, ik_foot_l, "VB foot_l") ]
```

Inside `LayeredBoneBlend`'s `LayerSetup`, `bones(a, b, ..., Depth = n)` is one layer (an
`FInputBlendPose`). Its branch filters are the listed bones, each with blend depth `n`. Bones are
identifiers or strings. Without `Depth` the blend depth keeps its default. `bones` anywhere else, or
with another named argument, is DAB4009.

### ExplicitFrame

`SequenceEvaluator(Sequence = A_Als_Roll, ExplicitFrame = 45)` also sets `bUseExplicitFrame = true`,
unless you write `bUseExplicitFrame` yourself.

## Values

### Constants

| Kind | Written as | Example |
|---|---|---|
| number | digits, a fraction, an exponent; `-` in front for a negative | `12`, `0.5`, `.5`, `1e-3`, `-0.5` |
| bool | `true`, `false` | `bLoopAnimation = false` |
| string, name, text | a string; a name may also be an identifier | `GroupName = "Movement"` |
| enum | the entry's short name, or its number | `Method = SyncGroup` |
| gameplay tag | a string | `"Als.Gait.Walking"` |
| tag container | a list of tag strings | `[ "Als.Gait.Walking", "Als.Gait.Running" ]` |
| asset | its name, `Path(...)`, an object path string, or `None` | `A_Als_Idle` |
| class | a C++ class name, a Blueprint's name or `Path(...)`, or `None` | `InstanceClass = AB_Als_Head` |
| struct | a tuple of fields | `(BlendOption = Linear)` |
| array, set | a list | `[ 1, 2, 3 ]` |
| map | a tuple of `"Key" = Value` entries | `("Left" = 1, "Right" = 2)` |
| nothing | `None` | |

Strings sit on one line. Their escapes are `\"`, `\\`, `\n`, `\t` and `\r` (DAB1006 for any other).

### Path(Root, "Rel")

`Path` names an asset exactly.

- `Root` is `Game`, `Engine` or `Plugin.<Name>`.
- `"Folder/Asset"` is the object `/<Mount>/Folder/Asset.Asset`. `"Folder/Package.Object"` names an
  object whose name differs from its package's.
- `:Sub` names an object inside the asset: `Path(Plugin.ALS, "ALS/Character/SK_Als:QuickFeetBlend")` is
  the blend profile `QuickFeetBlend` of the skeleton `SK_Als`.
- A malformed `Path` is DAB3006.

### Asset names

A bare name such as `A_Als_Idle` finds the asset of that name. The lookup tries, in order:

1. the Blueprint being built;
2. every asset in the asset registry with that name whose class fits the value: an AnimSequence for
   `Sequence`, a skeleton for `Skeleton`. For a class value, Blueprints whose class derives from the
   expected class.

More than one match is DAB3007: write `Path(...)`. No match is DAB3004. A name that is unique today can
become ambiguous when another asset of that name and class is added.

Names are looked up among assets that exist. A Blueprint that names another DreamAnim Blueprint not yet
built reports DAB3004 until that one is built. A `build` of all sources builds them in the right order:
interfaces first, then every document after the DreamAnim assets it names.

### Tuples and structs

A struct is a tuple of its fields: `(InRange = (Max = 0.25), InterpSpeedIncreasing = 20)`.

- Fields may be named, or positional in declaration order (DAB4007 when there are too many).
- Fields not written keep their defaults. Fields equal to their defaults are dropped.
- `()` is the struct with every field at its default. A block `{ Field = Value; }` reads like a tuple.
- A one-item tuple needs a name or a trailing comma: `(5)` is `5` in parentheses, `(5,)` is a tuple.
- A fixed-size array field cannot be written (DAB4006).

### Where only constants fit

Settings, `Defaults`, tuning value defaults, metadata, transition options and editor-node settings are
fixed when the Blueprint compiles. There:

- `Props.X` stands for the default of `X` (DAB5006 when it has none);
- `Assets.X` is DAB4010;
- any other value read at run time is DAB4010.

## Expressions

Expressions appear in node arguments (as bindings), in transition and conduit conditions, and in rules.

| Precedence (high to low) | Operators |
|---|---|
| member, call | `a.b`, `f(...)` |
| unary | `!a`, `-a` |
| multiplicative | `*`, `/` |
| additive | `+`, `-` |
| comparison | `<`, `<=`, `>`, `>=` |
| equality | `==`, `!=` |
| and | `&&` |
| or | `\|\|` |
| conditional | `c ? a : b` |

- Binary operators group from the left. Parentheses group.
- `&&` and `||` evaluate both sides.
- There is no index operator. A `[` after a complete expression opens the statement's metadata.

### Paths

A path starts at `Self`, an alias, `Props` or `Assets`, and continues with member names and calls
without arguments: `Self.GetParent().LocomotionState.bMoving`. Reads from `Self` become Property Access
nodes, the thread-safe way to read the instance. Calls with arguments cannot be read in an expression.

### Operators and types

- Arithmetic and comparisons pick the integer or real form from their operands. A constant with a
  fraction makes a comparison real: `Count > 0.5` stays exact.
- `==` and `!=` compare numbers, bools, names, strings, enums, gameplay tags and objects. A gameplay tag
  compares with a tag string: `Main.Gait == "Als.Gait.Walking"`. An enum compares with an entry name.
- `c ? a : b` chooses between numbers, bools, names, strings or text. `c ? 1 : 0` becomes the
  bool-to-number conversion.

### Functions

| Function | Result |
|---|---|
| `Abs(x)` | absolute value |
| `Min(a, b)`, `Max(a, b)` | smaller, larger |
| `Clamp(x, lo, hi)` | `x` limited to `lo..hi` |
| `IsValid(x)` | for a gameplay tag, whether it is valid; for an object or class, whether it is not `None` |
| `Matches(t, u)` | whether tag `t` matches tag `u`, or container `t` has tag `u`; `Matches(t, u, Exact = true)` for an exact match |
| `StateWeight`, `StateTime`, `MachineWeight`, `TimeRemaining`, `TimeRemainingRatio` | see [Reading machine state](#reading-machine-state) |

Any other function name is a build error (DAB7002).

`Unsupported("...")` is the decompiler's placeholder for something it could not express. It checks, but
it does not build (DAB7002).

## Calls

Calls appear in `Notifies`, in state functions (`OnEnter`, `OnExit`, `OnBlended`), in node functions
(`OnInitialUpdate`, `OnBecomeRelevant`, `OnUpdate`) and in `Call(...)` nodes.

```dab
OnEnter = Main.PlayTransitionAnimation(Sequence = A_Als_Stop_Left, PlayRate = 1.5, StartTime = 0.4);
```

- A call is `Target.Function(arguments)`. Without arguments the parentheses may be left out.
- The target is `Self`, an alias, or a path from `Self` that reads an object (DAB5001).
- The function belongs to the target's class. When the checker can type the target and the function is
  missing, that is warning DAB5004; the build then fails.
- The function must not be pure: a call that runs must have an effect.
- Arguments are constants, `Props.X` or `Assets.X` (DAB5002). A computed value is not an argument: give
  the C++ function no such parameter and let it read the value itself.
- Positional arguments fill the parameters in order. Named arguments go by parameter name; an unknown
  name is warning DAB5004.

The generator calls the function directly when the Blueprint can refer to it as it is. Otherwise it
writes a forwarding function, `DreamAnimCall_<Function>_<hash>`, that makes the call. State and node
functions run during the animation update, which may be on a worker thread; the C++ function must be
safe to call there. Notify events run on the game thread.

## Templates and #include

A template is a named expression with parameters, expanded where it is used.

```dab
// AlsCommon.dat
AnimTemplates(Name="ALS/AlsCommon", Root="Plugin.ALS")
{
    Template Frame(Seq, N) = SequenceEvaluator(Sequence = Seq, ExplicitFrame = N);

    // A four-way velocity blend; VB is the path to an FAlsVelocityBlendState.
    Template VelocityBlend(Forward, Backward, Left, Right, VB) = MultiWayBlend(
        Poses         = [ Forward, Backward, Left, Right ],
        DesiredAlphas = [ VB.ForwardAmount, VB.BackwardAmount, VB.LeftAmount, VB.RightAmount ]);

    Template Looped(Seq, Rate = 1.0) = SequencePlayer(Sequence = Seq, PlayRate = Rate, bLoopAnimation = true);
}
```

```dab
#include "AlsCommon.dat"

AnimBlueprint(Name="Characters/AB_Hero", Root="Game")
{
    Settings = {
        Parent   = AlsLinkedAnimationInstance;
        Skeleton = SK_Als;
    }

    Aliases = { Main = Self.GetParent(); }

    Graph = {
        Output = VelocityBlend(
            Frame(A_Als_Walk_Forward, 4),
            Frame(A_Als_Walk_Backward, 4),
            Frame(A_Als_Walk_Left_Forward, 6),
            Looped(A_Als_Walk_Right_Forward, Rate = 1.2),
            VB = Main.GroundedState.VelocityBlend
        );
    }
}
```

### Templates

- A template is `Template Name(Params) = Expression;` at the document level, in the file or in an
  included library. A block body or no body is DAB3106.
- Parameters may have defaults; those come last (DAB2014). A parameter declared twice is DAB2014.
- Arguments bind to parameters by position, then by name. A missing argument without a default is
  DAB3101; an unknown name DAB3102; too many arguments DAB3103.
- Expansion replaces each use of a parameter in the body with the argument, including a parameter at
  the start of a path (`VB.ForwardAmount`). Argument names, keys and metadata keys are not replaced.
- A template may call another. Expansion deeper than 16 levels is DAB3104.
- Two templates with the same name, in the file and its libraries together, are DAB3105.
- A template call uses a bare name. A template named like a node type hides that node type.
- Templates expand before any name is checked. A diagnostic inside an expansion points at the call.
- A template's body is any expression, not only a pose.

### #include

- `#include "Path.dat"` lines come before the document (DAB2009).
- The path resolves against the including file's folder first, then against each source root
  (DAB3201 when not found).
- An included file must hold an `AnimTemplates` document (DAB3203). Libraries may include libraries.
  A cycle is DAB3202. Each library is read once.

## Regions and comments

- `// ...` runs to the end of the line. `/* ... */` may span lines and does not nest (DAB1003 when not
  closed).
- `fmt` keeps comments where they were relative to their statement.
- `#Region "Label"` and `#EndRegion` start at the beginning of a line and pair up inside one block. An
  unpaired one is warning DAB2008: an extra `#EndRegion` is ignored, and a region left open runs to the
  end of its block. Regions may nest.
- In a graph block (`Graph`, a layer, a state), a region becomes a comment box titled with its label
  around the nodes its named poses (and its `Output` or `Pose`) make. In a state machine, it becomes a
  box around its states, conduits and aliases. Elsewhere it only groups the text. The boxes change
  nothing the Blueprint does, and decompiling does not read them back.

```dab
Graph = {
    #Region "Movement"
    MoveForwardBase = BlendSpacePlayer(
        BlendSpace = BS_Als_WalkRun_Forward,
        X          = Main.StandingState.StrideBlendAmount
    ) [ Cached ];
    #EndRegion

    Output = MoveForwardBase;
}
```

The only directives are `#include`, `#Region` and `#EndRegion` (DAB1005).

## Names

- An identifier is a letter or `_`, then letters, digits and `_`. Any Unicode letter counts, so
  Chinese names work. Identifiers are case-sensitive.

  ```dab
  StateMachine 站立状态
  {
      Entry = 待机;
      State 待机 { Pose = 站立姿势; }
      State 移动 { Pose = 站立姿势; }
  }
  ```

- State machines, states, conduits, aliases, rules, layers, templates, named and cached poses, tuning
  values and asset slots are named by identifiers. The decompiler turns engine names such as
  `Rotate Left` into identifiers ([Decompiling.md](Decompiling.md)).
- A string stands in for a name that is not an identifier in these places: statement keys
  (`"Standing Input" = Standing;`, `"Als.Gait.Sprinting" = ...;`), argument names, metadata keys, and
  input pose names in `Input(...)` and `Inputs`.
- There are no reserved words in the lexer. These words mean something where they are written:
  `true`, `false`, `None`, `Self`, `Props`, `Assets`, `Path`, `Auto`, `All`, `Default`, `Output`,
  `Pose`, `Inputs`, `Entry`, `OnEnter`, `OnExit`, `OnBlended`, the section names and the block words
  (`StateMachine`, `State`, `Layer`, `Template`, `Conduit`, `Alias`).

## Canonical format

`Tools/dab.ps1 fmt` prints each file in its canonical form. `-Write` rewrites the files that differ,
keeping their line ends; `-Verify` writes nothing and fails on any file that is not canonical. A file
with syntax errors is not formatted. Formatting twice gives the same text, and the result parses to the
same tree. In short:

- four spaces of indentation, lines up to 110 columns;
- one statement per line; one blank line at most between statements;
- `=` signs aligned over runs of single-line assignments; transitions aligned on `->`, `:` and `[`;
- calls on one line when they fit, otherwise one argument per line with the `=` aligned;
- a block with a single short statement on one line: `Aliases = { Main = Self.GetParent(); }`;
- metadata as `[ Key = Value; Flag ]`;
- numbers as written, and parentheses the author wrote kept.

The full rules are in [Grammar.md](Grammar.md#canonical-format).
