---
name: dream-fx-create
description: Write a new DreamFXLang Niagara effect from a plain-language description, then build it headlessly to prove it compiles. Use when asked to create, author, add, or write a .dfs / .dfe / .dfm, a DreamFX effect, or a text-authored Niagara system for an Unreal project.
---

# dream-fx-create `<description>`

Writes a `.dfs` and proves it builds. The proving is not optional — module input names are read off
real assets, and nobody, including you, can guess them reliably.

Paths below are relative to the plugin root, `Plugins/DreamFX/`.

## The loop

1. **Find the modules.** `dfx.ps1 list` prints every module the search paths expose.
2. **Read their real signatures.** `dfx.ps1 schema <Module>` — once per module you intend to call.
   This is the step people skip and then spend twenty minutes on.
3. **Write the file** under a `DFX/` root.
4. **Build it.** `dfx.ps1 build <file> -Force`.
5. **Fix and repeat.** Diagnostics carry line and column; see
   [`dream-fx-diagnose`](../dream-fx-diagnose/SKILL.md) for what each code means.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema AddVelocityInCone
```

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Samples/NS_MyEffect.dfs -Force
```

## Skeleton

```cpp
System(Name="Samples/NS_MyEffect", Root="Plugin.DreamFX")
{
    Settings   = { WarmupTime = 0.0; }
    Properties = { int Count = 24 [ Group="Burst" ]; }

    Emitter Sparks
    {
        Settings = { SimTarget = CPU; Determinism = true; RandomSeed = 1337; }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.15);
            SpawnBurst_Instantaneous(SpawnCount = User.Count);
        }

        ParticleSpawn = {
            Spawn/Initialization/InitializeParticle(WriteLifetime = true, Lifetime = 0.4);
            SystemLocation();
        }

        ParticleUpdate = {
            GravityForce(Gravity = (0, 0, -980));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core { Alignment = Unaligned; }
    }
}
```

## The five things that trip people up

1. **Write the static switch before the input it gates.** Source order is write order. `LifeCycleMode`
   gates `LoopBehavior`, which gates `LoopDuration`. `WriteSpriteSize = true` gates `SpriteSize`.
   Out of order you get `DFX5021`, saying the input is hidden or its EditCondition is false.
2. **Input names have spaces in Niagara and none in DreamFX.** `Loop Duration` is written
   `LoopDuration`. Checkbox inputs live in the `Module.` namespace and the prefix is dropped:
   `Module.WriteLifetime` is written `WriteLifetime`.
3. **Linked parameters do not convert.** `User.Count` on an int input must itself be `int`. A `float`
   there is `DFX4027`, and it is a real bug, not a pedantic one.
4. **`hlsl { }` must be a single expression.** No statements, no locals, no `return`. The node it
   lowers to has one output pin and no body to put them in. Multi-statement logic goes in a `.dfm`
   **`Module`**, whose body is emitted verbatim — not a `DynamicInput`, whose body the translator
   wraps as `Output = (Type)( ... );` and which is therefore under the same one-expression limit.
   See [`Docs/language/dfm.md`](../../Docs/language/dfm.md).
5. **A short module name that matches twice is refused, not guessed.** Disambiguate with a partial
   path: `Spawn/Initialization/InitializeParticle` selects that asset and not its `V2/` sibling.
6. **Generating a `.dfm` needs MoonEngine.** Writing HLSL onto a Niagara custom node uses export
   macros a stock engine does not have, so `.dfm` generation is probe-gated at build time. The
   *product* is unrestricted: generate the module on MoonEngine, commit the asset, and any engine
   references and cooks it. Elsewhere the build reports `DFX5100` (no asset) or `DFX5107` (asset out
   of date).

## Value forms

| Written | Becomes |
| :-- | :-- |
| `450.0`, `24`, `true`, `(1, 0, 0, 1)` | a literal |
| `Once`, `Complete`, `Uniform` | an enum entry — the label, not the internal name |
| `User.Speed`, `Particles.Velocity`, `Engine.Time` | a linked parameter |
| `RandomRangeFloat(Minimum = 1.0, Maximum = 2.0)` | a dynamic input; nests to any depth |
| `User.Speed * 0.6`, `normalize(Particles.Velocity)` | one HLSL expression (L6 whitelist) |
| `hlsl { saturate(1.0 - Particles.NormalizedAge) }` | a raw HLSL expression |
| `curve { 0.0 -> 1.0; 1.0 -> 0.0 [ Interp=Linear ]; }` | a curve data interface |

Inline arithmetic is limited to `+ - * / %`, unary minus, and
`normalize saturate clamp lerp frac min max abs floor ceil pow sqrt dot cross length`. Anything
else is `DFX4031` — that boundary is deliberate and widening it is a design decision, not a fix.

## Assignments

```cpp
Particles.Moon.SparkSeed = 0.5;          // type inferred from the literal
Color Particles.Color    = hlsl { ... }; // type written out: an expression carries none
```

Consecutive assignments fold into one Set Parameters module. A module call between them starts a new
group. A first assignment declares the attribute; write the type explicitly whenever the value is an
expression, an `hlsl` block or a dynamic input, because inference only works on literals.

## Where the file goes

`Name=` is a package path relative to the root named by `Root=`:

| `Root=` | Resolves to |
| :-- | :-- |
| omitted, or `"Game"` | `/Game` |
| `"Plugin.MoonToon"` | `/MoonToon` |

Source lives under a `DFX/` directory — the project's, or a plugin's. `-All` finds every one.

## See also

- [`Docs/language/`](../../Docs/language/README.md) — the full syntax, one page per file kind plus
  the value modes and the `L*` rules
- [`Docs/getting-started.md`](../../Docs/getting-started.md) — the same loop as prose, for a human
- [`dream-fx-verify`](../dream-fx-verify/SKILL.md) — the build harness and its flags
- [`dream-fx-diagnose`](../dream-fx-diagnose/SKILL.md) — what each diagnostic code means
