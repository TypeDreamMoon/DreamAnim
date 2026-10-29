# DreamAnim Control Rig language reference

This page is for people writing `.dcr` files. The exact grammar and the canonical format are in
[Grammar.md](Grammar.md). Error codes are in [Diagnostics.md](Diagnostics.md). Turning an existing Control Rig
into text is described in [Decompiling.md](Decompiling.md#writing-control-rigs-as-text).

A `.dcr` file is the source of one Control Rig. The syntax around it (header, blocks, metadata, comments,
`#include`, templates) is the one of `.dab` files, described in [Language.md](Language.md). What is new is that
the bodies of events and functions are code: statements that make the nodes of the rig's graphs.

- [A first file](#a-first-file)
- [Statements and nodes](#statements-and-nodes)
- [Files and the document header](#files-and-the-document-header)
- [Sections](#sections)
- [Settings](#settings) · [Module](#module)
- [Hierarchy](#hierarchy) · [Variables](#variables)
- [Events](#events) · [Functions](#functions) · [Detached code](#detached-code)
- [Statements](#statements)
- [Expressions](#expressions) · [Calls](#calls) · [Operators](#operators)
- [Names and scopes](#names-and-scopes) · [Types and wildcards](#types-and-wildcards)
- [Values](#values)
- [Traits](#traits) · [Injected nodes](#injected-nodes) · [Running another event](#running-another-event)
- [Templates and #include](#templates-and-include) · [Comments and regions](#comments-and-regions)
- [What the text cannot hold](#what-the-text-cannot-hold)
- [Names](#names) · [Canonical format](#canonical-format)

## A first file

This is the foot IK rig of the Mannequin in the MoverExamples plugin, as the decompiler writes it, shortened: its
last steps are left out and the region labels are cut down.

```dcr
ControlRig(Name="Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK", Root="Plugin.MoverExamples")
{
    Settings  = { PreviewMesh = SKM_Manny_Simple; }
    Hierarchy = { Import = SKM_Manny_Simple; }

    Variables = {
        double ZOffset_L_Target;
        double ZOffset_R_Target;
        double ZOffset_L;
        double ZOffset_R;
        bool   ShouldDoIKTrace [ Public ];
    }

    Function FootTrace(RigElementKey `Ik Foot Bone`) -> (Vector Offset)
    {
        local Vector AnimatedFloorLocation;

        let FootPos = GetTransform(`Ik Foot Bone`).Translation;
        let RootPos = GetTransform(Bone(root)).Translation;
        let Y       = FootPos.Y + 5;
        let Hit = SphereTraceByTraceChannel(
            Start  = Injected(VisualDebugVectorItemSpace((X = FootPos.X, Y = Y, Z = RootPos.Z) + (0, 0, 30))),
            End    = Injected(VisualDebugVectorItemSpace((X = FootPos.X, Y = Y, Z = RootPos.Z) - (0, 0, 50))),
            Radius = 3
        );
        return (Offset = Injected(VisualDebugVectorItemSpace(Hit.HitLocation)));
    }

    Event Forward
    {
        if (ShouldDoIKTrace)
        {
            #Region "Trace both feet"
            ZOffset_L_Target = FootTrace(Bone(IK_foot_L)).Z;
            ZOffset_R_Target = FootTrace(Bone(IK_foot_R)).Z;

            Detached
            {
                ZOffset_L_Target = 0;
                ZOffset_R_Target = 0;
            }
            #EndRegion

            #Region "Smooth the offsets"
            ZOffset_L = AlphaInterp(
                ZOffset_L_Target,
                bInterpResult         = true,
                InterpSpeedIncreasing = 15,
                InterpSpeedDecreasing = 15
            );
            ZOffset_R = AlphaInterp(
                ZOffset_R_Target,
                bInterpResult         = true,
                InterpSpeedIncreasing = 15,
                InterpSpeedDecreasing = 15
            );
            #EndRegion

            ModifyTransforms(
                ItemToModify = [ (Item = Bone(IK_foot_L), Transform = (Translation = (Z = ZOffset_L))) ],
                Mode         = AdditiveGlobal
            );
        }
    }
}
```

- The header names the asset: `/MoverExamples/Characters/Mannequins/Rigs/CR_Mannequin_BasicFootIK`.
- `Settings` gives the mesh the editor previews the rig with. `Import = SKM_Manny_Simple;` fills the hierarchy
  with the bones and curves of that mesh.
- `Variables` are the rig's member variables. `ShouldDoIKTrace` is public.
- `Function FootTrace` is a function of the rig. `local` declares a variable of the function, `let` names a
  value, `return` gives the output. Backticks write a name that is not an identifier.
- `Injected(...)` puts a node on a pin: here, a visual debug node on the trace's start and end.
- `Event Forward` is the forwards solve. Its code is a list of statements that run in the order written. `if`
  makes a Branch node.
- `ZOffset_L_Target = FootTrace(...).Z;` calls the function, then sets the variable to the `Z` of its output.
- `#Region ... #EndRegion` becomes a comment box around the nodes of the statements between the two.
- `Detached { ... }` holds statements that nothing runs. They stay in the graph.
- `(Translation = (Z = ZOffset_L))` is a literal with an expression in it. The constants of a literal become the
  pin's value; each expression is linked to its part of the pin.

## Statements and nodes

The text is the graph written in another way. It adds no meaning of its own.

- A call is one node. `GetTransform(Bone(root))` is one Get Transform node. Writing the same call twice makes
  two nodes, and a node that keeps state (`AlphaInterp`, a spring, a trace with a cache) has one state for
  each call.
- A call with execute pins (a mutable call) is a node on the execute chain. As a statement it runs where it
  stands. Statements run in the order written.
- A call without execute pins computes a value. It is a data node: nothing runs it, the nodes that read its
  value pull it. A call like this cannot stand alone as a statement (DAB9005).
- `let Name = call(...);` names the node. Every use of `Name` reads that one node. The node has that name in
  the asset.
- Reading a variable by its name is a getter node, one at every use. `Name = value;` is a setter node on the
  execute chain.
- `if` makes a Branch node and `for` a loop node. The statements after one continue from its Completed pin.
  The other execute outputs of a node are blocks after its call; see [Blocks after a call](#blocks-after-a-call).
- A mutable call inside the value of a statement (`A = FootTrace(...).Z;`) runs just before that statement,
  innermost first and left to right.
- RigVM evaluates a data node once in a run, where the first node that needs it runs. Later readers get the
  same value, even if something changed in between. This is the engine's rule, and the text builds the same
  graph the editor shows. To read a value again after something changed it, write the call again.

## Files and the document header

| Extension | Document | Builds |
|---|---|---|
| `.dcr` | `ControlRig(...)` | a Control Rig |
| `.dat` | `AnimTemplates(...)` | nothing; `.dcr` files may `#include` it |

Text is UTF-8; a byte order mark is accepted. Lines may end in `\n`, `\r\n` or `\r`.

Sources live under the source roots: the project's `DAnim/` folder and the `DAnim/` folder of every enabled
plugin that has one. A file holds optional `#include` lines, then exactly one document (DAB2002, DAB2003).

### The header

```dcr
ControlRig(Name="Modules58/Hinge", Root="Plugin.ControlRigModules")
```

| Argument | Meaning |
|---|---|
| `Name` | The asset's path under the root, without an extension. The last segment is the asset's name. Required. |
| `Root` | Where the path starts: `"Game"` (the project's content, the default), `"Engine"`, or `"Plugin.<Name>"` (that plugin's content). |

| Header | Asset |
|---|---|
| `ControlRig(Name="Rigs/CR_Hero")` | `/Game/Rigs/CR_Hero` |
| `ControlRig(Name="Modules58/Hinge", Root="Plugin.ControlRigModules")` | `/ControlRigModules/Modules58/Hinge` |

The header takes only `Name` and `Root`, both as named strings (DAB2005). Any other root is DAB3006.

### The asset

Unreal Engine 5.8 has two kinds of Control Rig asset: the Blueprint-based rig (`UControlRigBlueprint`) and the
independent rig (`UControlRigRuntimeAsset`, whose graphs live in its editor data), which is what the engine
creates by default. A `.dcr` file builds either; both are read and written through the interfaces they share.

- A build creates the asset when it does not exist. A new asset is an independent rig. The text cannot ask for
  the other kind.
- When the asset exists, the build rebuilds it in place and it keeps its kind. For a Blueprint-based rig,
  warning DAB9102 says so.
- Rebuilding replaces the hierarchy, the member variables and every graph with what the text says.
- The function library is updated in place. A function keeps its node, so rigs that call it keep their calls.
  A function the text no longer has is removed, and the engine then removes the calls to it in rigs that are
  loaded; DAB9103 names them.
- After the graphs are built the rig is compiled. Compile errors come back as DAB9101 and compile warnings as
  DAB9102.

### Checking and building

```
Tools/dab.ps1 check -File DAnim/Rigs/CR_Hero.dcr
Tools/dab.ps1 build -File DAnim/Rigs/CR_Hero.dcr
Tools/dab.ps1 build -File DAnim/Rigs/CR_Hero.dcr -NoSave
```

- Checking resolves every name against the engine's reflection and the asset registry. It loads the assets
  the file names: the preview mesh, shape libraries and the rigs in `Uses`.
- `build -NoSave` builds the rig and keeps it in memory only.
- A `build` of all sources builds Control Rigs before Animation Blueprints, since a Blueprint may run a rig.
  A document is built after the text-made assets whose names it mentions, so a rig comes after the rigs it
  uses.
- Close any editor that has the project open before a build, as for `.dab` files.
- The options of the other commands are in the [README](../README.md#command-line).

## Sections

A document's body holds sections. Their order does not matter.

| Section | Form | What it is |
|---|---|---|
| Settings | `Settings = { ... }` | preview mesh, shape libraries, the rigs whose functions are called |
| Module | `Module = { ... }` | module settings; makes the rig a module |
| Hierarchy | `Hierarchy = { ... }` | the static hierarchy |
| Variables | `Variables = { ... }` | member variables |
| Event | `Event Name [ meta ] { ... }` | an event and its code |
| Function | `Function Name(...) ...` | a function of the rig's function library |
| Detached | `Detached [ meta ] { ... }` | code at the top level that nothing runs |

- Each `Name = { ... }` section appears at most once (DAB6008) and is a block (DAB6011).
- Events, functions and detached blocks may appear any number of times.
- Anything else at the document level is DAB6011.

## Settings

```dcr
Settings = {
    PreviewMesh    = SKM_Biped_Template;
    ShapeLibraries = [ DefaultGizmoLibraryNormalized ];
    Uses           = [ StandardFunctionLibrary ];
}
```

| Key | Value |
|---|---|
| `PreviewMesh` | The skeletal mesh the editor previews the rig with, by name or `Path(...)`. `None` for no mesh. |
| `ShapeLibraries` | The shape libraries the controls draw their shapes from: a list of assets, by name or `Path(...)`. Left out, the rig has the default shape library of the Control Rig settings, as a new rig does; `[]` is a rig with none. |
| `Uses` | The Control Rigs whose public functions this rig calls: a list of assets, by name or `Path(...)`. |

- Any other key is DAB9001.
- An asset given by name must be the only registered asset of that name, of the class the key takes. A name
  that finds none is DAB9001. A name that finds several is DAB9002 for `PreviewMesh` and `Uses`; write
  `Path(...)` to say which. See [Path](#path).
- An asset in `Uses` that is not a Control Rig is DAB9012.

### Uses

A function of a used rig is called by its name, as if it were a function of this rig:
`` `Color Module Controls`(Controls = [ SpawnControl ], ...) `` calls a function of `StandardFunctionLibrary`.
Only public functions can be called (DAB9012). The rule for names is in [Calls](#calls).

`Library.Name(...)` always finds the function in that library. The library's name is its asset's name:

```dcr
Modules56_Arm.`FWD TwoBoneLimb`(FK_or_IK = GetAnimationChannel, Bones = GetModuleMetadata("Rig Bones").Value)
```

Two used rigs with the same asset name are DAB9002: the second one is not used. Give each a name for the calls
into it by writing `Alias = Rig` in the list. Write an alias as a plain identifier.

```dcr
Uses = [
    StandardFunctionLibrary,
    Modules_Arm   = Path(Plugin.ControlRigModules, "Modules/Arm"),
    Modules56_Arm = Path(Plugin.ControlRigModules, "Modules56/Arm")
];
```

An item without a name goes by its asset's name. Two libraries with the same name are DAB9002. The decompiler
writes an alias only where two used rigs share a name; it makes the alias from the rig's folder and name.

## Module

```dcr
Module = {
    Identifier = (Name = "Hinge");
    Category   = "General";
}
```

A rig with a `Module` section is a module of the modular rig system. Without one, it is an ordinary rig.

- The keys are the properties of the engine's module settings, by name: `Identifier`, `Icon`, `Category`,
  `Keywords`, `Description` and the rest. Any other key is DAB9001.
- The values are constants. A property that equals its default can be left out.
- A module usually has `Connector` and `Socket` elements in its `Hierarchy`, an `Event Connector` that resolves
  its connectors, and creates its controls by code in `Event Construction`.

## Hierarchy

The hierarchy section is the rig's static hierarchy: the elements it starts with.

```dcr
Hierarchy = {
    Import = SKM_Manny_Simple;

    Bone ik_hand_l_wrld
    {
        Parent    = Bone(ik_hand_root);
        Transform = (Translation = (0, 12.5, 0));
    }
    Curve IKBlend_l;
    Null root_space { Transform = (Translation = (0, 0, 100)); }
    Connector Steering;
}
```

The section holds `Import = Mesh;` and elements. Anything else is DAB6011.

### Import

`Import = Mesh;` adds the bones of the mesh's skeleton and the mesh's curves, the way the Control Rig editor
imports them. `Mesh` is a skeletal mesh, by name or `Path(...)`.

- Write `Import` before the elements it gives. An element written earlier that the import gives again is
  DAB9009.
- Elements written after `Import` that the import did not give are new elements of the rig: a bone you add is a
  user bone.
- To change an imported element, write it again after `Import` with only the properties that change. It keeps
  its place and, unless you write `Parent`, its parent. An imported element can be written again once.

```dcr
Bone pelvis { Transform = (Translation = (0, 0, 96)); }
```

- A mesh name that finds no skeletal mesh, or an asset that is not a skeletal mesh with a skeleton, is DAB9001.

### Elements

An element is written `Word Name;` or `Word Name { keys }`. The name is an identifier, or a name in backticks.

| Word | Element | Keys |
|---|---|---|
| `Bone` | a bone | `Parent`, `Transform`, `BoneType` (`User` or `Imported`) |
| `Null` | a null | `Parent`, `Transform` |
| `Control` | a control | `Parent`; the properties of the engine's control settings by name (`ControlType`, `AnimationType`, `DisplayName`, `ShapeName`, `ShapeColor`, `bShapeVisible`, `LimitEnabled`, `MinimumValue`, `MaximumValue`, ...); and `Offset`, `Initial`, `ShapeTransform` |
| `Curve` | a curve | `Value` |
| `Connector` | a connector, for modules | the properties of the engine's connector settings by name (`Type`, `bOptional`, `Rules`, ...) |
| `Socket` | a socket | `Parent`, `Transform`, `Color`, `Description` |

- A key is written `Key = Value;`. A key that is not one of the element's is DAB9001.
- The values are constants (DAB9004). A key equal to its default can be left out.
- `Transform` is the element's initial transform, relative to its parent. `Offset` and `ShapeTransform` are a
  control's initial offset and shape transform. `Initial` is the control's initial value, written as a value of
  its control type: `true`, `0.5`, a vector, a rotator, a transform.
- `Value` of a curve is its initial value, a number.
- Element names are the rig's own: they are how other assets and the rig's code refer to the elements. The
  same element (the same kind and name) twice is DAB9009.
- Elements are created in the order written.

```dcr
Control leg_l_stretch_switch
{
    AnimationType = AnimationChannel;
    ControlType   = Bool;
    DisplayName   = "Stretch";
}
```

### Parents

There are two ways to say where an element sits.

- `Parent = Bone(pelvis);` inside the element. The parent is written as an element key: `Bone(name)`,
  `Null(name)` or `Control(name)`. The decompiler always writes this form.
- Nesting: an element written inside the block of another has that one as its parent.

```dcr
Control global_ctrl
{
    ControlType    = EulerTransform;
    ShapeName      = "Hexagon_Thin";
    ShapeTransform = (Scale3D = (15, 15, 15));

    Null leg_l_controls_space
    {
        Control thigh_l_fk_ctrl
        {
            ControlType = EulerTransform;
            ShapeName   = "Circle_Thick";
        }
    }
}
```

- A bone, null, control or socket may have a parent. A curve or a connector may not (DAB9009).
- A bone, a null or a control may be a parent. A curve, a connector or a socket may not (DAB9009).
- The parent must be declared before the element (DAB9009), whether it is named or the element is nested in
  it. A `Parent` that is not an element key is DAB9009.
- An element with no parent sits at the root.

## Variables

```dcr
Variables = {
    ModuleSettings `Module Settings` [ Public ];
    RigControlAxis PrimaryAxis           = X [ Public ];
    LinearColor    Color                 = (1, 0, 0, 1) [ Public ];
    Array<RigElementKey> Chain [ Public; Category = "Setup" ];
    FName          `Control Shape`       = "Circle_Thick" [ Public ];
}
```

- Each entry is `Type Name;` or `Type Name = Default;`, optionally with metadata (DAB6011). A name declared
  twice is DAB6008.
- Types:
  - `bool`, `int32`, `int64`, `uint8`, `float`, `double`, `FName`, `FString`, `FText`;
  - a struct by name, with or without its `F`: `Vector`, `Rotator`, `Quat`, `Transform`, `EulerTransform`,
    `LinearColor`, `RigElementKey`, ...;
  - an enum by name, with or without its `E`: `RigControlAxis`;
  - a user struct or user enum, by the name of its asset;
  - a class, by name, for an object reference;
  - an array: `Array<T>`, or `TArray<T>`. An array of arrays is not supported.
- A type that finds nothing, or finds several, is DAB9001. Write the full name for the second case.
- The default is a constant of the type (DAB9004): a number, a string, a value of an enum, an asset, a tuple, a
  list. A name of a variable cannot stand there.
- Metadata:

| Key | Meaning |
|---|---|
| `Public` | The variable is public. A module's public variables are its settings. |
| `ReadOnly` | The code cannot assign the variable (DAB9006). |
| `Category = "Name"` | The variable's category. |
| `Tooltip = "Text"` | The variable's tooltip. |

- Any other metadata is DAB9001.
- Code reads a variable by its name and sets it with `Name = value;`. See [Statements](#statements).
- The variables of a rig are its member variables. A function can have local variables of its own; see
  [Functions](#functions).

## Events

```dcr
Event Forward
{
    SetTransform(Item = Control, Value = GetTransform(SteeringItem));
}

Event Construction [ Graph = "ConstructionGraph" ]
{
    // ...
}

Event `Old Arm CONSTR`
{
    // ...
}
```

An event is a node that starts an execute chain, and its code is the statements that run from it. The event's
word picks the node:

| Word | Event | The engine runs it as |
|---|---|---|
| `Forward` | Begin Execution | `Forwards Solve` |
| `Backward` | Inverse Execution | `Backwards Solve` |
| `Construction` | Prepare For Execution | `Construction` |
| `PostConstruction` | Post Prepare For Execution | `PostConstruction` |
| `PreForward` | Pre Begin Execution | `Pre Forwards Solve` |
| `PostForward` | Post Begin Execution | `Post Forwards Solve` |
| `Interaction` | Interaction Execution | `Interaction` |
| `Connector` | Connector Execution | `Connector` |
| any other name | a user-defined event | that name |

- An event is written `Event Name [ meta ] { code }`. Without code it is DAB9004.
- The metadata of an event is `Graph`, a non-empty string (DAB9010). Anything else is DAB9001.
- `Graph = "Name"` says which graph of the rig the event is in, by the graph's name. A graph the rig does not
  have is added. Without `Graph`, the event is in the rig's default graph, `RigVMModel`. Modules often keep
  `Construction` in `ConstructionGraph` and `Connector` in `RigVMModel Connector Event Graph`.
- The same event twice in one graph is DAB9010. Put the second in another graph.
- A user-defined event is named by its word. Write it in backticks when it is not an identifier.
- An event has no local variables, and no `return` (DAB9004, DAB9007).

## Functions

```dcr
Function `Scale Module Controls`(Array<RigElementKey> Controls, double `Local Scale` = 0, double `Global Scale` = 1)
    [ Public; Category = "Modules"; Keywords = "Standard Functions Library Module Modular" ]
{
    for (Control in Controls)
    {
        let Shape = GetShapeTransform(Control.Name);
        SetShapeTransform(
            Control   = Control.Name,
            Transform = (
                Rotation    = Shape.Rotation,
                Translation = Shape.Translation,
                Scale3D     = Multiply(
                    Shape.Scale3D,
                    (`Global Scale`, `Global Scale`, `Global Scale`),
                    (`Local Scale`, `Local Scale`, `Local Scale`)
                )
            )
        );
    }
}
```

A function is a function of the rig's function library. Its head is

```
Function Name(Type a, Type b = 1) -> (Type c, Type d) [ meta ]
```

and its body is either `{ code }` or `= value;`.

- Parameters are `Type Name` with an optional constant default (DAB9004). Outputs are `Type Name` and take no
  default. The arrow and the outputs are left out when there are none. A name used twice among the parameters,
  or twice among the outputs, is DAB2014.
- A parameter is read by its name in the body. It cannot be assigned (DAB9006).
- A function declared twice is DAB6008.
- A function with neither `{ code }` nor `= value;` is DAB9004.
- The comment above a function is a comment box of its own in its graph.

### Metadata

| Key | Meaning |
|---|---|
| `Public` | Other rigs can call the function. |
| `Category = "Name"` | The function's category in the editor's menu. |
| `Keywords = "words"` | The function's keywords. |
| `Description = "Text"` | The function's description. |
| `Pure` | A function written as code that callers see as a value, with no execute pins. |
| `Inline` | The function is the body of one collapse node. See [Inline functions](#inline-functions). |

Any other metadata is DAB9001.

### Functions that run

A function written with `{ code }` and not marked `Pure` is mutable: it has execute pins and its code runs
when it is called. Its code may call mutable nodes and assign variables.

```dcr
Function Pick(Array<double> Values, int32 Index = 0, int32 Count = 3) -> (double Value, bool Found)
{
    local double Result = 0;
    if (Index >= 0 && Index < Count)
    {
        Result = Values[Index];
    }
    return (Value = Result, Found = Index >= 0);
}
```

- `local Type Name [= constant];` declares a local variable. It belongs to the whole function, wherever it is
  written in the code, and its default is a constant. The code reads it by name and assigns it with
  `Name = value;`. A local with the name of a parameter or another local is DAB6008.
- `return (Output = value, ...);` gives the outputs, each by name. It must be the last statement of the
  function's code, at its top level (DAB9007). A `return` inside an `if` or a loop is DAB9007.
- `return;` gives no values. An output that is not given keeps its default.
- A function without a `return` returns at the end of its code.
- A function with no mutable statement at all is still mutable, and the call is a node with execute pins.

### Functions that compute a value

```dcr
Function `Axis to Bool`(RigControlAxis Axis) -> (bool X, bool Y, bool Z) [ Public ] = (
    X = Axis == X,
    Y = Axis == Y,
    Z = Axis == Z
);

Function Second(Array<double> Values) -> (double Value) = Values[1];
```

`= value;` writes a function with no execute pins.

- With one output, the value is the output's value: `= Values[1];`.
- With several, the value is a tuple that names each output: `= (X = ..., Y = ...);`. A name that is not an
  output is DAB9001.
- With one output, a tuple naming that output also works, as long as the output's type has no member of that
  name.
- `[ Pure ]` on a function written `= value;` is DAB9013; it is pure already.

A function written as code and marked `Pure` is called as a value too. Its code computes: lets and a `return`.

```dcr
Function Split(double Value) -> (double Low, double High) [ Pure ]
{
    let Half = Value * 0.5;
    return (Low = Half - 1, High = Half + 1);
}
```

### Calling a function

A function is called like a node: `Name(arguments)`. See [Calls](#calls).

- Positional arguments fill the parameters in order; named arguments go by the parameter's name.
- An argument that is left out takes the parameter's default.
- A mutable function is a statement or an expression that runs where it stands. A function that computes a value
  is a data node.
- With several outputs, pick one with `.Name`: `Split(Speed).Low`. With one output the call is the output.

### Inline functions

```dcr
Function Twice(double Value) -> (double Result) [ Inline ] = Value + Value;
```

An `Inline` function is not a function of the library. It is the body of a collapse node, a subgraph inside one
graph, and the node is made where the function is called.

- It must be called exactly once. A function never called, or called twice, is DAB9008.
- The collapse node has the function's name. Its parameters and outputs are the node's pins.
- It may be written with code or with `= value;`, and may declare `local` variables. They belong to the
  collapse node's graph.
- An Inline function is written anywhere at the top level of the document. The decompiler writes them after the
  events.

```dcr
Function Attach_Bone_to_Control() [ Inline ]
{
    if (`Drive Skeleton`)
    {
        for (Element in `All Controls`)
        {
            Parent(Child = GetMetadata(Element, Name = "Item").Value, `New Parent` = Element);
        }
    }
}

Event Forward [ Graph = "RigVMModel Forwards Solve Event Graph" ]
{
    Attach_Bone_to_Control();
}
```

## Detached code

A `Detached` block holds statements that are chained one after the other, like any code, but nothing runs into
the first of them. They are in the graph and never run.

```dcr
Detached
{
    ZOffset_L_Target = 0;
    ZOffset_R_Target = 0;
}
```

- Inside an event or a function, `Detached { ... }` is a statement. The code around it goes on as if it were not
  there. It may read the lets written before it.
- At the top of the document, `Detached [ Graph = "Name" ] { ... }` holds detached code that belongs to no
  event. `Graph` works as for an event; without it the code is in the default graph.
- The decompiler writes a node that nothing runs under `Detached`, so it is not lost.

## Statements

The statements of a code block, in the order they run:

```dcr
let SteeringItem = ResolveConnector(Connector(Steering), SkipSocket = true).Result;
float Scale = GetModuleMetadata(Name = "Control Size", NameSpace = Root, Default = 1);
local Vector Offset;
Speed = AlphaInterp(Speed, bInterpResult = true);
Chain[2] = Bone(thigh_l);
SetTransform(Item = Control, Value = GetTransform(SteeringItem));
```

A statement ends with `;`, except `if`, `for`, `Detached` and a call with blocks, which end with their block.
Code statements take no metadata (DAB2015): a `[` after an expression is an index.

### let

`let Name = value;` names a value.

- When the value is a call, `Name` is the node, and a call with several outputs is picked with `Name.Output`.
- Every use of `Name` reads the same node. It is not a second call. See
  [Statements and nodes](#statements-and-nodes).
- When the value is a call that runs (a mutable call), the `let` is where it runs.
- When the value is a constant, `Name` stands for it, and each use writes the constant.
- A name is visible in the block where it is written and in the blocks inside it. After the block, it stays
  readable unless the code around it has a name of its own like it; it names a node whose value is there to
  read.
- A `let` is not assigned to again; `Name = value;` where `Name` is a let is DAB9006.

### Typed let

`Type Name = value;` is a `let` with its type written.

```dcr
float Scale = GetModuleMetadata(Name = "Control Size", NameSpace = Root, Default = 1);
Array<double> Copy = Weights;
```

It is needed when a node's types cannot be told from its arguments or from where its result goes. See
[Types and wildcards](#types-and-wildcards). The decompiler writes a type only where it is needed.

A typed let of a constant gives the constant that type. Every node it is given to takes the type:

```dcr
float Default = 1;
GetModuleMetadata("Control Size", NameSpace = Root, Default = Default).Value
```

### local

`local Type Name [= constant];` declares a local variable of the function. It is written in a function's code
and is the same for the whole function. In an event it is DAB9004. See [Functions](#functions).

### Assignment

`Name = value;` sets a variable: a member variable of the rig, or a local variable of the function.

```dcr
ZOffset_L = AlphaInterp(ZOffset_L_Target, bInterpResult = true, InterpSpeedIncreasing = 15);
Chain[2]  = Bone(thigh_l);
```

- It makes a setter node on the execute chain. The variable is set whole.
- `Name[index] = value;` sets one element of an array variable (an Array Set At Index node). A variable that is
  not an array is DAB9004.
- A name that is a parameter, a `let` or a loop name cannot be assigned (DAB9006). Neither can a read only
  variable (DAB9006).
- One member of a variable cannot be assigned: `Position.X = 1;` is DAB9006. A setter writes the whole value, so
  the other members would go back to the pin's default. Write the whole value:
  `Position = (X = 1, Y = Position.Y, Z = Position.Z);`.
- A name that is no variable is DAB9001. A value of another type is DAB9004.

### Calls as statements

```dcr
SetTransform(Item = Control, Value = GetTransform(SteeringItem));
```

A call with execute pins can stand alone. Its node is on the execute chain. A call that only computes a value
is DAB9005. Use its value, or remove it.

### Blocks after a call

A node can have execute outputs besides the one execution goes on by after it: ALS's Is Game World runs `True`
or `False`, Run Once runs `Once` the first time, Timer runs `JustStarted`, `IsRunning` and `JustFinished`. The
code each one runs is a block, written in braces after the call and named by the output.

```dcr
AlsRigVMFunction_IsGameWorld()
{
    True
    {
        RefreshDiagonalScaling();
        RefreshSpineRotation();
    }
}

let Clock = ControlFlowTimer(Duration = 2, Repeats = true)
{
    IsRunning
    {
        Elapsed = Clock.Time;
    }
    JustFinished
    {
        Speed = Speed + 1;
    }
}
```

- The statement ends with its closing brace; it takes no `;`.
- Each block is `Output { statements }`, the output by its pin name. The blocks may come in any order, and an
  output with nothing to run has no block.
- The output execution goes on by (`Completed`, or the execute pin the node takes and gives) is not a block:
  the statements after the call run from it. An output the call does not have, or that one, is DAB9001. A
  block written twice is DAB9004, and so are blocks after a call without execute pins.
- A `let` names the node, so its blocks can read its outputs (`Clock.Time`). A typed let takes blocks too; a
  `local` does not.
- Names written in a block follow the rules of the blocks of an `if`.
- `if` and `for` stay the way to write the Branch, For Loop Count and Array Iterator nodes. The decompiler
  writes every other node with such outputs this way, a loop the text has no `for` for included: its body is
  the block of the pin the node takes and gives.

### if

```dcr
if (`Controlled by Proxy`)
{
    SetMetadataTag(Item = SpawnControl, Tag = "Run Backwards", NameSpace = Root);
}
else if (Weight > 0.5)
{
    // ...
}
else
{
    // ...
}
```

- The condition is a bool.
- It makes a Branch node. The first block runs from True. `else if` is another Branch node in the False side,
  and `else` is the False side's code.
- The statements after the `if` run after either side.
- An `else` without an `if` before it is DAB2016. An empty block is allowed.

### for

```dcr
for (Control in Controls)                 { ... }
for (Control, Index in Controls)          { ... }
for (Item, Index, Count, Ratio in Chain)  { ... }
for (Index, Ratio in Range(5))            { ... }
```

- `for (names in Array)` makes an Array Iterator node. The names are bound, in this order, to `Element`,
  `Index`, `Count` and `Ratio`. One to four names; leave out the ones you do not need from the end.
- `Range(n)` makes a For Loop Count node. It can only be the array of a `for`. Its names are `Index` and
  `Ratio`.
- A header with no name before `in`, more than four names, or no `in` is DAB2017. More names than the loop has
  values is DAB9004.
- The body runs from the loop's body pin. The statements after the loop run after it completes.
- A node with state in the body has one state, shared by all iterations, as in the graph.
- A loop name cannot be assigned (DAB9006).

### return

`return (Output = value, ...);` in a function. See [Functions](#functions). A `return` whose value is not a
tuple of named items is DAB2018.

### Detached

`Detached { ... }` as a statement. See [Detached code](#detached-code).

### Regions and comments

`#Region "Label"` and `#EndRegion` mark a stretch of statements. Comments above a statement are kept as comment
boxes. See [Comments and regions](#comments-and-regions).

## Expressions

Expressions are the values of arguments, lets, assignments, conditions and returns.

| Precedence (high to low) | Operators |
|---|---|
| member, call, index | `a.b`, `f(...)`, `a[i]` |
| unary | `!a`, `-a` |
| multiplicative | `*`, `/`, `%` |
| additive | `+`, `-` |
| comparison | `<`, `<=`, `>`, `>=` |
| equality | `==`, `!=` |
| and | `&&` |
| or | `\|\|` |
| conditional | `c ? a : b` |

- Binary operators group from the left. Parentheses group, and the ones you write are kept.
- The index `a[i]` exists only in code. In a section a `[` after a value opens metadata.
- A value is one of:
  - a constant: a number, `true`, `false`, a string, `None`;
  - a name: a variable, a parameter, a let, a loop name, or a value of an enum;
  - a tuple `(X = 1, Z = 3)` or `(0, 0, 30)`, or a list `[ a, b ]`;
  - an element key: `Bone(pelvis)`;
  - an asset: `SKM_Manny_Simple`, `Path(Plugin.X, "A/B")`;
  - a call: `GetTransform(Bone(root))`;
  - a member of a value: `Hit.HitLocation`, `FootPos.Z`, `Module Settings`.`Control Size`;
  - an operator on values.
- A member picks a part of a struct: `x.Translation.Z` is the Z of the translation of `x`. A member that the
  struct does not have is DAB9011. A member of a user struct goes by the name the editor shows.
- A member after a call with several outputs picks the output. After a call with one output, the member is a
  member of that output (the output's own name also works: `GetTransform(x).Transform`).
- A literal may hold expressions: `(Translation = (Z = ZOffset_L))`. Its constant parts become the pin's value;
  its expressions are linked to the parts they stand for.
- A value that a call cannot give is DAB9004: a call that gives no value (such as `RunEvent(...)`), or a call
  with several outputs used without picking one.

## Calls

A call is `Name(arguments)`, or `Library.Name(arguments)` for a function of a used rig. What it makes depends on
what the name finds.

| The name finds | The call makes |
|---|---|
| a function of this rig | a call node of the function, or a collapse node for an `Inline` one |
| a function of a rig in `Uses` | a call node of the function in that rig |
| a unit (`RigUnit_...`, `RigVMFunction_...`) | a unit node |
| a dispatch (`RigVMDispatch_...`, `RigDispatch_...`) | a dispatch node |
| a template | the unit or dispatch the arguments pick |

### Finding the name

- A unit or dispatch is written by its short name, its struct name without the prefix: `GetTransform` for
  `RigUnit_GetTransform`, `If` for `RigVMDispatch_If`. The full name works too, and picks exactly.
- A template is written by its name with the spaces left out (`Multiply`, `Scale`). It is found only when no
  unit or dispatch has that name. See [Templates](#templates).
- A function of this rig hides everything else of its name. To reach the engine's node, write its full name.
- A function of a used rig is found by its name when nothing else has it. Two libraries with a function of that
  name, or a library's function and a node of the engine, make the name ambiguous (DAB9002). Write the library's
  name before it, or the node's full name.
- `Library.Name` names the library by its asset's name, or by its alias (see [Uses](#uses)). A library that is
  not in `Uses` is DAB9001.
- A function that is not public cannot be called from another rig (DAB9012).
- Two units of one short name (DAB9002): write the full name. A current unit hides a deprecated one of the same
  name.
- A name that finds nothing is DAB9001.

### Arguments

- Positional arguments fill the node's inputs in the order the node declares them. Execute pins and hidden
  pins are not inputs. Named arguments go by the name of the input. Positional arguments come before named
  ones (DAB2012).
- A name that is not an identifier is written in backticks: `` `Local Scale` = 1 ``.
- An input given twice is DAB9004. An input the node does not have is DAB9001. More positional arguments than
  the node has inputs is DAB9004.
- An input that is not given keeps the node's default.
- A node that adds up values (`Add`, `Multiply`, `And`, `Or`, ...) takes more than two positional arguments:
  each extra one adds an input pin to the node. `Multiply(a, b, c)` is one node. `a * b * c` is two.
- The value of an argument must fit the input's type (DAB9004). A `float` and a `double` may be linked to each
  other without a conversion node.

### Outputs

- A node with one output: the call is the output.
- A node with several: pick one with `.Name`. `ResolveConnector(Connector(Steering)).Result`.
- A node with none: the call gives no value. It can only be a statement.

### Float math

The text holds no float math nodes. An operator picks the double version of a math node, and the decompiler
writes a float math node as its double version. Building such text makes the double node where the rig had the
float one. Float and double values link to each other, so the rest of the graph is not changed.

## Operators

An operator is a short way to write a common node. Which node comes from the types of the operands.

| Operator | Node |
|---|---|
| `a + b`, `a - b` | Add, Subtract |
| `a * b` | Multiply; Scale when a vector is multiplied by a number |
| `a / b`, `a % b` | Divide, Modulo |
| `-a` | Negate |
| `a < b`, `a <= b`, `a > b`, `a >= b` | Less, LessEqual, Greater, GreaterEqual |
| `a == b`, `a != b` | Core Equals, Core Not Equals |
| `!a`, `a && b`, `a \|\| b` | Not, And, Or |
| `c ? a : b` | If |
| `a[i]` | Array Get At Index |

- The operands of an operator have the same type. The exceptions are `float` with `double`, which mix, and a
  vector times a number. Any other pair is DAB9004.
- A constant operand takes the type of the other operand.
- `&&` and `||` are the And and Or nodes. Both sides are computed; there is no short-circuit.
- `a + b + c` is two nested nodes. To make one node with three inputs write `Add(a, b, c)`.
- `c ? a : b` is an If node. Its type comes from `a` and `b`, or from where the result goes. `c ? 1 : 0` alone
  has nothing to say which type the numbers are; see [Types and wildcards](#types-and-wildcards).

## Names and scopes

What a name in code means is found in this order:

1. a let, a loop name or a parameter of the function whose code it is in;
2. a local variable of that function;
3. a member variable of the rig;
4. a value of the enum the place takes, `None`, or an asset, when the place takes one of those.

- A name found in 1 to 3 is a value. A variable name makes a getter node.
- A name no one finds is DAB9001, unless the place takes an enum, an asset or a name, where it is a value.
- A name that is a value of the enum a pin takes is that value, unless a variable, let or parameter of that name
  has the enum's type. A variable of another type with the name does not hide it.
- `ERigVMTransformSpace.GlobalSpace` writes the enum's name in front of its value.
- The names of variables, functions, pins, events, elements and graphs are the asset's own. They are kept as
  they are; one with spaces or other characters is written in backticks.
- The names of lets are only in the text. The node made for a `let` has the let's name; any other node is named
  after its unit and a number. Node names do not change while the text does not.

## Types and wildcards

Every value has a type. It comes from a variable, a parameter or an output, from the pin a value goes to, or from
a member path.

- A constant has no type of its own. `1` may be an `int32`, a `float` or a `double`; `(0, 0, 30)` may be a
  `Vector` or a `Rotator`. The place it goes to decides.
- Some nodes have wildcard pins: `If` and `ArrayGetAtIndex` (the nodes behind `c ? a : b` and `a[i]`),
  `GetModuleMetadata` (its `Default`), and others. A wildcard takes its type from the arguments given, then
  from where the result goes: the pin it is linked to, the type of a typed `let`, the variable it is assigned
  to, the output of a function.
- Nothing is guessed. When neither tells the type, it is DAB9003, and the fix is a typed let:

```dcr
let Pick = true ? 1 : 2;           // DAB9003: the numbers could be any type
float Pick = true ? 1 : 2;         // the result is a float, so are the numbers
```

- Arguments that fit no version of the node are DAB9004.
- `float` and `double` are interchangeable where they are linked. When an operator or a template picks a
  version, it takes the one with the fewest swaps between them, then the one with the most doubles.
- Reading a member of a call whose type is still open (`If(c, a, b).X`) is DAB9003. Give the call a type with a
  typed let first.

### Templates

A template is a node that exists in several versions, one for each type: `Add`, `Multiply` and `Scale` are
templates. It is called by the template's name with the spaces left out. A unit or a dispatch of that name is found
first; a template is found when there is none. The arguments pick the version.

- The version is the one whose pins take the arguments: the fewest float and double swaps, then the most
  doubles, then the first. Arguments that fit no version are DAB9004.
- When every version takes the arguments and nothing says which, because no argument has a type and the result
  goes nowhere that tells, the node is kept as a template with no type picked, as the editor keeps a template
  node that nothing is linked to. This is warning DAB9003. The node does nothing until a type is picked. An
  argument for an input whose type depends on the missing choice is DAB9003 as an error.
- The decompiler writes a node by the name of its unit once its version is picked, and uses the template name
  only for a node with no type picked.

## Values

### Constants

| Kind | Written as | Example |
|---|---|---|
| number | digits, a fraction, an exponent; `-` in front for a negative | `12`, `0.5`, `1e-3`, `-0.5` |
| bool | `true`, `false` | `bInterpResult = true` |
| name, string, text | a string; a name may also be an identifier | `Name = "Weight"`, `Name = Weight` |
| enum | the value's name, or the enum's name and the value's | `AdditiveGlobal`, `ERigVMTransformSpace.GlobalSpace` |
| element key | the element's kind, then its name | `Bone(pelvis)`, `Control("foot_l")`, `Connector(Steering)` |
| struct | a tuple | `(X = 1, Z = 3)`, `(0, 0, 30)`, `()` |
| array | a list | `[ 0, 0.5, 1 ]`, `[ Bone(a), Bone(b) ]` |
| asset | its name, `Path(...)`, or `None` | `SKM_Manny_Simple` |
| nothing | `None` | `Path = None` |

Strings sit on one line. Their escapes are `\"`, `\\`, `\n`, `\t` and `\r` (DAB1006 for any other). A name in
backticks is one identifier whose text is what stands between the backticks (DAB1007 when it is empty or not
closed).

### Structs

A struct is a tuple.

- Members are named, `(X = 1, Z = 3)`, or positional in the order the struct declares them, `(0, 0, 30)`. A
  tuple names every member or none (DAB9004).
- Members not written keep their defaults, so `(Translation = (Z = ZOffset_L))` sets only that one.
- `()` is a struct at its defaults.
- A member that the struct does not have, or more positional values than it has members, is DAB9011.
- A one-item tuple needs a name or a trailing comma: `(5)` is `5` in parentheses.
- Members of a user struct go by the name the editor shows.
- A rotation of a transform is a quaternion, in the order `X, Y, Z, W`: `Rotation = (0, 0.707107, 0, 0.707107)`.
  A `Rotator` has the members `Roll`, `Pitch` and `Yaw`.
- A member holding an expression (`Z = ZOffset_L`) is linked; the rest is the pin's value.

### Element keys

`Bone(name)`, `Null(name)`, `Control(name)`, `Curve(name)`, `Connector(name)` and `Socket(name)` are values of the
type `RigElementKey`. The name is an identifier, a name in backticks, or a string; `None` is the key with no name.

When the name is any other expression, it is computed and linked to the key's Name pin:
`Bone(NameConcat(Prefix, "_l"))`.

A key has the members `Name` and `Type`. `Control.Name` is the name of the control a loop is on.

### Path

`Path` names an asset exactly.

```dcr
PreviewMesh = Path(Plugin.ControlRigModules, "Modules/Meshes/SKM_Car_Template");
```

- `Root` is `Game`, `Engine` or `Plugin.<Name>`.
- `"Folder/Asset"` is the object `/<Mount>/Folder/Asset.Asset`.

### Asset names

A bare name such as `SKM_Manny_Simple` finds the registered asset of that name whose class fits the place. None
is DAB9001. More than one is DAB9002 (a list of shape libraries reports it as DAB9001); write `Path(...)`. A
name that is unique today can become ambiguous when another asset of that name and class is added.

## Traits

A unit node can carry traits: named pins of a trait struct that are added to the node after its own pins. The
deformer graph node has one for each variable it sets.

In the text, a trait is an argument whose value calls the trait's struct. The argument's name is the trait's
name on the node. The struct is written without its `RigVMTrait_` prefix, and its arguments are its members.

```dcr
AddOptimusDeformer(
    Transform = SetDeformerTransformVariable(Value = GetTransform(Control(LatticeDeformer))),
    Weight    = SetDeformerFloatVariable(Value = GetFloatAnimationChannel("LatticeDeformer", Channel = "Weight"))
);
```

- An argument is read as a trait when the unit has no input of that name and its value is a call of a trait
  struct. The struct is found by its name without `RigVMTrait_`, or by its full name; a name that finds none, or
  several, is not a trait.
- A member of a trait may be a constant or any expression. An expression is linked to the trait's pin.
- A trait member left out keeps its default.

## Injected nodes

A node can be injected on a pin: it sits on the link into an input, or on an output, and the value passes through
it. The Visual Debug nodes are used this way.

```dcr
Start = Injected(VisualDebugVectorItemSpace((X = FootPos.X, Y = Y, Z = RootPos.Z) + (0, 0, 30))),
```

- `Pin = Injected(Node(...))` injects the node on the input `Pin`. The pin receives the node's output; the
  node's input is written in its call.
- `Injected(Node(Source), Output)` is the form for an output: the node reads `Source`, an output of another
  node, and is injected on that output. The value of the call is the node's output.
- The node must be a unit without execute pins, with an input and an output of the pin's type for the value to
  pass through (DAB9004).
- `Injected(...)` goes on a whole input pin, not on a part of its value (DAB9004).

## Running another event

`RunEvent(Event);` runs another event of the rig where it stands on the execute chain. The argument is the event
as `Event` writes it.

```dcr
if (GetMetadata.Value && GetMetadata.Found)
{
    RunEvent(Backward);
}
```

- It is a node on the execute chain (an Invoke Entry node) and gives no value.
- A user-defined event is named as it is: ``RunEvent(`Old Arm FWD`);``.

## Templates and #include

`#include` and templates work as they do in `.dab` files; see
[Templates and #include](Language.md#templates-and-include). A `.dcr` file includes a template library with
`#include "Path.dat"` before the document.

- A `.dcr` file uses templates but does not define them: they are written in a `.dat` library it includes. A
  `Template` in the ControlRig itself is DAB6011.
- A template is a named expression. It expands in the expressions of code, before any name is checked.
- Each expansion makes its own nodes, nodes with state included.

## Comments and regions

- `// ...` runs to the end of the line. `/* ... */` may span lines and does not nest (DAB1003 when not closed).
- `fmt` keeps comments where they were relative to their statement.
- Comments in the text become comment boxes in the graph:
  - The `//` lines on their own lines above a statement are one box around the nodes the statement makes.
    Each `/* */` comment above a statement is a box of its own.
  - `#Region "Label"` and `#EndRegion` are a box around the nodes made by the statements between them. A
    region that makes no node makes no box. Regions may nest.
  - The comment above an `Event`, a `Function` or a top-level `Detached` block is a box of its own, around no
    node.
  - A comment at the end of a line, or before a closing brace, stays in the text and makes no box.
- `#Region` and `#EndRegion` start at the beginning of a line and pair up inside one block. An unpaired one is
  warning DAB2008.
- A label is a string, so a line break is `\n` in it.
- When decompiling, a comment box becomes a `#Region` if it encloses two or more consecutive statements of
  one block. Otherwise its text becomes `//` lines above the first statement it touches. A second box on the
  same statement is a `/* */` comment. See [Decompiling.md](Decompiling.md#writing-control-rigs-as-text).

```dcr
#Region "Smooth the offsets"
ZOffset_L = AlphaInterp(ZOffset_L_Target, bInterpResult = true);
ZOffset_R = AlphaInterp(ZOffset_R_Target, bInterpResult = true);
#EndRegion
```

The only directives are `#include`, `#Region` and `#EndRegion` (DAB1005).

## What the text cannot hold

Some things of a Control Rig have no place in the text. The decompiler lists what it left out in a comment at
the top of the file, one line for each, with its place in the rig:

```dcr
// Not expressible in the text; kept out of it:
//   RigVMModel/Branch_1: two execute paths lead into this node; it is written once, where the first one reaches it
//   Variables/Poses: the type TArray<FPose> (/Script/Example.Pose)
ControlRig(Name="Rigs/CR_Example", Root="Game")
```

- A thing that stood in a value is written `Unsupported("...")` in its place. The file does not check until you
  replace it: it is DAB9001.
- A thing that stood in the structure of the graph is only in the comment. The file checks and builds without it.

Typical items:

- a node of a kind the text has no call for, and a wildcard pin with no type;
- a value that does not read back as a literal;
- a node that two execute paths lead into: it is written once;
- a second event of the same kind in one graph, and a user event named like a built-in one;
- inputs that depend on their own outputs;
- the modular rig model of a modular rig asset.

These are not kept, and they are not listed: node positions and sizes, node colors, and the expansion of pins.
A build lays the nodes out again.

## Names

- An identifier is a letter or `_`, then letters, digits and `_`. Any Unicode letter counts. Identifiers are
  case-sensitive.
- A name in backticks is one identifier whose text is what stands between the backticks, spaces and case kept.
  It can be written wherever an identifier can: variables, functions, parameters, events, elements, pins,
  members, loop names. `` `Module Settings` ``.
- A name in backticks is never read as a word the parser knows by position (`` `true` `` is a name).
- There are no reserved words in the lexer. At the start of a code statement these words mean something:
  `let`, `local`, `if`, `else`, `for`, `return`, `Detached`, and `in` in the header of a `for`. A variable or a
  pin with one of these names is written in backticks.
- These names mean something where they are written as values: `true`, `false`, `None`, `Path`, `Injected`,
  `Range`, `RunEvent`, `Unsupported`, and the element words `Bone`, `Null`, `Control`, `Curve`, `Connector`,
  `Socket`. A function of the rig with the name `Injected`, `Range` or `RunEvent` hides the word.
- At the top of the document `Event`, `Function` and `Detached` start a block. In `Hierarchy`, `Bone`, `Null`,
  `Control`, `Curve`, `Connector` and `Socket` do.

## Canonical format

`Tools/dab.ps1 fmt` prints each file in its canonical form; `-Write` rewrites the files that differ and
`-Verify` fails on any file that is not canonical. The rules of `.dab` files apply (four spaces, 110 columns,
`=` aligned, calls broken one argument per line). For code:

- `Event`, `Function` and `Detached`, and `if`, `else` and `for`, open their `{` on a line of their own, at
  their own indent.
- One statement per line, each ending with `;` (`if`, `for`, `Detached` and a call with blocks end with their block).
- The blocks after a call open their `{` on a line of their own at the call's indent; each output's name is on a
  line of its own, its block's `{` under it.
- A function is `Function Name(Type a, Type b = 1) -> (Type c) [ meta ]` on one line, then the block; or the
  head, ` = value;` on the same line when it fits.
- A run of `let`, typed `let` and `local` statements lines up its `=`; so does a run of assignments.
- A name that is not a plain identifier, or is a code word, prints in backticks.
- A type with arguments prints as `Array<RigElementKey>`.

The full rules are in [Grammar.md](Grammar.md#canonical-format-of-code).
