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

```cpp
InitializeParticle(
    Lifetime       = 0.25,                                     // literal
    Color          = User.TintA,                               // linked
    SpriteSize     = RandomRangeFloat(Minimum = 1, Maximum = 4), // dynamic input
);

Color Particles.Color = hlsl {                                 // HLSL expression
    float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge))
};
```

```cpp
450.0        24           true
(1, 0, 0)    (1.0, 0.72, 0.25, 1.0)      // 2, 3 or 4 components
"/Game/FX/M_Spark"                        // asset path
box(-200, -200, -50, 200, 200, 300)       // six numbers: min xyz, max xyz
```

- Linked: any namespaced parameter — `User.`, `Particles.`, `Emitter.`, `System.`, `Engine.` — bound
  directly, so the types must match exactly (a `float` cannot drive an `int32`; declare it `int`).
- Inline expressions: `+ - * /`, parentheses and `normalize saturate clamp lerp frac min max abs floor
  ceil pow sqrt dot cross length int`, positional. Anything else goes in `hlsl { }`, one expression.
- `int` → `float` is implicit; `float` → `int` needs `int(…)`.
- Types: `float` `int` `bool`, `Vector2` `Vector` `Vector4`, `Color`, `Position`, `Quat`, `DI<X>`
  (a bare data interface name means `DI<Name>`: `Texture` is `DI<Texture>`; there is no `Texture2D`).

A curve, where a curve data interface is expected:

```cpp
UniformScaleFactor = FloatFromCurve(
    FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
    CurveIndex = Particles.NormalizedAge
)
```

## Properties and renderers

```cpp
Properties = {
    int              SparkCount = 24                     [ Group="Burst"; SortPriority=10 ];
    float            SparkSpeed = 450.0;
    Color            TintA      = (1.0, 0.72, 0.25, 1.0);
    Vector           HitNormal  = (0, 0, 1)              [ Description="Impact normal from blueprint" ];
    Texture          NoiseTex   = "{\"Texture\":\"/MoonToon/Textures/T_Noise01.T_Noise01\"}";
    DI<SkeletalMesh> TargetMesh;
}
```

```cpp
SpriteRenderer Core
{
    Material     = "Plugin.MoonToon:Materials/FX/M_SparkSprite";
    Alignment    = VelocityAligned;
    FacingMode   = FaceCamera;
    SortMode     = ViewDepth;
    SubImageSize = (2, 2);

    Bind SpriteSize -> Particles.SpriteSize;
    Bind Color      -> Particles.Color;
}
```

`Bind` sets an attribute binding; `Meshes = ["/Engine/BasicShapes/Cube"];` lists a mesh renderer's
meshes (and `bOverrideMaterials = true;` before `OverrideMaterials`).

## Emitters from a `.dfe`

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

`from` copies the `.dfe` once, then the block overrides it; it is not inheritance.

## Modules and dynamic inputs — `.dfm`

```cpp
// A module: goes in a stack, reads and writes attributes.
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
{
    Settings = {
        Usage       = ParticleUpdate;
        Category    = "MoonToon|Motion";
        Description = "Spins a sprite at a constant rate, optionally reversed.";
    }

    Inputs = {
        float SpinRate   = 90.0  [ Description="Degrees per second." ];
        bool  bClockwise = true  [ StaticSwitch ];
        float RateScale  = 1.0   [ Advanced ];
    }

    Body = {
        float Dir = bClockwise ? 1.0 : -1.0;
        Particles.SpriteRotation += SpinRate * RateScale * Dir * Engine.DeltaTime;
    }
}
```

```cpp
// A dynamic input: computes a value for an input slot.
DynamicInput(Name="Modules/Moon/ToonPulse", Root="Plugin.DreamFX")
{
    Settings = { Usage = DynamicInput; Output = float; Category = "MoonToon|Math"; }
    Inputs   = { float Frequency = 6.0; float Sharpness = 2.0; }
    Body     = {
        return pow(0.5 + 0.5 * sin(Engine.Time * Frequency * 6.2831853), Sharpness);
    }
}
```

Called from a stack like any module: `ToonSpin(SpinRate = 220.0);`, and a dynamic input wherever a value
goes: `UniformScaleFactor = ToonPulse(Frequency = 4.0)`.

## Mistakes that cost the most time

| Code | Severity | Message |
| :-- | :-- | :-- |
| [DFX3001](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3001) | error | The module name resolved to nothing on the search paths. |
| [DFX3003](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3003) | error | Module '%s' has no input named '%s'.%s Available inputs: %s |
| [DFX2008](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx2xxx/#dfx2008) | error | Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'. |
| [DFX4027](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4027) | error | '%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Declare '%s' as %s, or drive the input another way. |
| [DFX4003](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4003) | error | Input '%s' is an integer, but %s was written. Narrowing is not implicit -- write int(%s) if truncation is intended. |
| [DFX4022](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4022) | error | Cannot infer the type of '%s' from its value. A first assignment to a new attribute must use a literal, so its type is unambiguous. |
| [DFX4030](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4030) | error | The hlsl block for '%s' must be a single expression: no statements, no local variables, no return. Move multi-statement logic into a .dfm DynamicInput and call it here. |
| [DFX4031](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4031) | error | '%s' is not an allowed inline function. Allowed: %s. Anything else belongs in a .dfm dynamic input or an hlsl { } block. |
| [DFX4025](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4025) | error | '%s' is not a valid assignment target. Parameter names are namespace-qualified, e.g. Particles.MyValue or Emitter.MyCounter. |
| [DFX3004](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3004) | error | Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, LightRenderer, DecalRenderer, ComponentRenderer, VolumeRenderer, or any UNiagaraRendererProperties subclass. |
| [DFX5033](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx5xxx/#dfx5033) | error | Emitter '%s' declares Stage blocks but simulates on the CPU. Simulation stages are a GPU feature: set `SimTarget = GPU` in the emitter's Settings. |
| [DFX6004](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx6xxx/#dfx6004) | warning | A Niagara stack issue at warning level. |

- A guessed module input name is `DFX3003`, which lists the real ones. Read the schema first.
- A static switch gates the inputs after it: write `LoopBehavior` before `LoopDuration`.
- An `hlsl { }` body is pasted into the shader as it stands: a `//` comment inside it can comment out the rest
  of the expression, and the Niagara compile fails with `DFX6001`. Put comments above it.
- Leave out `ParticleState()` and every age-driven input silently stays at age zero.
- `lint` checks a file without touching assets; `build -Force` proves it; the exit code is the error count.

<!-- Generated by .skill/gen-knowledge.ps1 from .skill/cheatsheet/cheatsheet.src.md. Edit that file, not this one. -->
