# DFX4xxx --- Values, types and expressions

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX4001

<!-- generated:begin DFX4001 -->
**Severity** error

**Message**

```
Input '%s' expects %s, but a number was written.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:355`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:365`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:380`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:426`
<!-- generated:end DFX4001 -->

**Cause.** A bare number was written where the input wants a vector, colour or other structured type.

**Fix.** Write all the components: `(0, 0, -980)`.

## DFX4002

<!-- generated:begin DFX4002 -->
**Severity** error

**Message**

```
Expression for '%s': a vector literal must have 2 to 4 components.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:119`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:203`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:287`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:349`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:388`
<!-- generated:end DFX4002 -->

**Cause.** Niagara vector types are 2, 3 or 4 components.

**Fix.** Check the component count.

## DFX4003

<!-- generated:begin DFX4003 -->
**Severity** error

**Message**

```
Input '%s' is an integer, but %s was written. Narrowing is not implicit -- write int(%s) if truncation is intended.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:336`
<!-- generated:end DFX4003 -->

**Cause.** L7: `float -> int` is never implicit. A silently truncated spawn count is among the hardest effect bugs to find.

**Fix.** Write `int(...)` if truncation is what you meant, or make the source an int.

## DFX4004

<!-- generated:begin DFX4004 -->
**Severity** error

**Message**

```
Input '%s' has no resolvable Niagara type.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:306`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:415`
<!-- generated:end DFX4004 -->

**Cause.** The input's type could not be resolved from the module schema.

**Fix.** Usually the input is hidden behind a static switch that has not been written yet -- write the switch first; source order is write order.

## DFX4005

<!-- generated:begin DFX4005 -->
**Severity** error

**Message**

```
Property '%s': component %d is not a number.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:217`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:244`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:777`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:954`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:402`, `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:473`
<!-- generated:end DFX4005 -->

**Cause.** A vector or `box()` component is not a number.

**Fix.** Components are literals; a parameter reference cannot appear inside one.

## DFX4006

<!-- generated:begin DFX4006 -->
**Severity** error

**Message**

```
Input '%s' is a %s, which has no entry named '%s'. Valid entries: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:447`
<!-- generated:end DFX4006 -->

**Cause.** The enum has no entry by that name. Niagara's user-defined enums store `NewEnumerator0` internally and keep the real name in display text, so the names here are not guessable from the asset -- they are read live.

**Fix.** Use one of the entries listed. Spaces and hyphens are normalised away, so `Direct Set` is written `DirectSet`.

## DFX4007

<!-- generated:begin DFX4007 -->
**Severity** error

**Message**

```
Input '%s' expects %s. '%s' is neither a literal of that type nor a parameter reference -- parameter references start with a namespace such as User., Particles., Emitter., System. or Engine.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:456`
<!-- generated:end DFX4007 -->

**Cause.** The value is neither a literal of the expected type nor a namespace-qualified parameter.

**Fix.** Qualify the parameter (`User.Speed`, `Particles.Velocity`), or write a literal.

## DFX4010

<!-- generated:begin DFX4010 -->
**Severity** error

**Message**

```
Input '%s' is set more than once on dynamic input '%s'.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:691`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:944`
<!-- generated:end DFX4010 -->

**Cause.** The same input was given twice on one dynamic input call.

**Fix.** Delete one.

## DFX4020

<!-- generated:begin DFX4020 -->
**Severity** error

**Message**

```
Parameter '%s': 'DI' needs an inner type, e.g. DI<SkeletalMesh>.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:216`
<!-- generated:end DFX4020 -->

**Cause.** `DI` names no class on its own.

**Fix.** Write the inner type: `DI<SkeletalMesh>`.

## DFX4021

<!-- generated:begin DFX4021 -->
**Severity** error

**Message**

```
Parameter '%s' has unknown type '%s'. Expected float, int, bool, Vector2, Vector, Vector4, Color, Position, Quat, or a data interface written as DI<Name>.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:250`
<!-- generated:end DFX4021 -->

**Cause.** The declared type is not one DreamFX knows.

**Fix.** Use one of the types listed in the message.

## DFX4022

<!-- generated:begin DFX4022 -->
**Severity** error

**Message**

```
Cannot infer the type of '%s' from its value. A first assignment to a new attribute must use a literal, so its type is unambiguous.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:294`
<!-- generated:end DFX4022 -->

**Cause.** L2 types a new attribute from its first value, and this value carries no type -- an `hlsl` block, a dynamic input and an inline expression all have none of their own.

**Fix.** Write the type: `Color Particles.Moon.Tint = hlsl { ... };`

## DFX4023

<!-- generated:begin DFX4023 -->
**Severity** error

**Message**

```
Property '%s': box() takes 6 numbers -- minX, minY, minZ, maxX, maxY, maxZ.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:317`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:329`
<!-- generated:end DFX4023 -->

**Cause.** `box()` is minX, minY, minZ, maxX, maxY, maxZ.

**Fix.** Write six numbers.

## DFX4024

<!-- generated:begin DFX4024 -->
**Severity** error

**Message**

```
Cannot type '%s = %s': the type of '%s' is not known here. Declare it in Properties, or assign a literal first so the type is explicit.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:509`
<!-- generated:end DFX4024 -->

**Cause.** The right-hand side references a parameter whose type is not known at this point.

**Fix.** Declare it in `Properties`, or assign it a literal earlier in the same stack -- L2's first write is what declares an attribute.

## DFX4025

<!-- generated:begin DFX4025 -->
**Severity** error

**Message**

```
'%s' is not a valid assignment target. Parameter names are namespace-qualified, e.g. Particles.MyValue or Emitter.MyCounter.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:768`
<!-- generated:end DFX4025 -->

**Cause.** An unqualified assignment target has no namespace to live in, and guessing one would put the value somewhere never named.

**Fix.** Qualify it: `Particles.MyValue`, `Emitter.MyValue`, `System.MyValue`.

## DFX4026

<!-- generated:begin DFX4026 -->
**Severity** error

**Message**

```
'Bind %s -> %s': the target must be a namespace-qualified parameter, e.g. Particles.SpriteSize.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:993`
<!-- generated:end DFX4026 -->

**Cause.** A renderer binding target is a parameter, not a bare name.

**Fix.** Write `Bind Color -> Particles.Color;`

## DFX4027

<!-- generated:begin DFX4027 -->
**Severity** error

**Message**

```
'%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Declare '%s' as %s, or drive the input another way.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:550`
<!-- generated:end DFX4027 -->

**Cause.** L7 across a link. A linked value binds the parameter directly -- there is no conversion step to truncate or widen in -- so the two types have to match exactly.

**Fix.** Declare the source parameter with the target's type. This is the one L7 case that cannot be fixed with an explicit cast, because there is nowhere to put one.

## DFX4030

<!-- generated:begin DFX4030 -->
**Severity** error

**Message**

```
The hlsl block for '%s' must be a single expression: no statements, no local variables, no return. Move multi-statement logic into a .dfm DynamicInput and call it here.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:315`
<!-- generated:end DFX4030 -->

**Cause.** A stack input's `hlsl` block lowers to a node with one typed output pin and no body, so it cannot hold statements.

**Fix.** Fold it into one expression, or move the logic into a `.dfm` Module, whose body is emitted verbatim (plan-v2 W1). This is the gap `.dfm` generation exists to close.

## DFX4031

<!-- generated:begin DFX4031 -->
**Severity** error

**Message**

```
'%s' is not an allowed inline function. Allowed: %s. Anything else belongs in a .dfm dynamic input or an hlsl { } block.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:158`
<!-- generated:end DFX4031 -->

**Cause.** L6's whitelist. Widening it is the slide into re-implementing a 13k-line expression backend, so it is deliberately short.

**Fix.** Use one of the listed functions, a `.dfm` dynamic input, or an `hlsl { }` block -- inside which any HLSL is allowed.

## DFX4032

<!-- generated:begin DFX4032 -->
**Severity** error

**Message**

```
'%s' in the expression for '%s' is not a parameter. Only namespace-qualified parameters (User.X, Particles.X, Engine.X, ...) can be read from an expression.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:100`
<!-- generated:end DFX4032 -->

**Cause.** Inline expressions see parameters, not locals. A bare name has nothing to bind to.

**Fix.** Qualify it, or move the expression into an `hlsl { }` block where locals exist.

## DFX4033

<!-- generated:begin DFX4033 -->
**Severity** error

**Message**

```
Builtin '%s' takes positional arguments, not named ones.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:166`
<!-- generated:end DFX4033 -->

**Cause.** An L6 builtin is positional. Named arguments read as a dynamic input call and would silently resolve to something else.

**Fix.** Write `saturate(x)`, not `saturate(Value = x)`.

## DFX4034

<!-- generated:begin DFX4034 -->
**Severity** error

**Message**

```
Builtin '%s' takes %d argument(s), but %d were written.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:174`
<!-- generated:end DFX4034 -->

**Cause.** Arity is the only thing separating `lerp(a, b)` from `lerp(a, b, t)`.

**Fix.** Check the argument count.

## DFX4035

<!-- generated:begin DFX4035 -->
**Severity** error

**Message**

```
Expression for '%s' contains a value that has no HLSL form.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:205`
<!-- generated:end DFX4035 -->

**Cause.** Part of the expression has no HLSL form -- a curve literal or a data interface inside arithmetic, for instance.

**Fix.** Move that part to its own input.

## DFX4036

<!-- generated:begin DFX4036 -->
**Severity** error

**Message**

```
The hlsl block for '%s' is empty.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:305`
<!-- generated:end DFX4036 -->

**Cause.** An empty `hlsl` block lowers to an expression with no text in it.

**Fix.** Write the expression, or delete the block.

## DFX4037

<!-- generated:begin DFX4037 -->
**Severity** error

**Message**

```
'%s' is %s; a curve { } literal only fits a curve data interface input.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:331`
<!-- generated:end DFX4037 -->

**Cause.** A `curve { }` literal fills a curve data interface, and this input is not one.

**Fix.** Feed the curve to the input that takes it -- usually a dynamic input such as `FloatFromCurve(FloatCurve = curve { ... })`.

## DFX4038

<!-- generated:begin DFX4038 -->
**Severity** error

**Message**

```
The curve for '%s' has no keys.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:339`
<!-- generated:end DFX4038 -->

**Cause.** An empty curve evaluates to nothing, which reads at runtime as a value stuck at zero.

**Fix.** Add keys.

## DFX4039

<!-- generated:begin DFX4039 -->
**Severity** error

**Message**

```
Unknown curve interpolation '%s'. Expected Auto, Cubic, Linear or Constant.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXExpressions.cpp:353`
<!-- generated:end DFX4039 -->

**Cause.** Curve tangents are data (plan 3.5); an unrecognised mode would fall back to Auto and drop a hand-tuned shape without saying so.

**Fix.** Use `Auto`, `Cubic`, `Linear` or `Constant`.

## DFX4091

<!-- generated:begin DFX4091 -->
**Severity** error

**Message**

```
Input '%s': dynamic inputs, hlsl blocks, curves and inline expressions are not available yet (planned for Phase 3).
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:467`
<!-- generated:end DFX4091 -->

**Cause.** A Phase 2 build met a Phase 3 value mode. Historical -- it should not appear on a current build.

**Fix.** Rebuild the plugin. If it persists, the source is being compiled by an old binary.

