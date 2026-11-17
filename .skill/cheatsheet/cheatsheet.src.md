# DreamFXLang cheat sheet

One page of the language: the three file kinds, the shape of a system, how values are written, and the
mistakes that cost the most time. The complete system on this page is built by the plugin's gate. For the
rules behind a line, read [the language reference](language/README.md); for a code,
[the diagnostics](diagnostics/README.md) or the same data as JSON,
[`diagnostics/codes.json`](diagnostics/codes.json).

DreamFXLang is not HLSL, C++ or any other Niagara tool's syntax. **Module input names come from the real
assets**: run `dfx.ps1 schema <Module>` (or read the module in the editor) before you call one.

## Files

| File | Declares | Builds |
| :-- | :-- | :-- |
| `.dfs` | `System(Name="…", Root="…") { … }` | a `UNiagaraSystem` |
| `.dfe` | `Emitter(Name="…", Root="…") { … }` | nothing on its own — copied into a system with `Emitter X from "…"` |
| `.dfm` | `Module(…) { … }` or `DynamicInput(…) { … }` | a `UNiagaraScript` |

Sources live under `DFX/`. `Root="Game"` (or empty) builds under `/Game`, `Root="Plugin.MyPlugin"` under
that plugin; `Name="Effects/NS_Spark"` is the path below it.

## A system

```cpp
// DFX/CheatSheet/NS_CheatSheetSparks.dfs -- a burst of sparks that fly out, fall and fade, once a second
System(Name="CheatSheet/NS_CheatSheetSparks", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-200, -200, -50, 200, 200, 300);
    }

    Properties = {                                   // user parameters: User.SparkCount, …
        Group("Burst") {
            int   SparkCount = 24;                   // int: it drives an int32 input
            float SparkSpeed = 450.0;
        }
        Color Tint = (1.0, 0.72, 0.25, 1.0) [ Group="Look" ];
    }

    Emitter Sparks
    {
        Settings = {
            SimTarget          = CPU;
            Determinism        = true;
            RandomSeed         = 7;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(
                LifeCycleMode = Self,                // a static switch: written before what it gates
                LoopBehavior  = Infinite,
                LoopDuration  = 1.0
            );
            SpawnBurst_Instantaneous(SpawnCount = User.SparkCount, SpawnTime = 0.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(  // a partial path picks one of two modules
                LifetimeMode         = Random,
                LifetimeMin          = 0.3,
                LifetimeMax          = 0.6,
                SpriteSizeMode       = RandomUniform,
                UniformSpriteSizeMin = 3.0,
                UniformSpriteSizeMax = 8.0
            );
            SystemLocation();
            AddVelocityInCone(
                ConeAngle        = 40.0,
                VelocityStrength = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed)
            );
        }

        ParticleUpdate = {
            ParticleState();                         // advances NormalizedAge; age-driven inputs need it
            GravityForce(Gravity = (0, 0, -680));
            Drag(Drag = 1.4);
            SolveForcesAndVelocity();
            // An hlsl value carries no type, so annotate it. Its body is pasted verbatim: no comments inside.
            Color Particles.Color = hlsl {
                float4(User.Tint.rgb, saturate(1.0 - Particles.NormalizedAge))
            };
        }

        SpriteRenderer Core
        {
            Alignment  = VelocityAligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;
        }
    }
}
```

- Stacks: `SystemSpawn` and `SystemUpdate` at the top; `EmitterSpawn`, `EmitterUpdate`, `ParticleSpawn`,
  `ParticleUpdate` in an emitter, plus `OnEvent(…) = { }` and `Stage Name(…) = { }`. Writing order is
  module order. An undeclared stack is left alone; `SystemUpdate = { }` empties it.
- `Settings` and renderer properties are read off the live asset: a misspelling reports the real list.
- Declaration order of renderers is draw order.

## Statements in a stack

```cpp
ModuleName(Input = Value, Input = Value);          // a module call: arguments are always named
Spawn/Initialization/V2/InitializeParticle(…);      // a partial path when the short name is ambiguous
disabled GravityForce(Gravity = (0, 0, -980));      // kept in the stack, not executed
Particles.Moon.Seed = 0.5;                          // an assignment; the first write declares the attribute
Color Particles.Tint = User.Tint;                   // ...consecutive assignments fold into one Set Parameters
```

Input names drop Niagara's spaces: `Loop Duration` is `LoopDuration`. A name that is not an identifier
goes in back-quotes, as one token: `` `User.PillarPower(0~1)` ``. Enum values are written by name
without spaces: `DirectSet`.

## Values

<!-- block: Docs/language/values.md #l3--the-four-value-modes -->

<!-- block: Docs/language/values.md #literals -->

- Linked: any namespaced parameter — `User.`, `Particles.`, `Emitter.`, `System.`, `Engine.` — bound
  directly, so the types must match exactly (a `float` cannot drive an `int32`; declare it `int`).
- Inline expressions: `+ - * /`, parentheses and `normalize saturate clamp lerp frac min max abs floor
  ceil pow sqrt dot cross length int`, positional. Anything else goes in `hlsl { }`, one expression.
- `int` → `float` is implicit; `float` → `int` needs `int(…)`.
- Types: `float` `int` `bool`, `Vector2` `Vector` `Vector4`, `Color`, `Position`, `Quat`, `DI<X>`
  (a bare data interface name means `DI<Name>`: `Texture` is `DI<Texture>`; there is no `Texture2D`).

A curve, where a curve data interface is expected:

<!-- block: Docs/language/values.md #dynamic-input -->

## Properties and renderers

<!-- block: Docs/language/dfs.md #properties--user-parameters -->

<!-- block: Docs/language/dfs.md #renderers -->

`Bind` sets an attribute binding; `Meshes = ["/Engine/BasicShapes/Cube"];` lists a mesh renderer's
meshes (and `bOverrideMaterials = true;` before `OverrideMaterials`).

## Emitters from a `.dfe`

<!-- block: Docs/language/dfs.md #referencing-a-dfe -->

`from` copies the `.dfe` once, then the block overrides it; it is not inheritance.

## Modules and dynamic inputs — `.dfm`

<!-- block: Docs/language/dfm.md #dfm--a-module-or-a-dynamic-input 1 -->

<!-- block: Docs/language/dfm.md #dfm--a-module-or-a-dynamic-input 2 -->

Called from a stack like any module: `ToonSpin(SpinRate = 220.0);`, and a dynamic input wherever a value
goes: `UniformScaleFactor = ToonPulse(Frequency = 4.0)`.

## Mistakes that cost the most time

<!-- codes: DFX3001 DFX3003 DFX2008 DFX4027 DFX4003 DFX4022 DFX4030 DFX4031 DFX4025 DFX3004 DFX5033 DFX6004 -->

- A guessed module input name is `DFX3003`, which lists the real ones. Read the schema first.
- A static switch gates the inputs after it: write `LoopBehavior` before `LoopDuration`.
- An `hlsl { }` body is pasted into the shader as it stands: a `//` comment inside it can comment out the rest
  of the expression, and the Niagara compile fails with `DFX6001`. Put comments above it.
- Leave out `ParticleState()` and every age-driven input silently stays at age zero.
- `lint` checks a file without touching assets; `build -Force` proves it; the exit code is the error count.
