# `.dfm` — a module or a dynamic input

Produces one `UNiagaraScript`. Two kinds, differing in two lines:

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

Used from a `.dfs` like anything else:

```cpp
ToonSpin(SpinRate = 220.0, bClockwise = true);
ScaleSpriteSize(ScaleSpriteSizeMode = Uniform, UniformScaleFactor = ToonPulse(Frequency = 4.0));
```

---

## Which engines can generate one

Writing HLSL onto a Niagara custom node needs `UNiagaraNodeCustomHlsl::SetCustomHlsl`, and building
the graph around it needs four more declarations. MoonEngine puts an export macro on all five; a
stock engine exports none of them. `DreamFXEditor.Build.cs` probes the engine headers and defines
`DREAMFX_HAS_CUSTOMHLSL_WRITE` accordingly.

That is no longer the end of the story, because *exported* and *reachable* are not the same thing.
Public data members need no export macro, public virtuals dispatch through the vtable, and a private
field that is a `UPROPERTY` can be written by name. Every behaviour behind those five declarations
turned out to be reachable that way, so there are two backends and three outcomes:

| | When | Behaviour |
| --- | --- | --- |
| **direct** | the engine exports the five declarations | calls them; this is MoonEngine |
| **reflection** | it does not, but the shapes it depends on all check out | rebuilds each operation from the public surface |
| **degraded** | a shape it depends on has moved | refuses to generate, and says which one |

The reflection backend is verified against the direct one rather than assumed equivalent: the same
`.dfm` built both ways reads back with a byte-identical schema, and that holds across engines —
a module generated on stock UE 5.8 matches the same module generated on MoonEngine.

`-DreamFXForceReflectionBackend` (or `dfx.ps1 -ForceReflectionBackend`) selects the reflection
backend on an engine that does not need it, which is how the two are diffed on one machine.

**The product was never the limited part.** A generated module is an ordinary `UNiagaraScript`: any
engine loads it, references it from a `.dfs`, cooks it and runs it. *Generate anywhere it works,
commit the asset, everyone consumes it* — and on a team with MoonEngine, generating there stays the
recommended path simply because it is the one the whole corpus is verified against.

When neither backend can run:

| Situation | What happens |
| --- | --- |
| asset committed and matching the source | build skips it, CI stays green |
| source edited without regenerating | **DFX5107** — regenerate where a backend runs |
| no asset at all | **DFX5100** — names the check that failed |

The provenance check runs either way; only the remedy differs.

---

## `Settings`

| Key | Meaning |
| --- | --- |
| `Usage` | which stack(s) the module may be placed in, or `DynamicInput`. One of the six stack names (L1), or an array of them: `Usage = [ParticleSpawn, ParticleUpdate];` |
| `Output` | a `DynamicInput`'s return type. Required (DFX3031); a value type, not a data interface (DFX3039) |
| `Category` | where it sits in the stack's add menu |
| `Description` | the module's tooltip |

A `DynamicInput` with no explicit stack list defaults to particle spawn and update — the two that
cover nearly everything. Widen it with the array form.

## `Inputs`

Each becomes a `Module.<Name>` input with the declared default and description. Defaults must be
literals or enum entries, because a module input's default is stored on the asset and cannot reference
anything outside the module (DFX3044).

Attributes: `[ Description="..." ]`, `[ Advanced ]`, `[ StaticSwitch ]`.

`[StaticSwitch]` is accepted, validated (bool/int/enum, constant default — DFX3034/DFX3035) and then
**lowered as an ordinary input**, with DFX5102 saying so. Tier-one generation puts the whole body in
one custom HLSL node, which has no branch for a switch to select. The body reads it identically; what
is lost is the compile-time folding.

## `Body`

The body is HLSL, with DreamFX's namespaces on top.

**Its own inputs are bare names.** `SpinRate`, not `Module.SpinRate` — inside a module the namespace
is implied. The qualified form is accepted and normalised away, because it is not wrong about the
language, but bare is the spelling that describes what is happening: the input arrives as a pin.

**`Engine.`, `User.`, `System.` and `Emitter.` are read as written.** They resolve against the
parameter map directly.

**`Particles.*` is read and written as written**, and DreamFX wires the pins for it:

```cpp
Body = {
    Particles.SpriteRotation += SpinRate * Engine.DeltaTime;   // read and write, both handled
}
```

An attribute Niagara already knows (`Particles.SpriteRotation`, `Particles.Color`, …) needs nothing.
A custom one needs its type at first use, the same way a `.dfs` declares a new attribute — otherwise
the pin would be wired at a guessed width (DFX3046):

```cpp
Body = {
    float Particles.Moon.SpinPhase = 0.0;
    Particles.Moon.SpinPhase += Engine.DeltaTime;
}
```

`Particles.Color.rgb` resolves to `Particles.Color` with a swizzle: the longest dotted prefix that is a
known or declared attribute wins.

### Calling a data-interface input

An input declared `DI<X> Name;` is a pin on the custom node, and its functions are called the way
Niagara custom HLSL calls them: `Name.Function(args)`. Declare the `out` arguments as **plain,
uninitialised locals**:

```cpp
Inputs = { DI<DreamWind> Wind; }
Body = {
    float3 WindVelocity;
    float  Gust;
    Wind.SampleWind(Particles.Position, WindVelocity, Gust);
    Particles.Velocity = WindVelocity;
}
```

`float Gust = 1.0;` before the call fails the CPU VM compile with `internal compiler error: out/inout
parameters must be lvalues in call to 'SampleWind_Module_Wind'` (DFX6006): the VM's HLSL front end,
hlslcc, constant-folds the initialised local into the argument list before it checks the out
parameter, and a constant is not an lvalue. GPU sims accept both spellings; write the uninitialised
form so one body compiles on both targets.

### Modules take statements; dynamic inputs take one expression

A module's body is emitted verbatim — locals, branches, as many statements as you like. **That is the
capability DFX4030 has always pointed at**, and the reason `.dfm` generation was worth unblocking:
an inline `hlsl { }` in a `.dfs` is a single rvalue and can never hold more.

A dynamic input's body is wrapped by the Niagara translator as `Output = (Type)( <body> );`, so it has
to be one expression, with or without the `return`. Statements before it produce invalid HLSL rather
than an error naming the real problem, so DreamFX catches it first (DFX3037) and points at the module
form.

---

## What tier one costs

The whole body becomes one `UNiagaraNodeCustomHlsl`, so the module is a black box in the node editor
(plan 3.3). For a text-first workflow that is the intent — the text is the source. What it buys over
an inline `hlsl { }` is everything a stack input cannot hold: statements, named inputs with defaults
and tooltips, and reuse by name across systems.

Lowering a body into a real node graph is tier two, and is not planned: it is the ~13k-line problem
DreamShader already has, reproduced.
