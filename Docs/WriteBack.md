# Working in the editor

The text is the source of an Animation Blueprint made with DreamAnim. While the editor runs it keeps the
two in step both ways. Saving a source rebuilds its Blueprint. Changing the Blueprint in the editor
writes the change into its source. When both changed at once, nothing is overwritten: the editor lists
the conflict and you choose.

## Sources and the assets they make

The editor finds sources under the project's `DAnim/` folder and the `DAnim/` folder of every enabled
plugin. The header of a source names the asset it makes: `AnimBlueprint(Name="Characters/AB_Hero")`
makes `/Game/Characters/AB_Hero`, and `Root="Plugin.ALS"` puts it under that plugin's content. A Blueprint
is "made from text" when some source under those folders names it.

**Tools > Dream Anim > Rescan DreamAnim Sources** finds the sources again. The editor also does this by
itself for files it sees appear.

## From text to Blueprint

When a `.dab` or `.dai` file is saved, its Blueprint is rebuilt. When a `.dat` library is saved, every
document that includes it is rebuilt, interfaces first and each document after the Blueprints it names.

- An asset editor open on the Blueprint is closed for the rebuild and opened again afterwards.
- The rebuilt Blueprint is saved, unless **Save Rebuilt Blueprints** is turned off.
- A source with errors does not touch its Blueprint. The errors go to the **Dream Anim** message log
  (**Tools > Dream Anim > DreamAnim Log**). Each entry opens the source at its line.

A Blueprint keeps, in its package, the text it was last built from or written to. When you open a
Blueprint whose source changed since then (it was edited while the editor was closed, or the rebuild
failed), a notification offers to rebuild it.

**Rebuild from DreamAnim Source**, on the Blueprint in the Content Browser, and **Tools > Dream Anim >
Rebuild All from DreamAnim Sources** rebuild on demand.

## From Blueprint to text

A change you make to a Blueprint made from text is written into its source once your edits pause. That
means **Write-Back Delay** seconds with no further change, no transaction open and no mouse button held.
**Write Changes to DreamAnim Source** on the Blueprint writes them at once. An undo is a change like any
other and is written back too. Saving the Blueprint also writes them first, so a change made outside a
transaction (by a script or another tool) reaches the text no later than the asset reaches disk.

The editor reads the Blueprint again and compares it, value by value, with what its source says. A node
is matched to the text it came from by its GUID, which the generator derived from the node's place in
the text. What is written, and how:

| Change in the editor | What changes in the text |
|---|---|
| A value on a node (`PlayRate`, a blend time, an enum) | Just that value. |
| An option the text did not mention, or one set back to its default | The node's call, reprinted with the arguments it now needs. Arguments that did not change keep their text. |
| A transition's blend, priority or other option; its condition | Its row in `Transitions`. The table is aligned again. |
| A state's setting (`bAlwaysResetOnEntry`) or its `OnEnter` / `OnExit` / `OnBlended` | The state's header or functions. |
| A tuning value's default, a class setting, a class default, an asset slot, a notify | That entry in `Properties`, `Settings`, `Defaults`, `Assets` or `Notifies`. Only the value is replaced when nothing else changed. A new entry goes at the end, and a removed one is deleted. |
| Nodes or links added or removed, a pose wired differently | The graph block the change is in (`Graph`, a layer, a state's body), rewritten from the Blueprint. Statements keep their comments by name. |
| States, conduits or transitions added or removed | The state machine, rewritten from the Blueprint. States that did not change shape keep their text. |

Everything else stays as you wrote it: comments, blank lines, aligned tables, and blocks you broke over
lines or kept on one. The file keeps its line ends and its byte order mark, if it had one.

After writing, the new text is checked. It must describe exactly what the Blueprint holds. If writing
broke the text, nothing is written (DAB8009). If something is left that could not be written, it is listed
(DAB8005).

### Templates

A node that comes from a template call is written through the call. A changed value that was passed to
the template as an argument is written at the call, in place of that argument.

A value written inside the template's body has no place at the call: changing it there would change
every use of the template. The change is refused (DAB8001), and the notification offers **Write the
Template Out**. That rewrites the graph block the node is in from the Blueprint, with the block's
template calls written out as what they expand to (DAB8004). After that the change can be kept.

### What is never written back

- **Only changed in the text** (DAB8008): the parent class, skeleton, implemented interfaces and default
  animation set. A layer removed in the editor is also only changed in the text.
- **Not expressible** (DAB8006): what the language cannot hold, such as logic in event graphs or
  functions. Move that logic into the C++ anim instance.

A refused change stays in the Blueprint, and the text keeps what it said. The next rebuild from the text
drops the change. The notification and the log say what was refused. Refusals are reported once, not
again every time a node is moved.

With **Write Back Structure** turned off, added and removed nodes, links, states and transitions are
refused (DAB8003) instead of rewriting the block.

## When both changed

If the source changes on disk while the Blueprint has changes not yet in it, neither side is
overwritten. This happens when the text is edited before an edit in the editor was written back, or
after a change was refused. The editor lists the Blueprint's changes in the log, and a notification
waits for your choice:

- **Use the Text** rebuilds the Blueprint from the file. Its own changes are dropped.
- **Merge Both** writes the Blueprint's changes into the text it was last in step with. It then merges
  that, line by line, with the file as it is now, writes the result and rebuilds the Blueprint from it.
  Changes on the same or neighbouring lines are not merged. Then nothing is written, the log shows both
  versions of each place, and you can edit the text by hand or use it as it is. Nothing is merged
  either when part of the Blueprint's changes cannot be written (a refusal such as DAB8006): merging
  the rest would drop it. The conflict stays until you choose Use the Text or make the change in the
  text yourself.

A refused change counts as a change of the Blueprint's own: a text saved on disk after a refusal is a
conflict, not a rebuild that would drop it. So is a text the Blueprint can no longer be compared with,
for instance after a library it includes changed the meaning of a template. One case is not caught: a
change to a library the Blueprint includes, when nothing in this editor session marked the Blueprint as
edited (after a restart, say), rebuilds it and drops what the text never held. A changed library makes
the old text a different Blueprint, so it cannot tell such changes apart.

Until you choose, the Blueprint's changes are not written back.

## Node positions

Next to a source, `<source>.layout` (for example `AB_Hero.dab.layout`) lists where the editor put each
node of the Blueprint. Each line is a node GUID, its position, and a comment with the graph and node
name:

```text
# Where the editor put the nodes of AB_Hero, by node GUID. Written by the editor; safe to delete.
6A0F1C2B9E7D4C3A8B5E2F1D0C9B8A7E 320 -48  # Idle: Sequence Player
```

The editor writes it whenever it writes a Blueprint back, so it records nodes you moved. Every build,
including `dab.ps1 build`, puts the nodes the file knows back where it says. A node it does not know
gets the automatic layout. Generated GUIDs come from the node's place in the text, so they hold across
rebuilds. A renamed node starts again from the automatic layout. Delete the file to go back to the
automatic layout. Commit it with the source if node positions matter to your team.

## Writing an existing Blueprint as text

**Write as DreamAnim Text**, on an Animation Blueprint not made from text, writes it under the project's
`DAnim/` folder. The path follows the asset's: `/Game/Characters/AB_Hero` becomes
`DAnim/Characters/AB_Hero.dab`. An existing file is replaced only if you say so. You are then asked
whether to rebuild the Blueprint from the new text. From then on the text is its source, and changes made
in the editor are written back. Write-back matches nodes by the GUIDs the generator gives them, so a
Blueprint must be rebuilt from its text once before its changes can be written back.

A Blueprint with something the text cannot hold is not rebuilt. The file lists those things at the top.
[Decompiling.md](Decompiling.md) covers moving a whole character over.

**Write as DreamAnim Text with an Animation Set...** does the same, and moves the animations the Blueprints
read into slots of an animation set you name or pick in a save dialog. Picking an existing set adds the
Blueprints' groups to it; the dialog's warning about replacing it does not apply, nothing in it is
replaced. See [Moving the animations into a set](Decompiling.md#moving-the-animations-into-a-set).

## Control Rigs

A Control Rig made from a `.dcr` source is kept in step the same way: saving the text rebuilds the rig,
and a change made to the rig in the editor is written into the text once your edits pause, and before the
rig is saved. **Write Changes to DreamAnim Source**, on the rig in the Content Browser, writes them at once.
The settings above apply to rigs too.

The editor reads the rig again and compares it with the model its text builds to. Nodes are matched by the
names the text gives them (a `let`'s name, or the name the checker made), so a rig must have been built
from its text once before its changes can be written back. What is written, and how:

| Change in the editor | What changes in the text |
|---|---|
| A value the text writes: a pin's argument (`Weight = 0.5`), a variable's default, a control's setting, shape transform, offset or initial value, an element's transform | Just that value, written the way the decompiler writes it. |
| A value the text leaves at its default, nodes or links added or removed, an element added, removed or given another parent, a variable added or removed | The block it is in, rewritten from the rig: the `Event`, the `Function`, the `Detached` block, or the `Hierarchy`, `Variables`, `Settings` or `Module` section. |
| An event or function added, or one removed | Added at the end of the document, or removed. |

In a rewritten block, the comments are the rig's comment boxes, as decompiling writes them; comments above
the block and everything outside it stay as you wrote them. Moving or editing a comment box, and moving
nodes, are not written back. With **Write Back Structure** turned off, what would rewrite a block is refused
(DAB8003).

After writing, the new text is checked: it must build to exactly what the rig holds. If it does not, nothing
is written (DAB8009).

When the source changed on disk while the rig has changes not yet in it, neither is overwritten and the log
says so. **Rebuild from DreamAnim Source** takes the text. To keep the rig's changes, undo the text's change
and use **Write Changes to DreamAnim Source**. Rigs have no line merge.

**Write as DreamAnim Text**, on Control Rigs not made from text, writes each one under the project's `DAnim/`
folder ([Decompiling.md](Decompiling.md)) and then offers to rebuild them from their new text, which
write-back needs.

From code: `WriteBackDcrRig` (`Rig/DcrWriteBack.h`, in the DreamAnimRig module) compares a rig with its text
and returns the new text, the changes and the refusals; it reads and writes no files.
`GetDcrGeneratedText` reads the text a rig was last built from or written to.

## Settings

**Editor Preferences > Plugins > Dream Anim**:

| Setting | Default | |
|---|---|---|
| Rebuild When Text Changes | on | Rebuild a Blueprint when its source, or a library it includes, is saved. |
| Save Rebuilt Blueprints | on | Save a Blueprint after rebuilding it, so the asset on disk matches the text. |
| Write Back Editor Changes | on | Write changes made in the editor into the source. Off, edits stay in the Blueprint and the next rebuild drops them; a text changed meanwhile is still a conflict. |
| Write Back Structure | on | Write added and removed nodes, links, states and transitions by rewriting their block. Off, they are refused (DAB8003). |
| Write-Back Delay | 1 s | Seconds without further edits before changes are written. |
| Open Source Command | `code -g "{file}:{line}:{column}"` | How a source is opened at a line, run through the shell. Empty opens the file with its associated program. |

## From code

`UDreamAnimEditorSubsystem` (`GEditor->GetEditorSubsystem<UDreamAnimEditorSubsystem>()`) does all of the
above: `Rebuild`, `RebuildAll`, `WriteBack`, `HasEditorChanges`, `UseText`, `MergeEditorChanges`,
`Decompile` and `OpenSource`.

Without the editor around it:

- `WriteBackDabBlueprint` (`WriteBack/DabWriteBack.h`) compares a Blueprint with its text and returns the
  new text, the changes and the refusals. It reads and writes no files.
- `MergeDabTexts` (`WriteBack/DabTextMerge.h`) is the line merge.
- `GetDabGeneratedText` and `MakeDabLayout` (`Generate/DabGenerate.h`) read the text a Blueprint was built
  from and its node positions.

The codes are listed in [Diagnostics.md](Diagnostics.md#write-back).
