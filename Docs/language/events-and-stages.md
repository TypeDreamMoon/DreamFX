---
title: Events and simulation stages
description: OnEvent handlers and named Stage blocks — how they are written, the hard limit on each, and why a DataInterface name is the whole contract.
---

Beyond the six fixed stacks, an emitter may carry one **event handler** and any number of
**simulation stages**. Both are ordinary stacks: module calls, folded assignments, `disabled`, all of
it.

## `OnEvent` — reacting to another emitter's events

```cpp
Emitter Receiver
{
    ParticleSpawn = { … }

    OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = {
        ReceiveLocationEvent();        // ordinary stack statements; runs per received event
    }
}
```

The header configures the handler; the block is a stack like any other.

| Argument | Meaning |
| --- | --- |
| `Source` | the emitter in this system that generates the event. **Required** |
| `Event` | the event it generates (a `GenerateLocationEvent()` module sends `"LocationEvent"`). **Required** |
| `Mode` | `SpawnedParticles` spawns `SpawnNumber` new particles per event and runs the block on them; `EveryParticle` runs it on the existing ones |
| `SpawnNumber` / `RandomSpawnNumber` / `MinSpawnNumber` | how many to spawn |
| `MaxEventsPerFrame` | the per-frame ceiling on events handled |
| `UpdateAttributeInitialValues` | whether initial values are updated |

Write only the ones that differ from the defaults. A missing `Source` or `Event`, or an unknown
argument, is [DFX2025](../diagnostics/DFX2xxx.md#dfx2025).

Exports can also use backquoted source tokens: `` `<none>` `` for a handler reading from its own
emitter, or `` `<unresolved GUID>` `` for a stale source identifier. Both rebuild verbatim; an
unresolved source reports `DFX5036` because the handler never fires. See the
[system reference](dfs.md) for the full event-source contract.

> **Warning:**
>
> **One `OnEvent` block per emitter** ([DFX5031](../diagnostics/DFX5xxx.md#dfx5031)). DreamFX
> addresses the event stack through the same rails as the other four, and those can reach exactly one
> handler. An emitter that should react to two event streams becomes two emitters.

An event *generator* usually needs `RequiresPersistentIDs = true` in the source emitter's `Settings`,
because it reads `Particles.ID`; without it the build reports the engine's own stack issue
([DFX6003](../diagnostics/DFX6xxx.md#dfx6003)).

## `Stage` — simulation stages

```cpp
Emitter Fluid
{
    Settings = { SimTarget = GPU; }

    ParticleUpdate = { … }

    Stage Settle = {                     // a bare stage: enabled, particle iteration, runs once
        ScaleColor();
    }

    Stage Project(DataInterface = "Emitter.PressureGrid", NumIterations = 10) = {
        …                                // ordinary stack statements; runs per grid cell, ten times
    }
}
```

A stage is a particle stack that runs **after** `ParticleUpdate`, on a GPU emitter
(`SimTarget = GPU`; a CPU emitter with a `Stage` block is
[DFX5033](../diagnostics/DFX5xxx.md#dfx5033)). **Declaration order is run order.**

| Header argument | Meaning |
| --- | --- |
| `DataInterface` | the grid the stage iterates over (one thread per cell rather than per particle); it already implies the iteration source |
| `Iteration` | the iteration source stated explicitly — `Particles`, `DataInterface`, `DirectSet` — for the shapes that need it, such as a data-interface iteration with nothing bound |
| `NumIterations` | repeats the stage (default 1) |
| `Enabled` | `false` parks it |
| `ExecuteBehavior` | when it runs (e.g. `Always`, `OnSimulationReset`) |

These are the common header fields. Generic stages additionally support direct-dispatch sizing,
thread-count overrides and particle-iteration state filters; the complete argument table is in
the [system reference](dfs.md).

Write only what differs from a freshly added stage. An unknown or misshapen argument is
[DFX2026](../diagnostics/DFX2xxx.md#dfx2026); two `Stage` blocks with one name is
[DFX5032](../diagnostics/DFX5xxx.md#dfx5032) — stages are identified by name, and the build would
otherwise quietly keep whichever stack ran last.

### `DataInterface` is a name, and the name is the whole contract

The engine resolves the iteration grid **by name** at compile time and **never reads the binding's
stored type**. That is not a guess: authored assets ship with type handles that resolve to garbage in
later sessions, and simulate fine.

The grid itself is usually materialized by a module's internal writes — it does not need to be a
declared parameter for the binding to work, and a misspelt name surfaces as the stage compiling
against a grid that does not exist.

### `NumIterations` and `Enabled` take a value **or** a parameter

The value position decides which: a number, `true` or `false` is the literal, and anything that reads
as a name is a binding.

```cpp
Stage Project(NumIterations = 6, NumIterations = Emitter.OVERRIDE.SolveIterations) = { … }
Stage DebugSlice(Enabled = Grid3D_GAS_CONTROLS_SPAWN.RenderDebugSlice) = { … }
```

Both may appear at once, as above, because the engine stores both: the number is the fallback the
binding overrides.

> **Warning:**
>
> That is worth writing rather than simplifying away. A stage whose `Enabled` is bound runs when the
> effect says so, and a rebuild that kept only the literal flag ran it **always** — which is how
> Ninja's debug slice came to draw over the fluid in every mirror.

## Gaps

**[DFX8016](../diagnostics/DFX8xxx.md#dfx8016):** A stage of a custom C++ stage class — anything that is not the engine's generic stage — has no
DreamFXLang form, and is written into the exported file's header as a gap.

**[DFX8015](../diagnostics/DFX8xxx.md#dfx8015):** More than one event handler on an emitter cannot be carried either; the export names what the rebuilt
emitter will no longer receive.

Both belong to the "declared, never dropped" contract — see
[round trip and equivalence](../generation/roundtrip.md).

## Both families are measured

Events and stages have both been round-tripped over the whole library: all 27 real stages in this
project go through the corpus, `mirror-diff` and the stage fact family of `asset-diff`, with
`DFX8016` at zero. Stacks with a zero usage id need special handling on both the read and the write
side — that part is implementation — but the conclusion for a user is direct: **these two families
are not "probably fine", they are measured.**
