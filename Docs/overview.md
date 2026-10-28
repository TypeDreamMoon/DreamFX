---
title: Overview
description: DreamFXLang is a text-first language for Unreal Niagara effects, compiled into standard Niagara assets by the DreamFX plugin — and decompiled back out again.
---

DreamFXLang is a text-first authoring language for Unreal Engine Niagara. You write `.dfs`, `.dfe`
and `.dfm` files, and the **DreamFX** plugin compiles them into standard assets —
`UNiagaraSystem`, `UNiagaraEmitter`, `UNiagaraScript` — which a level, a blueprint or a sequencer
references exactly like any other.

The source is what you maintain. The `.uasset` is a build product: it can always be thrown away and
regenerated, and it is never edited by hand.

The reverse direction holds too: **any** existing Niagara system decompiles back into a `.dfs`.
That is not a convenience export, it is a contract — anything the language cannot express is written
into the file header as an explicit gap, never dropped in silence.

| | |
| --- | --- |
| Plugin version | `1.0.0` |
| Engine | Unreal Engine `5.8` (a source build and the installed engine share **one source tree with zero `#if` forks**) |
| Modules | `DreamFX` (Runtime), `DreamFXEditor` (Editor) |
| Source extensions | `.dfs` · `.dfe` · `.dfm` |
| Project settings | *Project Settings ▸ Plugins ▸ DreamFX* |
| Diagnostics | [`DFXnnnn` code catalog](diagnostics/README.md), with file, line and column |
| License | MIT |

## Three kinds of source file

| Extension | Declares | Produces |
| --- | --- | --- |
| `.dfs` | `System` | a `UNiagaraSystem` |
| `.dfe` | `Emitter` | nothing on its own — merged into a `.dfs` by `from` |
| `.dfm` | `Module` / `DynamicInput` | a `UNiagaraScript` (works on the installed engine through a reflection backend) |

All three share one header:

```cpp
<Kind>(Name="<path under the root>", Root="<root token>")
{
    ...
}
```

`Kind` has to agree with the file extension ([DFX2021](diagnostics/DFX2xxx.md#dfx2021)). `Root` is
`Game`, empty (the same thing), or `Plugin.<PluginName>`; `Name` is the asset path relative to that
root's content directory. See [the file model](language/README.md).

## A minimal system

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];   // exposed as User.Speed
    }

    Emitter Motes
    {
        Settings = { SimTarget = CPU; Determinism = true; RandomSeed = 1; }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode = DirectSet, Lifetime = 2.0,
                SpriteSizeMode = Uniform, UniformSpriteSize = 8.0
            );
            SystemLocation();
            AddVelocityInCone(ConeAngle = 30.0, VelocityStrength = User.Speed);
        }

        ParticleUpdate = {
            ParticleState();
            GravityForce(Gravity = (0, 0, -400));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core
        {
            Alignment = Unaligned;  FacingMode = FaceCamera;  SortMode = ViewDepth;
        }
    }
}
```

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

That writes `/Game/Effects/NS_Hello` with no editor involved. With the editor open, saving the source
rebuilds it — leave the generated asset open in the Niagara editor and it is the live preview for
this workflow.

> **Note:**
>
> Commands that write packages (`build`, `corpus`, `mirror-diff`, `decompile-all`) want the **editor
> closed**. Two processes saving the same packages race silently, and whichever saves second wins.
> See [the daily loop](getting-started/workflow.md).

## What the language covers

- **User parameters**, including data-interface parameters with their JSON configuration (`DI<X>`);
- **the six stacks**: `SystemSpawn` / `SystemUpdate` / `EmitterSpawn` / `EmitterUpdate` /
  `ParticleSpawn` / `ParticleUpdate`;
- **event handlers** `OnEvent(...)` and **named simulation stages** `Stage <Name>(...)`;
- **renderers**, with schema-driven properties and `Bind`;
- every value mode: literals, links, enums, nested dynamic inputs, `hlsl { }`, `curve { }` with
  tangent modes, and static switches in every position.

Not covered, by design or not yet: Scratch Pad, module-internal graph lowering, GPU/CPU branch
conditions and Scalability conditions. Native emitter inheritance is supported with
[`inherits`](language/native-emitter-inheritance.md); `from` copies and merges source instead.
See [limits](diagnostics/limitations.md) for the remaining boundaries.

## Round-trip, verified four ways

The correctness of a decompile is measured, not claimed:

| Layer | Question it answers |
| --- | --- |
| **L1** `mirror-diff` | does the mirror's export match the original's, line by line? |
| **L2** | does the mirror compile clean? |
| **L3** | does the rebuilt system *simulate* like the original? (fixed-step SimCache, per-frame counts) |
| **asset-diff** | do the two agree *as assets* — reflection-walked facts, independent of the exporter? |

The detail is in [round trip and equivalence](generation/roundtrip.md).

## Where to start

  - **[Getting started](getting-started/installation.md)** — Check the plugin is on, write a first `.dfs`, and learn the save-and-rebuild loop.
  - **[Language reference](language/README.md)** — The file model, the three file kinds, stacks and statements, values, and the eight `L*` rules.
  - **[Events and stages](language/events-and-stages.md)** — `OnEvent` handlers, named simulation stages, and the hard limits on each.
  - **[Generation and round trip](generation/pipeline.md)** — The build pipeline, provenance, decompilation, Export vs Adopt, engines and backends.
  - **[Tooling](tools/headless-compilation.md)** — `dfx.ps1`, `ci.ps1`, the editor integration, the VSCode workspace, the Python surface.
- **[Diagnostics](diagnostics/overview.md)** — The generated `DFXnnnn` catalog in eight families, each with its cause and its fix.
  - **[Limits](diagnostics/limitations.md)** — What is not covered, the 1.0.0 known issues, and the environment requirements.
  - **[Changelog](changelog/index.md)** — What each release covers.
