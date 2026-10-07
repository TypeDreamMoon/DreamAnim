# Animation sets

An animation set gives a family of DreamAnim Blueprints one character's animations. The Blueprints
declare slots. A set fills them. Swapping the set at run time gives the same Blueprints another
character's animations, without building anything.

- [Declaring slots](#declaring-slots)
- [What the build adds](#what-the-build-adds)
- [The Dream Anim Set asset](#the-dream-anim-set-asset)
- [DefaultAnimSet](#defaultanimset)
- [Run-time API](#run-time-api)
- [When a swap takes effect](#when-a-swap-takes-effect)
- [Threading rules](#threading-rules)
- [What a swap does to playback](#what-a-swap-does-to-playback)
- [Cooking and skeletons](#cooking-and-skeletons)

## Declaring slots

A Blueprint declares its slots in `Assets` and reads them as `Assets.<Name>`:

```dab
AnimBlueprint(Name="DreamAnimTests/AnimSetSwap", Root="Game")
{
    Settings = { Parent = AnimInstance; Skeleton = SK_Als; }
    Assets = { AnimSequence Pose; }
    Graph = { Output = SequenceEvaluator(Sequence = Assets.Pose, ExplicitFrame = 0); }
}
```

- Each slot is `Type Name;`. The type is an animation asset class: `AnimSequence`, `BlendSpace`,
  `AnimMontage`, ... (DAB3005). A slot takes no value and no metadata.
- `Assets.Name` can go wherever a value is read at run time: a node argument that can be bound, such as
  a player's `Sequence` or `BlendSpace`; an argument of a call (`OnEnter`, notifies, `Call(...)`); an
  expression.
- It cannot go where only a constant fits: settings, metadata, transition options, editor-node settings,
  tuning value defaults (DAB4010). Write `Path(...)` to a fixed asset there.
- An undeclared slot is DAB5003.

The full rules for the section are in [Language.md](Language.md#assets).

## What the build adds

When a Blueprint declares at least one slot, the build adds:

| What | Purpose |
|---|---|
| variable `DreamAnimSetActive` | The set the graph reads. Its default is the Blueprint's `DefaultAnimSet`. |
| variable `DreamAnimSetPending` | The set `SetAnimSet` wrote, waiting for the next update. |
| function `DreamAnimAsset_<Slot>`, one per slot | Reads the slot from the active set. Thread-safe. |
| an Update Animation event | Calls `ApplyPendingAnimSet` on the game thread before the graph runs. |

- The two variables are in the `DreamAnim` category. When the parent is a DreamAnim Blueprint that
  already has them, they are not added again.
- Every pin that reads `Assets.X` is bound to `DreamAnimAsset_X`. The getter looks the slot up under the
  Blueprint's asset name and returns it typed: an AnimMontage for a montage slot, a BlendSpace for a
  blend space slot, an AnimSequenceBase for a sequence slot, an AnimationAsset otherwise.
- A slot no pin reads still gets its getter. The getter is what records the slot in the Blueprint, so
  the decompiler finds it again.
- A Blueprint without slots gets none of this.

## The Dream Anim Set asset

Create one in the Content Browser: **Add > Animation > Dream Anim Set**. It is a `UDreamAnimSet`, a
primary data asset in the `DreamAnim` runtime module.

| Field | Meaning |
|---|---|
| `Skeleton` | The skeleton every animation of the set uses. The slot pickers show only assets compatible with it. |
| `Groups` | One group per Blueprint, keyed by the Blueprint's asset name (`AB_Als_Standing`). |
| `Groups > Slots` | One row per slot the Blueprint declares: its name, its type and the chosen animation. |

Each slot row shows the slot's name and an asset picker:

- the name and type come from the Blueprint and cannot be edited;
- the picker offers only assets of the slot's type whose skeleton fits the set's `Skeleton`;
- a slot the Blueprint no longer declares is an orphan. It is shown dimmed as "(no longer used)". Its
  animation is kept until you remove the row yourself. Declaring the slot again clears the mark.

A group is keyed by the Blueprint's asset name, not its path. Two Blueprints with the same asset name
share one group.

### How slots get into a set

A build updates the Blueprint's `DefaultAnimSet`:

- it adds the Blueprint's group when it is missing, and every slot the Blueprint declares;
- it updates a slot's type when the declaration changed;
- it marks slots the Blueprint no longer declares as orphans;
- it never removes an animation already chosen.

A build that saves the Blueprint saves the set with it when its slots changed; `-NoSave` leaves both in
memory.

To make a set from an existing character, decompile its Blueprints with `-AnimSet`: every animation they
read becomes a slot, and the set is created and filled with them. See
[Moving the animations into a set](Decompiling.md#moving-the-animations-into-a-set).

Other sets are not touched by a build. To make another character's set, duplicate a filled set and
change its `Skeleton` and animations. A slot added to a Blueprint later reaches only the default set. To
add it to another set, call `UDreamAnimSet::SyncSlots` on that set from a tool and save it.

In a set that lacks a slot, or leaves it empty, the slot reads as nothing: the node gets no animation.

## DefaultAnimSet

```dab
Settings = {
    Parent         = AlsLinkedAnimationInstance;
    Skeleton       = SK_Als;
    DefaultAnimSet = DAS_Hero;
}
```

- The value is the set's asset name or `Path(...)`. It must be a Dream Anim Set (DAB7004 at build time).
- It becomes the default of `DreamAnimSetActive`, so every new instance starts with it.
- It has an effect only when the Blueprint declares slots.
- Without it, every slot reads as nothing until a set is given with `SetAnimSet`.

Each Blueprint of a character can name its own default. Once the main instance has an active set,
linked instances follow it; see below.

## Run-time API

`UDreamAnimLibrary`, in `DreamAnimLibrary.h` of the `DreamAnim` module. All of it is callable from
Blueprints.

| Function | Thread | What it does |
|---|---|---|
| `SetAnimSet(AnimInstance, Set)` | game | Gives the instance the set. Given the mesh's main instance, also every linked instance on the mesh. They take it at their next update. |
| `GetAnimSet(AnimInstance)` | game | The set the instance plays from now: its active set. |
| `ApplyPendingAnimSet(AnimInstance)` | game | Moves the pending set to active. Generated code calls it at the start of each update; it is hidden in Blueprints and not meant to be called by hand. |
| `GetAnimSetAsset(Set, Blueprint, Slot, Fallback)` | any | The asset in a slot, or `Fallback` when the set is null or the slot is empty. |
| `GetAnimSetSequence(Set, Blueprint, Slot)` | any | The same as an AnimSequenceBase; null for an asset of another type. |
| `GetAnimSetBlendSpace(Set, Blueprint, Slot)` | any | The same as a BlendSpace. |
| `GetAnimSetMontage(Set, Blueprint, Slot)` | any | The same as an AnimMontage. |

`UDreamAnimSet`, in `DreamAnimSet.h`:

| Function | Thread | What it does |
|---|---|---|
| `Find(Blueprint, Slot)` | any | The asset in a slot, or null. |
| `SetAsset(Blueprint, Slot, Asset)` | game | Fills an existing slot from code. False when the group or slot does not exist. |
| `SyncSlots(Blueprint, Slots)` | game | Adds a Blueprint's group and slots and marks orphans, as a build does. True when anything changed. For tools. |

Swapping a character's set from C++:

```cpp
#include "DreamAnimLibrary.h"

void AHeroCharacter::UseAnimSet(UDreamAnimSet* Set)
{
    UDreamAnimLibrary::SetAnimSet(GetMesh()->GetAnimInstance(), Set);
}
```

The module that calls it lists `DreamAnim` among its dependencies.

## When a swap takes effect

1. `SetAnimSet` writes the set into the instance's `DreamAnimSetPending`. When the instance is the
   mesh's main instance, it writes it into every linked instance on the mesh as well.
2. At the start of its next update, on the game thread, each instance moves its pending set to
   `DreamAnimSetActive`.
3. The graph then runs, on the worker threads, and reads only the active set. One update never mixes
   two sets.

Consequences:

- Until that update, `GetAnimSet` returns the previous set.
- A linked instance with nothing pending follows the main instance's active set. A linked instance
  created after a swap, such as an overlay layer linked later, takes the main instance's set when it
  updates.
- That needs the main Blueprint to declare slots. Without them it has no active set to follow, and a
  linked instance created after the swap keeps its own default.
- Call `SetAnimSet` on the main instance. Given a linked instance alone, the set lasts one update: after
  that the linked instance follows the main instance's set again, when the main instance has one.
- Passing `None` cancels a swap that has not been applied yet. It does not clear the active set.

## Threading rules

- `SetAnimSet` and `GetAnimSet` run on the game thread. `SetAnimSet` checks it.
- The slot getters (`GetAnimSetAsset`, `GetAnimSetSequence`, ...) and `UDreamAnimSet::Find` are
  thread-safe. The generated `DreamAnimAsset_<Slot>` functions call them from the worker threads.
- The getters read only the active set, which changes only on the game thread before the worker threads
  start.
- Do not change a set's contents while instances that use it are animating: not with `SetAsset`, not in
  the editor. The set's lookup is rebuilt on the game thread while worker threads may be reading it. To
  change animations at run time, swap to another set.

## What a swap does to playback

The animation changes on the update that applies the set. DreamAnim does not blend between the old and
the new animation. What happens to playback is up to the node that plays it:

| Node | On a new animation |
|---|---|
| SequencePlayer | Keeps playing from the same time in seconds, clamped to the new animation's length. |
| BlendSpacePlayer, BlendSpaceEvaluator | Restarts from `StartPosition` by default. Set the node's `bResetPlayTimeWhenBlendSpaceChanges = false` to keep the time. |
| players in a sync group | Keep their phase when the old and the new animations carry sync markers of the same names. |

- `Auto` transitions and `TimeRemaining()` read the new animation from the next update. On the update
  that applies the swap they still see the previous one.
- The automated test `DreamAnim.Corpus.Als.AnimSetSwap` swaps a running instance from a standing pose set
  to a crouching one and back. The pose follows each swap, and swapping back gives the first pose again
  to within 0.01 cm.

## Cooking and skeletons

- A set holds hard references to its animations, so they cook with the set. This was checked by cooking
  only a Blueprint whose one animation is in its default set: the Blueprint's package references the set
  and the skeleton, not the animation, and the animation is in the cooked output.
- The default set is referenced by the Blueprint's class defaults and cooks with the Blueprint. Any other
  set needs a reference of its own (from a character class, a data asset, ...) or an entry in the
  project's cook settings, and must be loaded before it is passed to `SetAnimSet`.
- The run time does not check skeletons. An animation made for another skeleton plays with its bones
  matched by name. The editor's picker hides incompatible assets; check sets filled any other way.
