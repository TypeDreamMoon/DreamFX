---
name: dream-fx-diagnose
description: Resolve a DreamFX compile error or warning — look the DFXnnnn code up, explain the cause, and fix the source. Use when a .dfs / .dfe / .dfm fails to build, when a LogDreamFX error needs explaining, or when a DreamFX-generated Niagara effect comes out wrong.
---

# dream-fx-diagnose `<message>` | `<file>`

Every DreamFX diagnostic carries a stable `DFXnnnn` code, a file, and a line and column. The code
identifies the failure mode; the position identifies the token. Start from the code.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build <file> -Force
```

## Ranges

| Range | Stage | What it means |
| :-- | :-- | :-- |
| `DFX1xxx` | lexical | unterminated string or comment, character the lexer cannot read |
| `DFX2xxx` | syntax | the shape of the file is wrong |
| `DFX3xxx` | resolution | a name does not resolve to an asset, an input or a setting |
| `DFX4xxx` | types | the value does not fit the thing it is assigned to |
| `DFX5xxx` | generation | the asset write itself was refused |
| `DFX6xxx` | Niagara | Niagara's own compile events, mapped back onto source |
| `DFX7xxx` | drift / lint | `verify` found the asset out of step, or a static check fired |

## The ones you will actually hit

### `DFX3003` — module has no input named X

The name does not exist on that module. The message lists every input that does, in DSL spelling,
and suggests the nearest match. Confirm against the real signature:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema <Module> -Stack ParticleUpdate
```

Remember the two spelling rules: Niagara input names contain spaces and DreamFX identifiers do not
(`Loop Duration` → `LoopDuration`), and checkbox inputs are namespaced (`Module.WriteLifetime` →
`WriteLifetime`).

### `DFX5021` — refusing to set input: hidden, or EditCondition false

**Nearly always a write-ordering problem, not a wrong name.** The input is gated by a static switch
or a checkbox that has not been set yet, so at this moment it is not part of the executing graph.
Source order is write order, so move the gate above the thing it gates:

```cpp
EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.15);
InitializeParticle(WriteSpriteSize = true, SpriteSize = (6, 6));
ScaleSpriteSize(ScaleSpriteSizeMode = Uniform, UniformScaleFactor = 2.0);
```

`dfx.ps1 schema <Module>` marks the gates with `[static-switch]`.

### `DFX4006` — enum has no entry named X

The message lists the valid entries. Niagara's user-defined enum assets store meaningless internal
names (`NewEnumerator0`) and carry the real label in the display text, sometimes as
`Complete (Let Particles Finish then Kill Emitter)` — write just the label, `Complete`.

### `DFX4027` — linked parameter type mismatch

A link binds a parameter straight to an input with no conversion step. `float` driving an `int32`
input is not narrowed, it is wrong. Change the declaration in `Properties` to match the input.

### `DFX4003` — narrowing conversion

`24.0` written where an `int` is expected. `int → float` widens implicitly; `float → int` never does,
because a silently truncated spawn count is one of the hardest VFX bugs to trace back to a line.

### `DFX4030` — hlsl block must be a single expression

A stack input's custom HLSL lowers to a node with one typed output pin and no body, so it cannot
hold statements, locals or a `return`. Either collapse it into one expression, or move the logic into
a `.dfm` `DynamicInput` and call that.

### `DFX4031` — not an allowed inline function

The L6 whitelist is `normalize saturate clamp lerp frac min max abs floor ceil pow sqrt dot cross
length`, plus `+ - * / %` and unary minus. The boundary exists to stop a general expression backend
growing here. Use `hlsl { }` for a one-off, or a `.dfm` dynamic input for something reusable.

### `DFX3001` — no module named X

Check what the search paths actually expose:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list
```

The message includes how many assets were indexed. A count of zero means the search paths are wrong;
a large count means the name is. Add the folder to `Settings.ModulePaths`, or write the full path.

If instead the name is **ambiguous**, the message lists every candidate — disambiguate with a
trailing partial path (`Spawn/Initialization/InitializeParticle`), which is anchored at the end and
so will not also match a `V2/` sibling.

### `DFX4022` — cannot infer the type

A first assignment to a new attribute was given a value that carries no type: an expression, an
`hlsl` block, a dynamic input. Write the type:

```cpp
Color Particles.Color = hlsl { float4(Particles.Color.rgb, 0.5) };
```

### `DFX6001` / `DFX6003` — Niagara's own errors

The text comes from the Niagara compiler, and the position is the enclosing stack (compile events
carry only an emitter and a script, not a module). Common causes: an attribute read before anything
writes it, or a module in a stack it does not support.

### `DFX7001` / `DFX7002` — drift

`verify` found the asset out of step with its source: `7001` means no provenance stamp at all (never
generated, or created by hand), `7002` means the source changed since the last build. Both are fixed
by running `build`.

### `DFX7101` / `DFX7102` / `DFX7103` — lint

Warnings, never errors. GPU emitter with no `FixedBounds`; rate-based spawning with no allocation
cap; randomness with no `Determinism`. Each message says what to add.

## When it compiles but looks wrong

The build gate proves the asset generates, not that the effect is right. Two things to check first:

- **An undeclared system stack keeps whatever it already held.** A `DFX5003` info line lists what was
  left in place. Declaring `SystemUpdate = { }` is how you take ownership of it.
- **Stack issues are not reported headlessly** — reading them needs a Slate-backed view model, which
  a commandlet does not have. Open the generated asset in the editor to see them.

## See also

- [`dream-fx-verify`](../dream-fx-verify/SKILL.md) — the build harness and its flags
- [`dream-fx-create`](../dream-fx-create/SKILL.md) — writing a new effect from scratch
