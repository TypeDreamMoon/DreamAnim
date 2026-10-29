# DreamAnim changelog

What each grammar version changed in the text: the syntax, what a file builds, what write-back writes, and
how node GUIDs are made. The current version is in [Grammar.md](Grammar.md), and `Tools/dab.ps1 vocab` reports
it as `grammarVersion`. A tool that reads or writes DreamAnim sources can compare it with the version it
was written for.

Changes that do not touch the text (the command line, the editor, messages) are not listed.

## 5 (2026-10-29)

- A struct written inside a tuple is that struct's whole value: the fields it leaves out are the struct's own
  defaults. `LookAt_Axis = (Axis = (X = 1))` is the axis (1, 0, 0), though Look At's default axis is +Y.
  Before, the fields left out kept the node's default, and a field written at the struct's default (`Y = 0`)
  was dropped, so the text could not say such a value. The outermost tuple of a value still changes only
  the fields it writes. See [Language.md](Language.md#tuples-and-structs).

## 4 (2026-10-21)

- `.dcr` code: a call can be followed by a block for each of its node's other execute outputs,
  `IsGameWorld() { True { ... } }`, and so can a let of a call. The decompiler writes every such node this
  way; before, what those outputs ran was left out. See
  [ControlRig.md](ControlRig.md#blocks-after-a-call).

## 3 (2026-10-12)

- `.dcr` documents: Control Rigs as text, checked, built, decompiled and written back. See
  [ControlRig.md](ControlRig.md).
- An item of a list may be named in every document: `[ Name = Item ]`. In a `.dcr` the `Uses` setting
  gives it a meaning, the name the calls into that rig go by.

## 2 (2026-10-11)

- A comparison of an integer with a constant is made in a type wide enough for both: a byte compared with
  256, or an int with 2^31, no longer cuts the constant down to the variable's type. The conversions this
  needs are written in the text (`ByteToInt`, `IntToInt64`, ...).
- In a shared rule, `StateWeight()`, `TimeRemaining()` and `TimeRemainingRatio()` without a state read the
  source state of each transition that uses the rule: such a rule gets a rule graph per transition. A rule
  that names its state stays shared.

## 1 (2026-10-07)

- The first version: `.dab` Animation Blueprints, `.dai` Animation Layer Interfaces and `.dat` template
  libraries; write-back; node GUIDs from `<asset path>/<graph path>/<node id>`.
