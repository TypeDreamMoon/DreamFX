# Values, types and the `L*` rules

Everything that can appear on the right of an `=`, and the rules that decide what it means.

---

## The eight rules, by name

| | Rule |
| --- | --- |
| **L1** | A stack is an ordered statement block. Six of them: `SystemSpawn`, `SystemUpdate`, `EmitterSpawn`, `EmitterUpdate`, `ParticleSpawn`, `ParticleUpdate`. Writing order is module order. |
| **L2** | Two statement forms: a module call, and an assignment. **Consecutive assignments fold into one Set Parameters module**; a module call breaks the run. A new attribute is declared by its first write. |
| **L3** | Four value modes: literal, linked, dynamic input, HLSL. |
| **L4** | Module names resolve through `Settings.ModulePaths`; write a longer path only when a short name is ambiguous. |
| **L5** | `#Region` is a comment. It does not reach the asset — the external edit API has no stack-note function. |
| **L6** | Inline expressions lower to one HLSL expression, and only whitelisted functions are allowed. |
| **L7** | Numeric conversion widens, never narrows. `int -> float` is implicit; `float -> int` needs `int(...)`. |
| **L8** | Renderer properties are schema-driven generic assignment; every renderer type is supported without per-type syntax. |

---

## L3 — the four value modes

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

### Literals

```cpp
450.0        24           true
(1, 0, 0)    (1.0, 0.72, 0.25, 1.0)      // 2, 3 or 4 components
"/Game/FX/M_Spark"                        // asset path
box(-200, -200, -50, 200, 200, 300)       // six numbers: min xyz, max xyz
```

`24` and `24.0` are different: the first is an integer literal, and L7 uses that distinction.

### Names that are not identifiers

Niagara names come from a UI with no restrictions. Real content in this project has a user parameter
called `PillarPower(0~1)` and a module input called `Ring/DiscDistributionMode`; neither is an
identifier in any language. Back-quotes hold one:

```cpp
Properties = {
    float `PillarPower(0~1)`;
}

ParticleSpawn = {
    SphereLocation(
        `Ring/DiscDistributionMode` = Direct,
        Alpha                       = `User.PillarPower(0~1)`,
    );
}
```

A back-quoted name is **one token**, dots included — `` `User.PillarPower(0~1)` ``, not
``User.`PillarPower(0~1)` ``. Anywhere a name may appear, this may appear.

Only quote what needs it: `` `Speed` `` is legal but reads worse than `Speed`. The decompiler applies
exactly that rule, so an export shows which names in a given asset need quoting. A name containing a
back-quote has no written form at all — rename the parameter. An unterminated one is
[DFX1005](../diagnostics/DFX1xxx.md#dfx1005).

Enum entries are written by name, with spaces and hyphens removed — Niagara's `Direct Set` is
`DirectSet`. The names are read from the live asset, because a user-defined Niagara enum stores
`NewEnumerator0` internally and keeps the real name in display text; an unknown one reports the real
list (DFX4006).

### Linked

```cpp
SpawnCount = User.SparkCount;
ConeAxis   = Particles.Velocity;
```

Any namespace-qualified parameter: `User.`, `Particles.`, `Emitter.`, `System.`, `Engine.`.

A link binds the parameter **directly** — there is no conversion step anywhere in it. So the two types
have to match exactly, and DFX4027 is the one L7 case an explicit cast cannot fix, because there is
nowhere to put one. A `float` user parameter cannot drive an `int32` spawn count; declare it `int`.

### Dynamic input

```cpp
UniformScaleFactor = FloatFromCurve(
    FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
    CurveIndex = Particles.NormalizedAge
)
```

Nests to any depth — a chain is just a longer address, written parent-first.

### HLSL

```cpp
Color Particles.Color = hlsl {
    float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge))
};
```

Raw and multi-line, no escaping. Namespaced parameters are read as written.

**One expression only** (DFX4030). A stack input's HLSL lowers to a node with one typed output pin and
no body, so there is nowhere for a statement to go. Multi-statement logic belongs in a
[`.dfm` Module](dfm.md), whose body is emitted verbatim.

An `hlsl` block carries no type of its own, so an assignment to a new attribute needs the annotation:
`Color Particles.Color = hlsl { ... };` (DFX4022).

---

## `curve { }`

```cpp
curve {
    0.0 -> 1.0;
    0.7 -> 0.85 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ];
    0.9 -> 0.40 [ Interp=Cubic; Tangent=Break; Arrive=0.0; Leave=-2.25 ];
    1.0 -> 0.0  [ Interp=Linear ];
}
```

Fills a curve data interface, so it goes where one is expected — usually a dynamic input such as
`FloatFromCurve` (DFX4037).

Tangents are per key and are exported per key. They matter: a hand-tuned shape written back without its
tangents is a different curve, and losing shape is losing data. `Interp` defaults to `Auto`.

`Tangent` names the tangent mode — `Auto`, `User`, `Break` or `None` — and may be left out, in which
case a key with a tangent means `User` and a key without means `Auto`. Only `Break` and `None` ever
have to be written, plus the `Auto` key that carries a stored slope, so ordinary hand-written sources
never mention it. **`Break` is the one that had to be spelled**: it is how a key holds two
independent tangents — a corner — and until 2026-08-12 the exporter wrote tangents only under `User`
and dropped everything else, so every corner came back smooth and every Auto key's stored slope came
back zero. A curve is *evaluated* from its stored tangents whatever the mode says; the engine
re-derives them only when something edits the curve, so "the mode is Auto, the engine will work them
out" is false for a curve nobody is editing.

A curve data interface with anything set outside its keys — an exposed curve, a LUT turned off, an
external curve asset — is written as raw configuration JSON instead of a `curve { }` literal, and a
multi-channel interface (vector, colour) takes the readable form only when its channels are the same
shape. The readable form is a convenience, and a convenience does not get to lose data.

---

## L6 — inline expressions

```cpp
VelocityStrength = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed)
StretchDir       = normalize(Particles.Velocity)
```

Arithmetic (`+ - * /`), unary minus, parentheses, and these functions:

```
normalize  saturate  clamp  lerp  frac  min  max
abs  floor  ceil  pow  sqrt  dot  cross  length
```

The whole expression lowers to one HLSL expression dynamic input, with the type inferred from the
operands. Only namespace-qualified parameters exist inside one — there are no locals (DFX4032). The
builtins are positional, not named (DFX4033).

**Anything outside the list is an error** (DFX4031), and the list is short on purpose. Widening it is
the first step toward re-implementing a general expression compiler, which is the ~13k-line component
DreamShader has and DreamFX exists partly to avoid. `hlsl { }` is the escape hatch: inside one, any
HLSL is allowed.

Expressions are **not recovered by the decompiler** — they come back as an equivalent `hlsl { }`
block. The round trip is semantic, not textual.

---

## L7 — numeric conversion

| From → to | |
| --- | --- |
| `int` → `float` | implicit |
| `float` → `int` | error; write `int(...)` (DFX4003) |
| mismatched vector widths | error (DFX4002) |

A silently truncated spawn count is among the hardest effect bugs to find, which is the whole reason
the rule is one-directional.

---

## Types

| DSL | Niagara |
| --- | --- |
| `float` `int` `bool` | `NiagaraFloat` / `NiagaraInt32` / `NiagaraBool` |
| `Vector2` `Vector` `Vector4` | 2 / 3 / 4 floats |
| `Color` | `LinearColor` |
| `Position` | `NiagaraPosition` |
| `Quat` | `Quat` |
| `Texture2D`, `DI<X>` | data interfaces |

Data interface parameters carry their configuration as a quoted JSON object, the same verbatim form a
module's data interface input uses and the same form the exporter writes. Declared bare, they stay a
slot to fill at runtime. `curve { }` is the readable spelling for a curve interface. DFX5098 means
the value is the wrong shape — it no longer means "declared only, will be ignored".
