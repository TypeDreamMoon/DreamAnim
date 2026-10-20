# DreamAnim diagnostics

Every problem DreamAnim finds in a source or a Blueprint has a code `DABnnnn`, a severity, a message and
a position. Many also carry a hint that says how to fix them.

The command line prints one line per diagnostic, in the form Visual Studio and VS Code problem matchers
read, with the hint on an indented line below it:

```
I:/MyGame/DAnim/Characters/AB_Hero.dab(12,24): error DAB6003: There is no pose `Lok` in Graph.
    hint: A name in a pose is a named pose of this graph, a cached pose or a state machine.
```

`-Json <file>` (on `build`, `check`, `fmt` and `decompile`) writes the same diagnostics as JSON, hint
included:

```
[{"file": ..., "line": ..., "column": ..., "length": ..., "severity": "error", "code": "DAB6003", "message": ..., "hint": ...}]
```

Line and column are 1-based. A diagnostic that has no place in the text (the generator's, the Blueprint
compiler's) points at line 1, column 1 of the source.

The exit code is 0 when there are no errors and 1 when there are; warnings do not change it. It is 2 when
the run did not finish (a crash or a timeout).

A few problems belong to the run, not to any text: a `-File` that does not exist or is not a source, a
file that cannot be written, options that contradict each other, a Blueprint that cannot be loaded for
`decompile`, a decompiled file that would replace a different one without `-Force`, and a file
`fmt -Verify` finds not canonical. They are printed as errors without a code, and they count as errors.

| Severity | Meaning |
|---|---|
| error | The file cannot be built as written. |
| warning | The file builds, but something is probably wrong. |

The checker keeps going after an error, so one run reports as much as it can. When one mistake causes
several errors, fix the first one and check again.

| Range | Raised by |
|---|---|
| [DAB1xxx](#dab1xxx-characters) | the lexer: characters, strings, comments, numbers, directives |
| [DAB2xxx](#dab2xxx-syntax) | the parser: statements, blocks, arguments, metadata |
| [DAB31xx](#dab31xx-templates) | template expansion |
| [DAB32xx](#dab32xx-includes) | `#include` |
| [DAB30xx](#dab30xx-names-and-types) | the checker: names of node types, classes and assets |
| [DAB4xxx](#dab4xxx-values-and-arguments) | the checker: values, node arguments, settings, metadata |
| [DAB5xxx](#dab5xxx-paths-calls-and-aliases) | the checker: paths, calls, aliases, `Props` and `Assets` |
| [DAB6xxx](#dab6xxx-structure) | the checker: graphs, poses, state machines, sections |
| [DAB7xxx](#dab7xxx-generator) | the generator, while it writes the Blueprint, and the Blueprint compiler (DAB7100, DAB7101) |
| [DAB8xxx](#dab8xxx-decompiling-and-write-back) | write-back (DAB80xx) and the decompiler (DAB81xx) |
| [DAB9xxx](#dab9xxx-control-rig) | the checker for `ControlRig` documents (`.dcr`): names, types, statements, the hierarchy, functions (DAB90xx); the generator and the decompiler for Control Rigs (DAB91xx) |

Language rules are in [Language.md](Language.md).

## DAB1xxx: characters

The lexer turns text into tokens. These errors point at the character where reading failed.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB1001 | error | A character that cannot start any token, or a `#` that is not a directive at the start of a line. | Remove it. Write `&&` for `&` and `\|\|` for `\|`; strings use double quotes; a directive starts its line. |
| DAB1002 | error | The string is not closed on its line. | Add the closing `"`. A string cannot span lines; write `\n` for a line break. |
| DAB1003 | error | The block comment is not closed; `*/` is expected. | Add `*/`. Everything after `/*` is inside the comment, so errors that follow may come from this one. |
| DAB1004 | error | A malformed number. | Write numbers as `12`, `0.5`, `.5`, `1e-3` or `2.5E+4`, without a suffix. |
| DAB1005 | error | An unknown directive. The directives are `#include`, `#Region` and `#EndRegion`. | Fix the spelling; the hint suggests the right one when only the case differs. |
| DAB1006 | error | An unknown escape in a string. | Use `\"`, `\\`, `\n`, `\t` or `\r`. Write a backslash as `\\`. |
| DAB1007 | error | A name in backticks that is empty or not closed on its line. | Write the whole name between two backticks on one line: `` `Module Settings` ``. A name cannot be empty or contain a backtick. |

## DAB2xxx: syntax

The parser checks the shape of the file. The grammar is in [Grammar.md](Grammar.md).

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB2001 | error | Expected one thing, found another. The message names what was expected: `;`, `)`, a name after `State`, `{`, `=` or `;` after a named block, ... | Add or remove what the message names. A missing `;` at the end of a statement is the common case. Only a `Template` (and, in a `ControlRig`, a `Function`) takes parameters. |
| DAB2002 | error | The file has no document. | Write `AnimBlueprint(Name="...", Root="...") { ... }`, `AnimLayerInterface(...)`, `AnimTemplates(...)` or `ControlRig(...)`. |
| DAB2003 | error | The file already has a document; a file holds exactly one. | Move the second document to a file of its own. |
| DAB2004 | error | An unknown document kind. | Use `AnimBlueprint`, `AnimLayerInterface`, `AnimTemplates` or `ControlRig`. |
| DAB2005 | error | The header has no `Name`, has an argument other than `Name` and `Root`, has a positional argument, or a value that is not a string. | Write `AnimBlueprint(Name="Folder/Asset", Root="Game")`. |
| DAB2006 | error | An expression was expected. | Give the missing value. |
| DAB2007 | error | A bracket, brace or parenthesis is not closed before the end of the file. The message points at the opening one. | Add the matching closer. |
| DAB2008 | warning | A `#Region` or `#EndRegion` without its partner in the same block. | Pair them inside one block. |
| DAB2009 | error | `#include` after the document, or inside it. | Move the line to the top of the file. |
| DAB2010 | error | A block that holds a bare expression, or a stack that holds a statement. | Make every entry a statement (`Key = Value;`) for a block, or every entry an expression followed by `;` for a stack. |
| DAB2011 | error | The same argument name twice in one argument list. | Remove one. |
| DAB2012 | error | A positional argument after a named one. | Put positional arguments first, or name this one. |
| DAB2013 | error | Malformed metadata. | Write `[ Key = Value; Flag ]`, entries separated by `;` or `,`. |
| DAB2014 | error | A template parameter declared twice, or a parameter without a default after one with a default. A function's parameter or output declared twice. | Rename the duplicate. Put a template's parameters with defaults last. |
| DAB2015 | error | Metadata after a code statement: a `[` followed by `Key =`, `Key;` or `Key,` in the code of an `Event`, `Function` or `Detached` block. Inside code a `[` after an expression is an index. | Move the metadata to the `Event` or `Function`, or to the variable's declaration. |
| DAB2016 | error | An `else` with no `if` before it. | An `else` follows the `}` of an `if` or of an `else if`. Remove it or add the `if`. |
| DAB2017 | error | A malformed `for` header: no name before `in`, more than four names, or no `in` after the names. | Write `for (Element, Index, Count, Ratio in Items) { ... }`: one to four names, then `in`. |
| DAB2018 | error | A `return` whose value is not a tuple of named items. | Write `return (Name = value, ...);` with the name of each output, or `return;`. |

## DAB31xx: templates

Templates expand after parsing and before any name is checked. A diagnostic in an expanded template
points at the call.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB3101 | error | A template call misses an argument for a parameter that has no default. | Pass the argument, or give the parameter a default. |
| DAB3102 | error | A template call names a parameter the template does not have, or gives one twice. | Use the template's parameter names, each once. |
| DAB3103 | error | A template call has more arguments than the template has parameters. | Remove the extra arguments. |
| DAB3104 | error | Template expansion is more than 16 levels deep; the template probably calls itself. | Remove the recursion. |
| DAB3105 | error | Two templates with the same name, in the file and its includes together. The message says where the first one is. | Rename one of them. |
| DAB3106 | error | A template written without `= expression;`: a block body, or no body. | Write `Template Name(...) = expression;`. |

## DAB32xx: includes

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB3201 | error | The included file cannot be found. | The path is relative to the including file's folder, then to each `DAnim/` source root. Fix the path. |
| DAB3202 | error | The file includes itself through a cycle. The message lists the chain. | Break the cycle. Move templates both files need into a third library. |
| DAB3203 | error | The included file is not a template library. | Include only files that hold an `AnimTemplates(...)` document. |

## DAB30xx: names and types

The checker resolves every name against the engine's reflection and the asset registry. Codes
DAB3001-DAB6099 come from this pass; [Mapping.md](Mapping.md) lists them with the rules they enforce.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB3001 | error | There is no anim node type of that name, the class is abstract, its module is not loaded, or it has no runtime node. | Use the editor node class name without `AnimGraphNode_` (`TwoWayBlend`, `SequencePlayer`). Enable the plugin that defines the node. |
| DAB3002 | error | A node type name that several node classes end in. The message lists them. | Write the full class name, such as `AlsAnimGraphNode_GameplayTagsBlend(...)`. |
| DAB3003 | error | `Parent` does not derive from AnimInstance, or, for an interface, from AnimLayerInterface. | Name an anim instance class, or `AnimLayerInterface` for an interface. |
| DAB3004 | error | There is no asset, Blueprint or class of that name, or the parent Blueprint cannot be loaded. | Check the spelling, or write `Path(Root, "Folder/Name")`. Build the Blueprint the name refers to first. |
| DAB3005 | error | An asset slot's type is not an animation asset class. | Use `AnimSequence`, `BlendSpace`, `AnimMontage` or another animation asset class. |
| DAB3006 | error | A malformed `Path(Root, "Rel")`, or a header `Root` that is not `Game`, `Engine` or `Plugin.<Name>`. | Write `Path(Plugin.ALS, "ALS/Character/SK_Als")`: a root, then the path as a string. |
| DAB3007 | error | More than one asset has that name. | Write `Path(...)` to say which one. |
| DAB3008 | warning | A tuning value's type is not understood; its default is kept as written. | Spell the type as `float`, `int`, `int64`, `bool`, `FName`, `FString`, `FText`, or an enum, struct or class name. |

## DAB4xxx: values and arguments

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB4001 | error | A `Settings` key that is not a class setting of UAnimBlueprint. | Use `Parent`, `Skeleton`, `Implements`, `DefaultAnimSet` or a Class Settings property (`bUseMultiThreadedAnimationUpdate`, ...). |
| DAB4002 | error | Not a value of the enum (a property value, a `BlendPose` key, a `BlendTime` key), or no such enum. | Use an entry the hint lists. Name a C++ enum (`EAlsHipsDirection`), or a user enum by `Path(...)`. Key a per-pose array the way `BlendPose` is keyed. |
| DAB4003 | warning | A gameplay tag that is not registered. | Add it in Project Settings > GameplayTags, or fix its spelling. |
| DAB4004 | error | The node has no argument of that name; or the argument is filled in by the editor or the compiler; or a linked graph or layer node is given a list of poses; or the node lacks that property in this engine version. | Use the fields of the node's runtime struct and the editor node's settings (`Tag`, ...). Give a linked node's poses as `Inputs = { Name = Pose; }`. |
| DAB4005 | error | Unknown metadata or transition option, or metadata where none is taken (`Graph`, asset slots, settings). | Use a setting of the node, state, machine or transition. Remove metadata from `Graph`, slots and settings; layer settings go on the `Layer`. |
| DAB4006 | error | A value that does not fit the property's type. The message says what it takes. Also: a flag without a value on a setting that is not a bool, a pose array not written as a list, `Inputs` not written as a block, a fixed-size array field, a struct field not written `Field = Value`. | Write a value of the type the message names. |
| DAB4007 | error | A struct has no such field, or more positional values than fields. | Use the struct's field names. Name the fields when in doubt. |
| DAB4008 | error | A positional argument where arguments are named. | Name it. Node arguments are named; among the built-in nodes, `Input`, `Slot` and `Call` take positional arguments, and `Slot` takes one, its name. |
| DAB4009 | error | A built-in written wrong: `Input` without its name, `Call` without its function, `On` without a site, `bones(...)` outside `LayerSetup` or with something that is not a bone. | `Input("Name")`, `Call(Self.Function)`, `On = BecomeRelevant`, `LayerSetup = [ bones(thigh_l, "VB foot_l", Depth = 1) ]`. |
| DAB4010 | error | A value read at run time where only a constant fits: settings, defaults, metadata, transition options, editor-node settings. | Write a constant, or `Props.X` for a tuning value's default. An asset slot cannot go there; write `Path(...)` to the asset, or move the value to a node argument that can be bound. |
| DAB4011 | error | The same field, key, option or node function given twice, or given both directly and by the shorthand that implies it. | Give it once. With a keyed `BlendPose`, leave out `Tags` and `VisibleEnumEntries`. Give `ModifyCurve` curves either as arguments or as `CurveNames` / `CurveValues`. |
| DAB4012 | error | `BlendListByEnum` whose enum is neither written as `BoundEnum` nor the type of the path bound to `ActiveEnumValue`. | Write `BoundEnum = EnumName`, or bind `ActiveEnumValue` to a path of that enum's type. |
| DAB4013 | warning | `Inputs` names an input pose the linked graph or layer's target does not have. The hint lists the ones it has. | Use one of the listed names. When the target's inputs changed, update the file. |
| DAB4014 | error | A `Defaults` entry that is not an editable property of the parent class (AnimInstance when `Settings` names no `Parent`), or not written `Property = Value;`. | Use an editable property of the parent class, as the Class Defaults panel shows it. |

## DAB5xxx: paths, calls and aliases

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB5001 | error | A call that is not `Target.Function(...)`, or whose target is not `Self` or a path from it. | Write `Self.Refresh()`, or through an alias, `Main.PlayAnimation(...)`. Declare the alias in `Aliases`. |
| DAB5002 | error | A call argument that is computed. Arguments are constants, `Props.X` and `Assets.X`. | Give the C++ function no such parameter and let it read the value itself. |
| DAB5003 | error | `Props.X` or `Assets.X` names a tuning value or slot that is not declared. | Declare it: `Properties = { float X = 0; }` or `Assets = { AnimSequence X; }`. |
| DAB5004 | warning | The call's target class has no such function, or the function has no such parameter. | Check the spelling. When the function really does not exist, the build fails. |
| DAB5005 | error | A block or a stack where a value is expected. | Values are constants, paths such as `Main.Speed`, `Props.X`, `Assets.X`, or expressions of them. |
| DAB5006 | error | `Props.X` where only a constant fits, but `X` has no default. | Give it a default: `float X = 0.3;`. |
| DAB5007 | error | An alias that is not a path from `Self`, or an alias named `Self`, `Props`, `Assets` or `None`. | Write `Main = Self.GetParent();`, with another name if needed. |

## DAB6xxx: structure

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB6001 | error | A stack step without exactly one free pose input; a name as a later step; or a linked graph or layer step whose target cannot be loaded to tell. | Give the other inputs as arguments, put the name first, or write the node outside the stack. For a linked node, build its target first, or give its inputs in `Inputs = { ... }` outside a stack. |
| DAB6002 | error | A state machine used more than once, or never. | Name it as a pose in exactly one place. To use its pose elsewhere, cache it where it runs. |
| DAB6003 | error | A name in a pose that is no named pose, cache or state machine visible there. | Fix the name. A named pose of another graph is not visible; mark it `[ Cached ]` there to use it here. |
| DAB6004 | warning | A named pose that is never used, in a graph or state that has its result. Its nodes are still made, unlinked. | Use it, or delete it. |
| DAB6005 | error | A named pose without `[ Cached ]` used more than once. | Mark it `Name = ... [ Cached ];`. |
| DAB6006 | error | An `Entry`, transition end or alias member that is not a state or conduit of the machine; an alias as a transition target. | Name a state or conduit of this machine. An alias may only be a transition's source. |
| DAB6007 | error | A state machine without `Entry`. | Add `Entry = State;`. |
| DAB6008 | error | A name declared twice: a section, pose, cache, machine, state, rule, layer, alias, tuning value, slot, notify or setting; or a cache named like a state machine. | Rename or remove one. Cache names are shared by the whole Blueprint. |
| DAB6009 | error | A pose that uses itself. | Break the loop. |
| DAB6010 | error | An expression that is not a pose, or an empty stack. | A pose is a node, a stack, a named pose, a cache, a state machine's name, or `None`. |
| DAB6011 | error | A statement or section that does not belong where it is written. The message says what belongs there. | Move or rewrite it. State machines are written at the document level and named where they run. |
| DAB6012 | error | A graph or state without its result. | Add `Output = ...;` to a graph or layer, `Pose = ...;` to a state. Write `Output = None;` for a graph that outputs nothing. |
| DAB6013 | error | A shared rule that uses itself. | Break the loop between the rules. |
| DAB6014 | error | Nothing to build: the source file cannot be read. | Check that the file exists and is not locked by another program. |
| DAB6015 | error | Something an AnimLayerInterface cannot hold: a graph, a layer body, a state machine, notifies, asset slots. | An interface only declares layers: `Layer Name [ meta ];`. Write the layers' poses in the Blueprints that implement it. |
| DAB6016 | error | Unknown metadata on a pose statement, `Cached` with a value, `Cached` on the result, a node function without a call, or a node function with no node to go on. | A named pose takes `Cached`, `OnInitialUpdate`, `OnBecomeRelevant` and `OnUpdate`. Cache a named pose and output its name. |
| DAB6017 | error | A cached pose is used outside the graph that saves it: in another layer, or in the AnimGraph when a layer saves it. The engine's compiler reads a cache only in the AnimGraph or layer that saves it and in the states of the machines that graph runs. | Save a cache of that pose in the graph that uses it, or give the layer the pose as an input (`Input(...)`). |

## DAB7xxx: generator

The generator writes the Blueprint from the checked model; it runs only when the file checks without
errors. Its diagnostics have no position in the text: they name the source file at line 1, column 1.
Their message starts with a place inside the Blueprint (`AnimGraph/...`, `Layer/Overlay/...`,
`Settings:`, `EventGraph:`) and says what could not be done.

The checker catches most mistakes first. A generator error on a file that checks clean usually means the
engine refused something the checker allowed.

After the generator, the engine's Blueprint compiler compiles the result. Its messages come back as
DAB7100 and DAB7101, with the compiler's text after `The Blueprint compiler:`. A compiler error fails the
build like any other error.

Each message is placed on the text that made the node it names: the node's call, or its named pose. A
message about a node the generator added inside a transition's rule or a state is placed on that
transition's row or that state. One about a function the generator wrote (for an expression, a call, a
slot), or that names no node, is placed at the top of the file.

When there are errors, a Blueprint that exists already keeps what it had: the build first compiles a
candidate in a temporary package and touches the asset only when that compiles. Its animation set and
the text it was last built from stay as they were too. A new Blueprint is created and left unsaved. An
error that only the real asset shows (rare, such as a Blueprint whose parent was another Animation
Blueprint and now is not) leaves it rebuilt but unsaved.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB7001 | error | A node, transition or graph has no property or pin the model names: "has no property", "has no pin to bind", "the pin cannot be bound", "has no parameter", "a node has no Update function". | Check the argument against the node in this engine version. |
| DAB7002 | error | A value, binding, call, condition or variable default cannot be generated. This includes the decompiler's `Unsupported(...)` placeholder, a function the language does not know, a call with arguments inside an expression, a pure function as a call, and a transition without a condition. | Replace `Unsupported(...)` and move that logic into C++. Use only the functions in [Language.md](Language.md#functions). Give every transition a condition. |
| DAB7003 | error | A value does not fit the pin or property it is written to (`Tag = ...: expects a name`, `BlendProfile = ...: expects Path(...) to a blend profile`, a variable default). | Write a value of the expected type. |
| DAB7004 | error | Something the Blueprint refers to is missing or of the wrong kind: a parent class a Blueprint cannot derive from, a skeleton that is not a skeleton, a `DefaultAnimSet` that is not a DreamAnim animation set, an interface not yet built, an asset slot, a function not on the class, a node class. | Build interfaces and linked Blueprints before the Blueprints that use them; a `build` of all sources orders them so. Check the paths in `Settings`. |
| DAB7005 | error | The Blueprint cannot be built as the model describes: a variable whose name is taken, an interface that cannot be implemented, a `Graph` in a Blueprint whose parent is an Animation Blueprint, a node binding DreamAnim cannot write, two states or caches of one name, a link that cannot be made. | Rename what clashes with the parent class. Remove `Graph` from a Blueprint whose parent is an Animation Blueprint. |
| DAB7006 | error | The asset cannot be created or saved: an invalid asset path, an asset of another kind at that path, a package that cannot be created, a save that fails, a document that is not an Animation Blueprint or interface. | Check `Name` and `Root` in the header. Move or rename the asset in the way. Make the package file writable, and close any editor that has the project open. |
| DAB7100 | error | The Blueprint compiler reported an error on the generated Blueprint. The message carries the compiler's text. | Read the compiler's message, and fix the text or the C++ it points at. |
| DAB7101 | warning | The Blueprint compiler reported a warning on the generated Blueprint. | Read the compiler's message; the Blueprint was built. |

## DAB8xxx: decompiling and write-back

### Decompiling

Turning an Animation Blueprint into text is described in [Decompiling.md](Decompiling.md). What the
language cannot express is not an error: it is listed in a comment at the top of the decompiled file.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB8101 | error | The Blueprint has neither a generated class nor a parent class, so there is nothing to read its paths against. | Open the Blueprint in the editor and compile it, or fix its parent class. |
| DAB8102 | error | Decompiling with `-AnimSet`: the set cannot be made or saved. The path is not an asset path, an asset of another kind is there, or the package cannot be saved. | Give an asset path such as `/Game/Characters/DAS_Hero`, move the asset in the way, or make the package writable. |
| DAB8103 | warning | Decompiling with `-AnimSet`: a slot of the set already holds another animation than the one the Blueprint used. The set keeps its animation. | Nothing, if the set was filled on purpose. Otherwise pick the animation in the set's slot. |

### Write-back

Write-back takes what was changed in the editor on a Blueprint generated from text, and writes it into
that text. When it cannot write a change, it refuses it and says why. Every write-back diagnostic is a
warning: for a refused change, the Blueprint keeps the change and the text keeps what it said.

[WriteBack.md](WriteBack.md) describes the workflow in the editor: what is written and how, templates,
conflicts and node positions.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB8001 | warning | A changed value belongs to a node written inside a template: the template has no place for it, or the value is written in the template's body rather than passed to it. | Expand the template at that place, make the value a template parameter, or change the template. |
| DAB8002 | warning | The text has errors, so the editor's changes cannot be compared with it. The text's own errors are listed too. | Fix the errors in the text. The Blueprint keeps the changes until then. |
| DAB8003 | warning | A structural change made in the editor (nodes or links, how a node's poses are wired, states or transitions added or removed) while writing structure back is turned off. | Make the change in the text, or turn on writing structural changes back. |
| DAB8004 | warning | A block was rewritten from the Blueprint to carry a structural change, and the template calls in it were written out as their expansions. | Put the template calls back by hand if you want them. |
| DAB8005 | warning | After writing, the text still differs from the Blueprint. The message names the first difference. | Rebuild the Blueprint from the text to drop the rest, or make the change in the text. |
| DAB8006 | warning | A change the text cannot hold or write-back cannot place: something the language cannot express, node functions changed on a node that has no name in the text, poses written in a way write-back cannot follow, or a changed graph or machine whose place in the text was not found. | Move that logic into C++. Write the node as a named pose (`Name = Node(...) [ OnUpdate = ... ];`). Otherwise make the change in the text. |
| DAB8008 | warning | The parent class, skeleton, interfaces or default animation set changed in the editor, or a layer disappeared from it. These are only changed in the text. | Change them in `Settings` and rebuild. |
| DAB8009 | warning | Writing the changes produced text that does not check. Nothing was written. The message quotes the error. | Make the change in the text. |

DAB8007 is in the range but not raised by the current source.

## DAB9xxx: Control Rig

The checker for `ControlRig` documents. The lexer, parser, template and `#include` codes above apply to a `.dcr`
file as they do to a `.dab`; the checks that know the engine's Control Rig are these. Two codes of the `.dab`
checker are used the same way: DAB6008 (a name declared twice: a variable, a function, a section) and DAB6011 (a
statement or section that does not belong where it is written), and DAB3006 for a header `Root` that is not
`Game`, `Engine` or `Plugin.<Name>`. The language is described in [ControlRig.md](ControlRig.md).

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB9001 | error | A name finds nothing: a unit, dispatch, template or function; a variable, parameter or let; a type; an input or output pin the node does not have; an output a call does not give; an execute output a block after a call names that the call has no block for; a setting, a metadata key, a property of a hierarchy element; an asset; a value of an enum. Also an `Unsupported(...)` placeholder the decompiler left, which stands for something the text could not say. | Check the spelling. Units are written without their `RigUnit_` / `RigVMFunction_` prefix. A function of another rig needs that rig in `Uses`. Replace `Unsupported(...)` with what it stands for. |
| DAB9002 | error | A name finds more than one thing: a function of a library in `Uses` and a node of the engine, two functions of the libraries in `Uses`, two units or two dispatches of one short name, two assets of one name. Also two rigs in `Uses` that have the same asset name: the second is not used. (A function of the rig itself is found first and is never ambiguous.) | Write the library's name before the function (`Library.Name`), the node's full name (`RigUnit_...`), or `Path(Root, "Folder/Name")` for the asset. For two rigs of one name, give each an alias in `Uses`: `Other = Path(Root, "Folder/Name")`, and call `Other.Function(...)`. |
| DAB9003 | error, warning | Error: a node's wildcard types cannot be told from its arguments or from where its result goes (the pin it is linked to, the type of a typed `let`, the variable it is assigned to), or a member is read from a call whose type is still open. Nothing is guessed. Warning: a call of a template that every version accepts, with nothing that says which: the node is kept as a template with no type picked, and does nothing. | Give the result a type: `float Scale = GetModuleMetadata(...);`, or pass an argument whose type is known. For the warning, give an argument a type or write the node by its unit's name (`RigUnit_...`). |
| DAB9004 | error | A value does not fit: an argument of the wrong type, a literal that is not a value of the pin's type, a call used as a value that gives none, a call with a pin given twice or more arguments than it takes, an operator that no node takes the operands of, a constant expected where a value is computed, a function with neither code nor a value, an event without code, a `local` in an event, `Range(...)`, `Injected(...)` or `RunEvent(...)` used wrongly, blocks after a call without execute pins, two blocks for one execute output. | Write a value of the type the message names. Operators take two values of the same type (`float` and `double` mix); a vector times a number is the one other mix. |
| DAB9005 | error | A call that only computes a value, written as a statement: it has no execute pins, so it has no effect. | Use the value (`let X = ...;`, an argument, an assignment), or remove the statement. |
| DAB9006 | error | An assignment to something that is not assignable: a function's parameter, a `let`, a loop name, a read only variable; or to one member of a variable (`V.X = 1;`), or to anything but a variable or an element of an array variable. | Assign the whole variable: `V = (X = 1, Y = V.Y, Z = V.Z);`. RigVM's setter writes the whole value, so a one member assignment would reset the others. |
| DAB9007 | error | A `return` that is not the last statement of a function's code, at its top level: one that is followed by other statements, is inside an `if` or a loop, or is outside a function. | RigVM has one Return node per function: write the rest of the code as the other side of an `if`. |
| DAB9008 | error | An `Inline` function that is never called, or is called more than once. It is the body of one collapse node, made where it is called. | Call it once, or remove `[ Inline ]` to make it a function of the library. |
| DAB9009 | error | A hierarchy element declared twice, or given again by `Import` after it was written, or with a parent that is not allowed: a parent not declared before it, a parent of a kind that cannot be one (curves, connectors and sockets), a curve or connector given a parent, a `Parent` that is not written as an element key (`Bone(name)`, `Null(name)`, `Control(name)`). | Declare each element once, parents first (or nested). An imported element is refined by writing it after `Import` with only the properties that change. |
| DAB9010 | error | The same event twice in one graph, or a `Graph` that is not a non empty string. | Events are written once per graph: put the second one in another graph with `[ Graph = "..." ]`. |
| DAB9011 | error | A struct has no such member, or a tuple has more positional values than the struct has members. User structs go by their display names. | Use the struct's member names. Name the members when in doubt. |
| DAB9012 | error | An asset in `Uses` is not a Control Rig, or a function called from one is not public. | List Control Rig assets in `Uses`. Mark the function `[ Public ]` in its rig. |
| DAB9013 | error | `[ Pure ]` on a function written `= value;`, which is pure already. | Remove `[ Pure ]`. It marks a function written as code whose callers see no execute pins: `Function F(...) -> (...) [ Pure ] { ... }`. |

### Building and reading Control Rigs

The generator runs only when the file checks without errors. Its diagnostics point at the `ControlRig` word of
the header. Their messages start with a place in the rig (`FunctionLibrary/Name`, a graph and a node, `Hierarchy`)
and say what could not be done. DAB9104 is raised by the decompiler.

| Code | Severity | Meaning | How to fix |
|---|---|---|---|
| DAB9101 | error | The generator could not build something the model asks for (a node that would not add, a link that would not connect, a hierarchy element, a function), the asset could not be created or saved, or the compiler reported an error (the message starts with `compile:`). | Read the message. A compile error is the Control Rig compiler's own: fix the text or the rig it names. |
| DAB9102 | warning | The build went through, but something is not as the text says: the asset exists in the other kind of Control Rig and keeps its kind, the asset was created under another name, the generator ignored a property it could not apply, or the compiler reported warnings (`compile: n warnings`). A rig of the Blueprint-based kind gets the first of these every time it is built. | Nothing, if it is what you meant. For a kind, a name taken or a compile warning, read the message. |
| DAB9103 | warning | A function the text no longer has was removed from the library, and a loaded rig called it. The engine removed those calls. | Open the rig named in the message and replace the calls, or put the function back in the text. |
| DAB9104 | error | The decompiler cannot read the asset: it is not a Control Rig, or it has no editor data. | Open the asset in the editor and check that it is a Control Rig. |
