---
title: "值、类型与 `L*` 规则"
description: "值、类型与 `L*` 规则"
---

<a id="values-types-and-the-l-rules"></a>

# 值、类型与 `L*` 规则

所有可以出现在 `=` 右边的东西，以及决定它含义的规则。

---

<a id="the-eight-rules-by-name"></a>

## 八条规则，按名称

| | 规则 |
| --- | --- |
| **L1** | 一个栈就是一个有序语句块。共有六种：`SystemSpawn`、`SystemUpdate`、`EmitterSpawn`、`EmitterUpdate`、`ParticleSpawn`、`ParticleUpdate`。书写顺序即模块顺序。 |
| **L2** | 两种语句形式：模块调用，以及赋值。**启用状态相同的连续赋值会折成一个 Set Parameters 模块**；一次模块调用或 `disabled` 状态的变化都会打断这一串。新属性由它的第一次写入声明。 |
| **L3** | 四种值模式：字面量、linked、dynamic input、HLSL。 |
| **L4** | 模块名通过 `Settings.ModulePaths` 解析；只有当短名有歧义时才写更长的路径。 |
| **L5** | `#Region` 是注释。它不会进入资产 —— 外部编辑 API 没有栈注释函数。 |
| **L6** | 内联表达式降成一条 HLSL 表达式，并且只允许白名单里的函数。 |
| **L7** | 数值转换只放宽，从不变窄。`int -> float` 是隐式的；`float -> int` 需要 `int(...)`。 |
| **L8** | 渲染器属性是 schema 驱动的通用赋值；每种渲染器类型都被支持，无需按类型专门写语法。 |

---

<a id="l3--the-four-value-modes"></a>

## L3 —— 四种值模式

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

<a id="literals"></a>

### 字面量

```cpp
450.0        24           true
(1, 0, 0)    (1.0, 0.72, 0.25, 1.0)      // 2, 3 or 4 components
"/Game/FX/M_Spark"                        // asset path
box(-200, -200, -50, 200, 200, 300)       // six numbers: min xyz, max xyz
```

`24` 和 `24.0` 是两个东西：前者是整数字面量，L7 靠这个区分。

<a id="names-that-are-not-identifiers"></a>

### 不是标识符的名字

Niagara 的名字来自一个没有任何限制的 UI。这个项目里的真实内容有一个叫 `PillarPower(0~1)` 的用户参数，
以及一个叫 `Ring/DiscDistributionMode` 的模块输入；两者在任何语言里都不是标识符。反引号可以包住其中一个：

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

反引号包住的名字是**一个 token**，点也算在内 —— 是 `` `User.PillarPower(0~1)` ``，不是
``User.`PillarPower(0~1)` ``。任何可以出现名字的地方，它都可以出现。

只给需要它的名字加引号：`` `Speed` `` 合法，但读起来比 `Speed` 差。反编译器正是按这条规则
处理的，所以一份导出会显示某个资产里哪些名字需要加引号。名字里含反引号的完全没有书写形式 —— 请重命名该参数。
反引号未闭合会报 [DFX1005](../diagnostics/DFX1xxx.md#dfx1005)。

枚举项按名字写，去掉空格和连字符 —— Niagara 的 `Direct Set` 写成
`DirectSet`。名字是从活资产上读的，因为用户自定义的 Niagara 枚举内部存的是
`NewEnumerator0`，真名留在显示文本里；写错会报出真实列表（DFX4006）。

<a id="linked"></a>

### Linked

```cpp
SpawnCount = User.SparkCount;
ConeAxis   = Particles.Velocity;
```

任何带命名空间的参数：`User.`、`Particles.`、`Emitter.`、`System.`、`Engine.`。

链接是**直接绑定**参数 —— 中间没有任何转换步骤。所以两边的类型必须严格一致，而 DFX4027 是 L7 里唯一一个
显式 cast 也救不了的情况：没有地方可以放那个 cast。一个 `float` 的 user 参数驱动不了 `int32` 的
spawn count；把它声明成 `int`。

<a id="dynamic-input"></a>

### Dynamic input

```cpp
UniformScaleFactor = FloatFromCurve(
    FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
    CurveIndex = Particles.NormalizedAge
)
```

可以嵌套到任意深度 —— 一条链只是一个更长的地址，从父到子书写。

<a id="hlsl"></a>

### HLSL

```cpp
Color Particles.Color = hlsl {
    float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge))
};
```

原样多行，不需要转义。带命名空间的参数照写照读。

**只能是一个表达式**（DFX4030）。栈输入上的 HLSL 会降成一个只有一个类型化输出引脚、
没有函数体的节点，所以语句无处可去。多条语句的逻辑属于
[`.dfm` 模块](dfm.md)，那里的 body 逐字输出。

`hlsl` 块自身不带类型，所以赋给一个新属性时要写类型标注：
`Color Particles.Color = hlsl { ... };`（DFX4022）。

---

<a id="curve--"></a>

## `curve { }`

```cpp
curve {
    0.0 -> 1.0;
    0.7 -> 0.85 [ Interp=Cubic; Arrive=-0.4; Leave=-1.2 ];
    0.9 -> 0.40 [ Interp=Cubic; Tangent=Break; Arrive=0.0; Leave=-2.25 ];
    1.0 -> 0.0  [ Interp=Linear ];
}
```

它填的是一个曲线数据接口，所以只能出现在期待曲线接口的位置 —— 通常是 `FloatFromCurve` 这类
dynamic input（DFX4037）。

切线是逐键的，也逐键导出。它们很重要：一条手调过的曲线，写回去时丢了切线就是另一条曲线，
丢形状就是丢数据。`Interp` 默认 `Auto`。

`Tangent` 是切线模式 —— `Auto`、`User`、`Break` 或 `None` —— 可以省略，
此时带切线的键视为 `User`，不带的视为 `Auto`。真正必须写出来的只有 `Break` 和 `None`，
再加上带着存储斜率的 `Auto` 键，所以普通的手写源码几乎不会提到它。**`Break` 是那个非写不可的**：
它是一个键怎么持有两条互相独立的切线 —— 一个折角 —— 而在 2026-08-12 之前，导出器只在 `User` 模式下写切线，
其余全丢，于是每一个折角回来都变圆了，每一个 Auto 键存着的斜率回来都成了零。曲线的**求值**永远读存储的切线，
与模式无关；引擎只在有东西编辑这条曲线时才重新推导它 —— 所以「模式是 Auto，引擎会自己算」
对一条没人编辑的曲线是假的。

一个曲线数据接口如果在键之外还设了别的东西 —— 暴露曲线、关掉 LUT、外部曲线资产 —— 会写成原样的
配置 JSON 而不是 `curve { }` 字面量；多通道接口（向量、颜色）只在各通道形状一致时才取可读形式。
可读形式是一种便利，而便利没有资格丢数据。

---

<a id="l6--inline-expressions"></a>

## L6 —— 内联表达式

```cpp
VelocityStrength = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed)
StretchDir       = normalize(Particles.Velocity)
```

算术（`+ - * /`）、一元负号、括号，以及这些函数：

```
normalize  saturate  clamp  lerp  frac  min  max
abs  floor  ceil  pow  sqrt  dot  cross  length
int
```

整个表达式降成一条 HLSL 表达式 dynamic input，类型由操作数推出。表达式内部只存在带命名空间的参数 —— 没有局部变量（DFX4032）。
内置函数是位置参数，不具名（DFX4033）。

`int(expression)` 显式把一个标量值转换成有符号 32 位整数，向零截断。例如，`int(User.CountScale * 3.0)` 仍然是一次运行时 HLSL 转换。
浮点常量在表达式降级过程中保留其数值精度，包括 `1e-7` 这样的小系数。

**列表之外的一切都是错误**（DFX4031），而这张表短是故意的。放宽它的第一步就是走向重新实现一个通用表达式编译器 ——
那正是 DreamShader 里那个约 1.3 万行的组件，DreamFX 的存在有一部分就是为了避开它。`hlsl { }` 是逃生舱：
在那里面，任何 HLSL 都合法。

**表达式不会被反编译器还原** —— 它们会以等价的 `hlsl { }` 块回来。往返是语义上的，不是文本上的。

---

<a id="l7--numeric-conversion"></a>

## L7 —— 数值转换

| 从 → 到 | |
| --- | --- |
| `int` → `float` | 隐式 |
| `float` → `int` | 错误；写 `int(...)`（DFX4003） |
| 向量宽度不匹配 | 错误（DFX4002） |

一个被静默截断的 spawn count 属于最难找的那类特效 bug，这就是这条规则单向的全部理由。

`int(1.9)` 也适用于普通参数和模块输入的默认值，产生字面量 `1`。
默认实参必须是一个有限数值字面量，其截断后的值必须能放入 `int32`
（DFX4044）。像 `int(User.Scale)` 这样的运行时表达式不能存为字面量默认值；
应把它放进栈赋值或模块输入调用中。`[StaticSwitch]` 默认值则要求直接给出
结果字面量。

---

<a id="types"></a>

## 类型

| DSL | Niagara |
| --- | --- |
| `float` `int` `bool` | `NiagaraFloat` / `NiagaraInt32` / `NiagaraBool` |
| `Vector2` `Vector` `Vector4` | 2 / 3 / 4 个 float |
| `Color` | `LinearColor` |
| `Position` | `NiagaraPosition` |
| `Quat` | `Quat` |
| `Texture2D`、`DI<X>` | 数据接口 |

数据接口参数以带引号的 JSON 对象承载配置 —— 和模块的数据接口输入用的是同一种逐字形式，
也是导出器写出来的那种形式。裸声明的保持为「留给运行时填的槽」。`curve { }` 是曲线接口的可读写法。
DFX5098 的意思是值的形状不对 —— 不再是「只声明、会被忽略」。
