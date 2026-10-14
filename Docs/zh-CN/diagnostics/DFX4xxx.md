---
title: DFX4xxx — 值、类型与表达式
description: DFX4xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 32 条（32 错误）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx4001"></a>

## DFX4001

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' expects %s, but a number was written.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:520, Generation/DreamFXValueLowering.cpp:530, Generation/DreamFXValueLowering.cpp:545, Generation/DreamFXValueLowering.cpp:621`

**原因.** 输入要的是向量、颜色或别的结构化类型，写进去的却是一个裸数字。

**修复.** 把分量都写出来：`(0, 0, -980)`。

<a id="dfx4002"></a>

## DFX4002

**严重级别：** 错误 (`error`)

**消息：**

```text
Expression for '%s': a vector literal must have 2 to 4 components.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:119, Generation/DreamFXGenerator.cpp:331, Generation/DreamFXValueLowering.cpp:444, Generation/DreamFXValueLowering.cpp:514, Generation/DreamFXValueLowering.cpp:553`

**原因.** Niagara 的向量类型是 2、3 或 4 个分量。

**修复.** 数一下分量个数。

<a id="dfx4003"></a>

## DFX4003

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is an integer, but %s was written. Narrowing is not implicit -- write int(%s) if truncation is intended.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:501`

**原因.** L7：`float -> int` 永远不隐式。一个被静默截断的 spawn count 属于最难找的那类特效 bug。

**修复.** 真的要截断就写 `int(...)`，否则把源头改成 int。

<a id="dfx4004"></a>

## DFX4004

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' has no resolvable Niagara type.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:463, Generation/DreamFXValueLowering.cpp:580`

**原因.** 从模块 schema 里解析不出这个输入的类型。

**修复.** 通常是这个输入藏在一个还没写的静态开关后面 —— 先写开关；源码顺序就是写入顺序。

<a id="dfx4005"></a>

## DFX4005

**严重级别：** 错误 (`error`)

**消息：**

```text
Property '%s': component %d is not a number.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1122, Generation/DreamFXGenerator.cpp:1443, Generation/DreamFXGenerator.cpp:345, Generation/DreamFXGenerator.cpp:372, Generation/DreamFXValueLowering.cpp:567, Generation/DreamFXValueLowering.cpp:669`

**原因.** 向量或 `box()` 的某个分量不是数字。

**修复.** 分量只能是字面量，参数引用不能出现在里面。

<a id="dfx4006"></a>

## DFX4006

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is a %s, which has no entry named '%s'. Valid entries: %s
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:643`

**原因.** 这个枚举没有叫这个名字的条目。Niagara 的用户自定义枚举内部存的是 `NewEnumerator0`，
真名留在显示文本里，所以这些名字从资产上是猜不出来的 —— 它们是实时读的。

**修复.** 用列出来的条目之一。空格和连字符会被归一化掉，所以 `Direct Set` 写成 `DirectSet`。

<a id="dfx4007"></a>

## DFX4007

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' expects %s. '%s' is neither a literal of that type nor a parameter reference -- parameter references start with a namespace such as User., Particles., Emitter., System. or Engine.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:652`

**原因.** 这个值既不是期待类型的字面量，也不是带命名空间的参数。

**修复.** 给参数加上命名空间（`User.Speed`、`Particles.Velocity`），或者写一个字面量。

<a id="dfx4010"></a>

## DFX4010

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is set more than once on dynamic input '%s'.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1032, Generation/DreamFXGenerator.cpp:1433`

**原因.** 同一次 dynamic input 调用里，同一个输入被给了两次。

**修复.** 删掉一个。

<a id="dfx4020"></a>

## DFX4020

**严重级别：** 错误 (`error`)

**消息：**

```text
Parameter '%s': 'Object' needs an inner type, e.g. Object<Texture>.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:328, Generation/DreamFXValueLowering.cpp:373`

**原因.** `DI` 自己不指任何类。

**修复.** 把内层类型写出来：`DI<SkeletalMesh>`。

<a id="dfx4021"></a>

## DFX4021

**严重级别：** 错误 (`error`)

**消息：**

```text
Parameter '%s': no UObject class named '%s'. Object<> takes a class name such as Object<Texture> or Object<StaticMesh>.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:361, Generation/DreamFXValueLowering.cpp:407`

**原因.** 声明的类型不是 DreamFX 认识的那些。

**修复.** 用消息里列出来的类型之一。

<a id="dfx4022"></a>

## DFX4022

**严重级别：** 错误 (`error`)

**消息：**

```text
Cannot infer the type of '%s' from its value. A first assignment to a new attribute must use a literal, so its type is unambiguous.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:451`

**原因.** L2 用第一个值给新属性定类型，而这个值自己不带类型 —— `hlsl` 块、dynamic input 和内联表达式都没有自己的类型。

**修复.** 把类型写上：`Color Particles.Moon.Tint = hlsl { ... };`

<a id="dfx4023"></a>

## DFX4023

**严重级别：** 错误 (`error`)

**消息：**

```text
Property '%s': box() takes 6 numbers -- minX, minY, minZ, maxX, maxY, maxZ.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:457, Generation/DreamFXGenerator.cpp:469`

**原因.** `box()` 是 minX、minY、minZ、maxX、maxY、maxZ。

**修复.** 写六个数。

<a id="dfx4024"></a>

## DFX4024

**严重级别：** 错误 (`error`)

**消息：**

```text
Cannot type '%s = %s': the type of '%s' is not known here. Declare it in Properties, or assign a literal first so the type is explicit.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:735`

**原因.** 右边引用了一个此处还不知道类型的参数。

**修复.** 在 `Properties` 里声明它，或者在同一个栈里更早的位置先赋一个字面量 —— L2 的第一次写入才是声明属性的那一次。

<a id="dfx4025"></a>

## DFX4025

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is not a valid assignment target. Parameter names are namespace-qualified, e.g. Particles.MyValue or Emitter.MyCounter.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1113, Generation/DreamFXGenerator.cpp:1938`

**原因.** 不带命名空间的赋值目标没有落脚的地方，而猜一个出来会把值放到一个从没被点名的位置。

**修复.** 加上命名空间：`Particles.MyValue`、`Emitter.MyValue`、`System.MyValue`。

<a id="dfx4026"></a>

## DFX4026

**严重级别：** 错误 (`error`)

**消息：**

```text
'Bind %s -> %s': the target must be a qualified parameter -- either a namespace such as Particles.SpriteSize, or an emitter's own alias such as MyEmitter.Velocity.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1497`

**原因.** renderer 绑定的目标是一个参数，不是一个裸名字。

**修复.** 写 `Bind Color -> Particles.Color;`

<a id="dfx4027"></a>

## DFX4027

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Declare '%s' as %s, or drive the input another way.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:782`

**原因.** 跨链接的 L7。链接是直接绑定参数 —— 中间没有可以截断或加宽的转换步骤 —— 所以两边类型必须严格一致。

**修复.** 把源参数声明成目标的类型。这是 L7 里唯一一个显式 cast 也救不了的情况，因为没有地方放那个 cast。

<a id="dfx4028"></a>

## DFX4028

**严重级别：** 错误 (`error`)

**消息：**

```text
Default for '%s' needs a declared type. A default has no module signature to infer one from, so write it: `float %s = …`.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1946`

**原因.** 一条 `Defaults` 条目没写类型。在别处类型可以来自值、或来自输入所属的模块签名；
而默认值不属于任何模块，它的值又可能是 `hlsl` 块或链接，于是没有可推断的来源。

**修复.** 把类型写上：`float Particles.MySize = 1.0;`。

<a id="dfx4029"></a>

## DFX4029

**严重级别：** 错误 (`error`)

**消息：**

```text
Default for '%s' must be a literal, an enum or another parameter. A dynamic input computes a value per particle, which a default cannot do.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1983`

**原因.** 一条 `Defaults` 条目被喂了 dynamic input。默认值是存在参数上的**一个**值，
而 dynamic input 是一张逐粒子运行的图 —— 没有地方运行它。

**修复.** 用字面量、枚举，或者链接到另一个参数。真的需要计算，就在 `ParticleSpawn` 里写成赋值 —— 那里是会运行的。

<a id="dfx4030"></a>

## DFX4030

**严重级别：** 错误 (`error`)

**消息：**

```text
The hlsl block for '%s' must be a single expression: no statements, no local variables, no return. Move multi-statement logic into a .dfm DynamicInput and call it here.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:340`

**原因.** 栈输入的 `hlsl` 块会降成一个只有一个类型化输出引脚、没有函数体的节点，所以装不下语句。

**修复.** 折成一个表达式，或者把逻辑挪进 `.dfm` Module —— 它的 body 逐字输出。`.dfm` 生成的存在就是为了补上这个缺口。

<a id="dfx4031"></a>

## DFX4031

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is not an allowed inline function. Allowed: %s. Anything else belongs in a .dfm dynamic input or an hlsl { } block.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:158`

**原因.** L6 的白名单。放宽它就是滑向重新实现一个上万行的表达式后端，所以它故意很短。

**修复.** 用列出来的函数、一个 `.dfm` dynamic input，或者一个 `hlsl { }` 块 —— 那里面任何 HLSL 都合法。

<a id="dfx4032"></a>

## DFX4032

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' in the expression for '%s' is not a parameter. Only namespace-qualified parameters (User.X, Particles.X, Engine.X, ...) can be read from an expression.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:100`

**原因.** 内联表达式看得见参数，看不见局部变量。裸名字没有可绑定的东西。

**修复.** 加上命名空间，或者把表达式挪进有局部变量的 `hlsl { }` 块。

<a id="dfx4033"></a>

## DFX4033

**严重级别：** 错误 (`error`)

**消息：**

```text
Builtin '%s' takes positional arguments, not named ones.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:166`

**原因.** L6 的内置函数是位置参数。具名参数读起来像一次 dynamic input 调用，会静默地解析成别的东西。

**修复.** 写 `saturate(x)`，不是 `saturate(Value = x)`。

<a id="dfx4034"></a>

## DFX4034

**严重级别：** 错误 (`error`)

**消息：**

```text
Builtin '%s' takes %d argument(s), but %d were written.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:174`

**原因.** 参数个数是区分 `lerp(a, b)` 和 `lerp(a, b, t)` 的唯一线索。

**修复.** 数一下实参个数。

<a id="dfx4035"></a>

## DFX4035

**严重级别：** 错误 (`error`)

**消息：**

```text
Expression for '%s' contains a value that has no HLSL form.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:205`

**原因.** 表达式里有一部分没有 HLSL 形式 —— 比如算术里出现了曲线字面量或者数据接口。

**修复.** 把那一部分挪到它自己的输入上。

<a id="dfx4036"></a>

## DFX4036

**严重级别：** 错误 (`error`)

**消息：**

```text
The hlsl block for '%s' is empty.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:330`

**原因.** 空的 `hlsl` 块会降成一个里面没有文本的表达式。

**修复.** 把表达式写了，或者把块删了。

<a id="dfx4037"></a>

## DFX4037

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is %s; a curve { } literal only fits a curve data interface input.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:356`

**原因.** `curve { }` 字面量填的是曲线数据接口，而这个输入不是。

**修复.** 把曲线喂给真正接受它的那个输入 —— 通常是 `FloatFromCurve(FloatCurve = curve { ... })` 这类 dynamic input。

<a id="dfx4038"></a>

## DFX4038

**严重级别：** 错误 (`error`)

**消息：**

```text
The curve for '%s' has no keys.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:364`

**原因.** 空曲线求值出不来任何东西，运行时读起来就是一个卡在零的值。

**修复.** 加上键。

<a id="dfx4039"></a>

## DFX4039

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown curve interpolation '%s'. Expected Auto, Cubic, Linear or Constant.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:378`

**原因.** 曲线切线是数据；一个不认识的模式如果回退成 Auto，就会不吭声地丢掉一条手调过的形状。

**修复.** 用 `Auto`、`Cubic`、`Linear` 或 `Constant`。

<a id="dfx4040"></a>

## DFX4040

**严重级别：** 错误 (`error`)

**消息：**

```text
Parameter '%s': no asset at '%s'.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:601`

**原因.** 一个 `Object<T>` 参数拿到了一个资产路径，而那个路径上什么都没有。通常是源码写完之后资产被移动或改名了，
或者写的是 Content Browser 的显示名而不是对象路径。

**修复.** 用资产右键 **Copy Reference** 复制出来的路径，整条粘进去：
`Object<Texture> _Tex = "/Game/FX/T_Spark.T_Spark";`。点号后面那一半是包内的对象名，不能省。

<a id="dfx4041"></a>

## DFX4041

**严重级别：** 错误 (`error`)

**消息：**

```text
Parameter '%s': '%s' is a %s, which is not a %s.
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:610`

**原因.** 那个路径上的资产存在，但不是参数声明的那个类。`Object<Texture> = ".../M_Fire"` 指的是一个材质，
而 Niagara 会在**运行时**而不是构建时拒绝这次绑定。

**修复.** 要么指向声明类的资产，要么把声明放宽到你真正指的那个类 ——
`Object<Texture>`、`Object<StaticMesh>`、`Object<MaterialInterface>` 等等，与模块输入期待的一致。

<a id="dfx4042"></a>

## DFX4042

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown curve tangent mode '%s'. Expected Auto, User, Break or None.
```

**触发位置：** `Generation/DreamFXExpressions.cpp:388`

**原因.** 曲线键的 `Tangent=` 属性写了一个不属于 Unreal 四种切线模式的名字。这里拒绝而不是回退，
理由和 DFX4039 一样：回退成 `Auto` 会静默丢掉写在旁边的切线，而一条折角被磨圆了的曲线是另一条曲线。

**修复.** 用 `Auto`、`User`、`Break` 或 `None` —— 或者干脆不写 `Tangent`：
那样带切线的键视为 `User`，不带的视为 `Auto`，这也是这个属性存在之前所有源码的含义。

<a id="dfx4091"></a>

## DFX4091

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s': dynamic inputs, hlsl blocks, curves and inline expressions are not available yet (planned for Phase 3).
```

**触发位置：** `Generation/DreamFXValueLowering.cpp:663`

**原因.** 一个 Phase 2 的构建撞上了 Phase 3 的值形态。历史遗留 —— 在当前版本上不应该出现。

**修复.** 重新编译插件。还在出现，说明这份源码是被一个旧二进制编译的。
