---
title: 值、类型与规则
description: 等号右边能出现的一切：四种值形态、curve 字面量、内联表达式白名单、类型对照表，以及 L2 / L3 / L6 / L7。
---

<a id="l3"></a>

## L3 —— 四种值形态

```cpp
InitializeParticle(
    Lifetime   = 0.25,                                       // 字面量
    Color      = User.TintA,                                 // linked
    SpriteSize = RandomRangeFloat(Minimum = 1, Maximum = 4)  // dynamic input
);

Color Particles.Color = hlsl {                               // HLSL 表达式
    float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge))
};
```

<a id="字面量"></a>

### 字面量

```cpp
450.0        24           true
(1, 0, 0)    (1.0, 0.72, 0.25, 1.0)      // 2、3 或 4 个分量
"/Game/FX/M_Spark"                        // 资产路径
box(-200, -200, -50, 200, 200, 300)       // 六个数：min xyz，max xyz
```

`24` 和 `24.0` 是两个东西：前者是整数字面量，L7 靠这个区分。

枚举项按名字写，空格和连字符去掉 —— Niagara 的 `Direct Set` 写成 `DirectSet`。名字是从活资产上
读的，因为用户自定义的 Niagara 枚举内部存的是 `NewEnumerator0`，真名藏在显示文本里；
写错会报出真实列表（[DFX4006](../diagnostics/DFX4xxx.md#dfx4006)）。

不是标识符的名字用反引号包住，见[文件模型](README.md)。

<a id="linked"></a>

### Linked

```cpp
SpawnCount = User.SparkCount;
ConeAxis   = Particles.Velocity;
```

任何带命名空间的参数：`User.`、`Particles.`、`Emitter.`、`System.`、`Engine.`。

链接是**直接绑定**参数 —— 中间没有任何转换步骤。所以两边的类型必须严格一致，而
[DFX4027](../diagnostics/DFX4xxx.md#dfx4027) 是 L7 里唯一一个显式 cast 也救不了的情况：
没有地方可以放那个 cast。一个 `float` 的 user 参数驱动不了 `int32` 的 spawn count，
把它声明成 `int`。

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

**只能是一个表达式**（[DFX4030](../diagnostics/DFX4xxx.md#dfx4030)）。栈输入上的 HLSL 会降成一个
只有一个类型化输出引脚、没有函数体的节点，语句无处可去。多条语句的逻辑属于
[`.dfm` 模块](dfm.md)，那里的 body 逐字输出。

`hlsl` 块自身不带类型，所以赋给一个新属性时要写类型标注：
`Color Particles.Color = hlsl { ... };`（[DFX4022](../diagnostics/DFX4xxx.md#dfx4022)）。

<a id="curve"></a>

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
dynamic input（[DFX4037](../diagnostics/DFX4xxx.md#dfx4037)）。

**切线是逐键的，也逐键导出。** 它们很重要：一条手调过的曲线，写回去时丢了切线就是另一条曲线，
丢形状就是丢数据。`Interp` 默认 `Auto`。

`Tangent` 是切线模式 —— `Auto`、`User`、`Break`、`None` —— 可以省略：带切线的键视为 `User`，
不带的视为 `Auto`。真正必须写出来的只有 `Break`、`None`，以及带着存储斜率的 `Auto` 键，
所以手写源码几乎不会提到它。

**警告：**

**`Break` 是那个非写不可的。** 它是一个键怎么持有两条互相独立的切线 —— 一个折角。
  2026-08-12 之前，导出器只在 `User` 模式下写切线，其余全丢，于是每一个折角回来都变圆了，
  每一个 Auto 键存着的斜率回来都成了零。曲线的**求值**永远读存储的切线，与模式无关；
  引擎只在有东西编辑这条曲线时才重新推导它 —— 所以「模式是 Auto，引擎会自己算」
  对一条没人编辑的曲线是假的。

一个曲线数据接口如果在键之外还设了别的东西 —— 暴露曲线、关掉 LUT、外部曲线资产 —— 会写成原样的
配置 JSON 而不是 `curve { }` 字面量；多通道接口（向量、颜色）只在各通道形状一致时才取可读形式。
可读形式是一种便利，**而便利没有资格丢数据**。

<a id="l6"></a>

## L6 —— 内联表达式

```cpp
VelocityStrength = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed)
StretchDir       = normalize(Particles.Velocity)
```

算术（`+ - * /`）、一元负号、括号，以及这些函数：

```text
normalize  saturate  clamp  lerp  frac  min  max
abs  floor  ceil  pow  sqrt  dot  cross  length
```

整个表达式降成一条 HLSL 表达式 dynamic input，类型由操作数推出。表达式内部只存在带命名空间的参数，
**没有局部变量**（[DFX4032](../diagnostics/DFX4xxx.md#dfx4032)）。内置函数是位置参数，不具名
（[DFX4033](../diagnostics/DFX4xxx.md#dfx4033)）。

**列表之外的一切都是错误**（[DFX4031](../diagnostics/DFX4xxx.md#dfx4031)），而这张表短是故意的：
放宽它的第一步就是走向重新实现一个通用表达式编译器 —— 那正是 DreamShader 里那个约 1.3 万行的组件，
DreamFX 的存在有一部分就是为了避开它。`hlsl { }` 是逃生舱：那里面任何 HLSL 都合法。

**表达式不会被反编译器还原** —— 它们会以等价的 `hlsl { }` 块回来。往返是语义上的，不是文本上的。

<a id="l7"></a>

## L7 —— 数值转换

| 从 → 到 | |
| --- | --- |
| `int` → `float` | 隐式 |
| `float` → `int` | 错误；写 `int(...)`（[DFX4003](../diagnostics/DFX4xxx.md#dfx4003)） |
| 向量宽度不匹配 | 错误（[DFX4002](../diagnostics/DFX4xxx.md#dfx4002)） |

一个被静默截断的 spawn count 属于最难找的那类特效 bug，这就是这条规则单向的全部理由。

<a id="l2"></a>

## L2 —— 两种语句

```cpp
ParticleUpdate = {
    GravityForce(Gravity = (0, 0, -680));      // 模块调用
    Particles.Moon.Seed = 0.5;                 // 赋值 ┐
    Particles.Moon.Tint = User.TintA;          //      ┘ 折成一个 Set Parameters 模块
    SolveForcesAndVelocity();                  // 调用打断这一串
}
```

新属性由第一次写入声明。展开在 [`.dfs`](dfs.md)。

<a id="类型对照"></a>

## 类型对照

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
[DFX5098](../diagnostics/DFX5xxx.md#dfx5098) 的意思是**值的形状不对**，不再是「只声明、会被忽略」。
