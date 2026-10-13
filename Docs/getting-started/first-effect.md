---
title: Your first effect
description: From an empty directory to a running NS_Hello — one .dfs, one command, one standard UNiagaraSystem.
---

Fifteen minutes, no editor required.


## Write the source

Create `Effects/NS_Hello.dfs` under the project's `DFX/`:

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-100, -100, -100, 100, 100, 100);
    }

    // Exposed to blueprint as User.Speed, and settable with SetNiagaraVariableFloat.
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];
    }

    Emitter Motes
    {
        Settings = {
            SimTarget          = CPU;
            Determinism        = true;
            RandomSeed         = 1;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode      = DirectSet,
                Lifetime          = 2.0,
                SpriteSizeMode    = Uniform,
                UniformSpriteSize = 8.0
            );
            SystemLocation();
            AddVelocityInCone(ConeAngle = 30.0, VelocityStrength = User.Speed);
        }

        ParticleUpdate = {
            // Advances NormalizedAge. Anything age-driven needs it, and Niagara reports an
            // unmet dependency if it is missing.
            ParticleState();
            GravityForce(Gravity = (0, 0, -400));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core
        {
            Alignment  = Unaligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;
        }
    }
}
```

The header's `Root="Game"` says which content root `Name="..."` is relative to, so this file builds
`/Game/Effects/NS_Hello`.


## Build it

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

You should see `1 built, 0 up to date, 0 failed | 0 error(s)`, and the asset listed under "Assets
written to disk by this run". Open `/Game/Effects/NS_Hello` in the editor and it plays.

A second build with no edits reports `0 built, 1 up to date` — the provenance stamp on the asset
holds a hash of the source, and a matching hash means there is nothing to do. `-Force` overrides it.


## Change one number and build again

Change `SpawnRate` from `20.0` to `60.0` and build again. This time it rebuilds — the hash moved.

That is the whole loop: **the text is the only authoring surface, and the asset is build output.**
Do not edit a generated asset in the Niagara editor; the next rebuild wipes the edit, and does not
ask first.


## When it does not compile

Every diagnostic is a `DFXnnnn` with a file, line and column:

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

Two things to reach for:

- **[Diagnostics](../diagnostics/overview.md)** — every code, with what causes it and how to fix it;
- **`dfx.ps1 schema <Module>`** — a module's *real* input signature, read from the asset. This is the
  authority on names: Niagara input names contain spaces (`Loop Duration`), which DreamFX
  normalises, and some inputs only exist once a static switch above them is set.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce
```

> **Note:**
>
> Leaving `ParticleState()` out is not a syntax error — it surfaces as Niagara's own unmet
> dependency. It advances `NormalizedAge`, which every age-driven module reads.

## Things to try next

- Swap `VelocityStrength` for a random range, to see how a nested dynamic input is written:

  ```cpp
  AddVelocityInCone(
      ConeAngle        = 30.0,
      VelocityStrength = RandomRangeFloat(
          Minimum        = User.Speed * 0.5,
          Maximum        = User.Speed,
          RandomnessMode = SimulationDefaults
      )
  );
  ```

  `RandomnessMode` is a static switch. On a **module** a switch has to be written before the inputs
  it gates; on a **dynamic input** that rule is relaxed — switches are hoisted — so the order above
  works either way round. See [values and rules](../language/values.md).

- Add a colour fade driven by a curve:

  ```cpp
  ParticleUpdate = {
      ParticleState();
      ScaleColor(
          ScaleAlpha = FloatFromCurve(
              FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
              CurveIndex = Particles.NormalizedAge
          )
      );
  }
  ```

  - **[Layout and naming](project-layout.md)** — `DFX/` roots, how `Root` and `Name` resolve, module search paths.
  - **[The `.dfs` reference](../language/dfs.md)** — Settings, Properties, the six stacks, renderers, `from`, `Defaults`.
