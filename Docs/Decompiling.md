# Decompiling an Animation Blueprint

The decompiler reads an Animation Blueprint, or an Animation Layer Interface, and writes the text that
builds it. Use it to bring an existing character to DreamAnim; from then on the text is the source.

- [Running it](#running-it)
- [What you get](#what-you-get)
- [What gets renamed, and why](#what-gets-renamed-and-why)
- [What cannot be expressed](#what-cannot-be-expressed)
- [Forwarder functions become calls](#forwarder-functions-become-calls)
- [Asset names](#asset-names)
- [Checking the result](#checking-the-result)
- [Migrating a character safely](#migrating-a-character-safely)

## Running it

```
Tools/dab.ps1 decompile -Asset /ALS/ALS/Character/AnimationInstances/AB_Als_Head
Tools/dab.ps1 decompile -Under /ALS/ALS/Character
Tools/dab.ps1 decompile -Asset /Game/Characters/AB_Hero -Stdout
```

| Option | Meaning |
|---|---|
| `-Asset A[,B]` | One or more Blueprints, comma-separated: a package path (`/Game/Characters/AB_Hero`), an object path (`/Game/Characters/AB_Hero.AB_Hero`) or a copied reference (`AnimBlueprint'/Game/Characters/AB_Hero.AB_Hero'`). |
| `-Under <content path>` | Every Animation Blueprint and layer interface under a content folder. The path starts with a mount: `/Game/...`, `/ALS/...`. |
| `-Out <dir or file>` | Where to write, relative to the project. A folder takes the same layout as `DAnim/`; a file is allowed for one Blueprint only. |
| `-Stdout` | Print the text instead of writing files. |
| `-Force` | Replace a file that exists and differs. |
| `-Json <file>` | Also write the diagnostics as JSON. |
| `-Project <uproject>` | The project to open; the default is the plugin's test host. |

Without `-Out` or `-Stdout`, each file goes under the project's `DAnim/` folder, mirroring the package
path. Plugin content keeps its mount name:

| Blueprint | Text |
|---|---|
| `/Game/Characters/AB_Hero` | `DAnim/Characters/AB_Hero.dab` |
| `/ALS/ALS/Character/AB_Als` | `DAnim/ALS/ALS/Character/AB_Als.dab` |
| `/ALS/ALS/Character/ALI_Overlay` | `DAnim/ALS/ALS/Character/ALI_Overlay.dai` |

`DAnim/` is also where sources live, so the decompiler never replaces a file there by surprise. When the
target file already holds the same text, it is left alone and reported unchanged. When it holds
different text (perhaps a source edited by hand), that is an error unless `-Force` is given.

The decompiler reads the Blueprint's compiled defaults. Compile a Blueprint before decompiling it. One
that has neither a generated class nor a parent class cannot be read (DAB8101).

Things the language cannot express are not errors. The run reports how many there were, and exits 0
when nothing else went wrong. Read the comment at the top of each file; see
[What cannot be expressed](#what-cannot-be-expressed).

In the editor, a Content Browser action to decompile the selected Blueprints is coming.

## What you get

The text is in the canonical format, so `fmt` leaves it as it is. Comments at the top list what was
renamed and what was left out:

```dab
// Renamed from the Blueprint:
//   CrouchingAlias <- "Crouching Alias"
//   CrouchingToStanding <- "Crouching To Standing"
//   EntryConduit <- "Entry Conduit"
//   GroundedStates <- "Grounded States"
//   StandingAlias <- "Standing Alias"
//   StandingToCrouching <- "Standing To Crouching"
AnimBlueprint(Name="ALS/Character/AnimationInstances/AB_Als_Grounded", Root="Plugin.ALS")
{
    // ...
}
```

The header's `Name` and `Root` are the Blueprint's own path. Building the file without changing them
rebuilds that very asset.

What becomes what:

| In the Blueprint | In the text |
|---|---|
| parent class, skeleton, interfaces, Class Settings | `Settings` |
| class defaults that differ from the parent's | `Defaults` |
| member variables | `Properties`, with their category as `Group` |
| DreamAnim slot getters | `Assets` |
| `AnimNotify_` events made only of calls | `Notifies` |
| the AnimGraph | `Graph` |
| animation layers | `Layer` |
| state machines | `StateMachine` |
| state and node functions, Call Function nodes | calls |

The decompiler also makes these choices, in order of preference:

1. When any path starts with `Self.GetParent()`, it declares `Aliases = { Main = Self.GetParent(); }`
   and writes those paths as `Main.`.
2. Each Save Cached Pose node becomes a named pose `<Cache> = ... [ Cached ];` in the graph that holds
   it. Its uses become its name.
3. A chain of nodes with one pose input each becomes a stack: `{ Source; Step; Step; }`.
4. Other nodes are written inline as arguments. A pose argument that is a stack, or that would nest
   more than three calls deep, is pulled out into a named pose. The name comes from the pin it feeds
   (`Locomotion Input` gives `Locomotion`), else the node's `Tag`, else the node type and a number
   (`MultiWayBlend1`, `Slot1`).
5. An asset is written by its bare name when no other registered asset has that name.
6. The shorthands of the language are used where they give back exactly the same values: curve
   arguments on `ModifyCurve`, keyed blend maps, one value for a per-pose array whose entries are all
   equal, `ExplicitFrame` without `bUseExplicitFrame`.
7. Engine names become identifiers; see below.

## What gets renamed, and why

The engine allows any characters in the names of state machines, states, conduits, aliases, shared
rules, cached poses and layers. The text names them with identifiers, so the decompiler changes them:

- characters other than letters, digits and `_` are dropped, and the letter after them is upper-cased:
  `Rotate Left` becomes `RotateLeft`, `(N) RB Movement` becomes `NRBMovement`;
- a name that clashes with one already taken gets `_2`, `_3`, ... In AB_Als_Camera the cache
  `Look States` becomes `LookStates_2`, because the state machine is `LookStates`;
- the original names are listed in the comment at the top of the file.

The `Layer` of a linked layer node that runs a layer of the same Blueprint is renamed with that layer:
`Grounded Locomotion Mode` is written `Layer = "GroundedLocomotionMode"`.

The rebuilt Blueprint uses the new names. C++ that finds a state, a machine or a layer by its name
(state weights by name, for example) must use the new name.

Input pose names are not renamed. The engine finds a linked graph's input poses by these names at run
time, so they stay as the engine has them, written as strings when they are not identifiers:

```dab
Inputs = [ "Standing Input", "Crouching Input" ];
Crouching = Input("Crouching Input") [ Cached ];
```

```dab
Inputs = { "Locomotion Input" = Locomotion; "Overlay Input" = OverlayPose; }
```

## What cannot be expressed

DreamAnim keeps logic in C++. Blueprint logic has no place in the text, and neither do a few engine
features. The decompiler lists everything it could not express in a comment at the top of the file,
one line per item, with its place in the Blueprint:

```dab
// Not expressible in DreamAnim; move into C++:
//   EventGraph/K2Node_Event_1: the event BlueprintUpdateAnimation is Blueprint logic; move it to C++
//   EventGraph/K2Node_CallFunction_2: calls AnimInstance::GetOwningActor outside an AnimNotify event
//   EventGraph/K2Node_CallFunction_1: calls AlsUtility::GetAnimationCurveValueFromCharacter outside an AnimNotify event
//   EventGraph/K2Node_DynamicCast_0: a K2Node_DynamicCast node outside an AnimNotify event
//   EventGraph/K2Node_VariableSet_0: a K2Node_VariableSet node outside an AnimNotify event
//   EventGraph/K2Node_CallFunction_3: calls AlsConstants::PoseAimingCurveName outside an AnimNotify event
AnimBlueprint(Name="ALS/OverlayObjects/Bow/AB_Als_Bow", Root="Plugin.ALS")
```

Typical items:

- event graph logic: any event other than an `AnimNotify_` event, and any node outside one; inside a
  notify event, anything that is not a call;
- Blueprint functions that do more than forward a call, macros, event dispatchers, interface functions
  that are not animation layers;
- Call Function nodes that call a library function instead of a function of the instance;
- nodes that hold a nested graph, custom blend graphs on transitions, bindings on state machine and
  result nodes, state "fully blended out" functions;
- property paths that index an array, fixed-size arrays, maps with unsupported key types;
- variables without a compiled default (compile the Blueprint first).

There are two kinds of loss:

- When the lost thing was a value inside the graph (an argument, a binding, a condition), the text holds
  `Unsupported("...")` in its place. The file checks, but it does not build (DAB7002) until you replace
  the placeholder.
- When the lost thing is logic outside the graph (an event graph, a Blueprint function), the text
  does not have it. The file builds, and the rebuilt Blueprint does not have it either: a build empties
  the event graph and replaces the functions.

So move every listed item into the C++ anim instance (`NativeUpdateAnimation`,
`NativeThreadSafeUpdateAnimation`, functions the graph calls), then decompile again. Repeat until the
list at the top holds nothing you need.

## Forwarder functions become calls

Blueprints like ALS-Refactored's call C++ through small Blueprint functions: an entry, a getter such
as `GetParent()`, and one call. State functions and node functions point at them. The decompiler
recognises such a function and writes the call it makes:

```dab
State StandingToCrouching
{
    OnEnter = Main.StopTransitionAndTurnInPlaceAnimations();

    Pose = SequencePlayer(Sequence = A_Als_StandToCrouch, PlayRate = 1.2, bLoopAnimation = false);
}
```

The forwarding function itself does not appear in the text. When the file is built, the generator calls
the C++ function directly when it can, and otherwise writes a forwarder of its own,
`DreamAnimCall_<Function>_<hash>`.

A Blueprint function that does anything more than forward one call is listed as not expressible, with
the reason. So is a Blueprint function that no state or node function uses.

## Asset names

An asset is written by its bare name when no other registered asset has that name:

```dab
Sequence = A_Als_StandToCrouch
```

Otherwise it is written with `Path(...)`. Objects inside an asset, such as a blend profile inside a
skeleton, always are:

```dab
BlendProfile = Path(Plugin.ALS, "ALS/Character/SK_Als:QuickFeetBlend")
```

ALS-Refactored has two Blueprints named `AB_Als_Bow` (the bow overlay and the bow object), so a
reference to either is written with `Path(...)`.

Bare names are looked up again on every check. If another asset with the same name and a fitting class
is added later, the name becomes ambiguous (DAB3007) and needs a `Path(...)`.

## Checking the result

```
Tools/dab.ps1 check -File DAnim/ALS/ALS/Character/AnimationInstances/AB_Als_Head.dab
Tools/dab.ps1 build -File DAnim/ALS/ALS/Character/AnimationInstances/AB_Als_Head.dab -NoSave
```

- `check` parses and checks without writing anything: one source with `-File`, every source without it.
- `build -NoSave` also generates and compiles the Blueprint, and keeps it in memory only. It shows
  whether the engine accepts the result without touching the asset on disk.
- Names are looked up among assets that exist. A file that names a Blueprint which only exists as text
  so far reports DAB3004 until that Blueprint is built.

The automated tests run every ALS-Refactored Blueprint through the whole loop: extract the model,
decompile, check the text, generate a new Blueprint from it, extract that one. All 26 Blueprints give
the same model at every turn.

## Migrating a character safely

1. **Move the Blueprint logic to C++ first.** Decompile, read the "Not expressible" list, move each item
   into C++, and decompile again until the list holds nothing you need.
2. **Update C++ that uses renamed names.** States, machines and layers come back under their identifier
   names.
3. **Commit or back up the content.** Building a decompiled file rebuilds the original asset in place.
   Its graphs, functions, variables and event graph are replaced.
4. **Decompile every Blueprint of the character together**: the main Blueprint, every Blueprint it links,
   every layer interface and every Blueprint that implements one. `-Under` on the character's folder does
   this.
5. **Build them all together**, in one `build` run without `-File`, with no editor open on the project.
   The build orders the sources: interfaces first, and every document after the DreamAnim assets it
   names.
6. Run the game and compare.

Why everything together:

- A `LinkedAnimGraph` or `LinkedAnimLayer` node feeds its pose pins to the target's input poses by
  position. A Blueprint compiled against another version of its target can feed the wrong poses.
  Rebuilding all of them at once keeps every link consistent.
- Checking a linked graph node used as a stack step loads its target to find its free input. The target
  has to exist when the Blueprint that links it is checked.
- Generated copies must reference generated copies. The text names other Blueprints by asset name, and
  a bare name finds the registered asset of that name. Building in place keeps every reference right.
  If you build into copies instead (a new `Name` in each header), point every reference at the copies,
  in the text and in C++. A copy with a new asset name that is still referred to by the old name links
  the original. A copy that keeps the asset name in another folder makes the bare name ambiguous
  (DAB3007); write `Path(...)` to the copy.

The automated tests do exactly this for ALS-Refactored. They regenerate the character's Blueprints into
copies, point the main copy at the copies of its linked graphs and the character's overlay map at the
copies of the overlay layers, and drive the character through a 420-frame script. Every bone matches the
original on every frame, to within 0.01 cm and 0.01 degrees.

After migrating, edit the text, not the Blueprint. Editor changes are not written back to the text yet,
and the next build replaces them.
