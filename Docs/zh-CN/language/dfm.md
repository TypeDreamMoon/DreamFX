---
title: .dfm — 模块与动态输入
description: 用文本写一个 Niagara 模块或动态输入：Usage、Inputs、HLSL Body，以及哪些引擎能生成它。
---

一个 `.dfm` 产生一个 `UNiagaraScript`。两种，差别只有两行：

```cpp
// 模块：放进栈里，读写属性。
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
{
    Settings = {
        Usage       = ParticleUpdate;
        Category    = "MoonToon|Motion";
        Description = "以恒定速率旋转 sprite，可反向。";
    }

    Inputs = {
        float SpinRate   = 90.0  [ Description="度/秒。" ];
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
// 动态输入：给一个输入槽算一个值。
DynamicInput(Name="Modules/Moon/ToonPulse", Root="Plugin.DreamFX")
{
    Settings = { Usage = DynamicInput; Output = float; Category = "MoonToon|Math"; }
    Inputs   = { float Frequency = 6.0; float Sharpness = 2.0; }
    Body     = {
        return pow(0.5 + 0.5 * sin(Engine.Time * Frequency * 6.2831853), Sharpness);
    }
}
```

在 `.dfs` 里的用法和别的模块没有任何区别：

```cpp
ToonSpin(SpinRate = 220.0, bClockwise = true);
ScaleSpriteSize(ScaleSpriteSizeMode = Uniform, UniformScaleFactor = ToonPulse(Frequency = 4.0));
```

<a id="settings"></a>

## `Settings`

| 键 | 含义 |
| --- | --- |
| `Usage` | 模块可以被放进哪个（哪些）栈，或者 `DynamicInput`。取六个栈名之一，或一个数组：`Usage = [ParticleSpawn, ParticleUpdate];` |
| `Output` | `DynamicInput` 的返回类型。必填（[DFX3031](../diagnostics/DFX3xxx.md#dfx3031)）；必须是值类型，不能是数据接口（[DFX3039](../diagnostics/DFX3xxx.md#dfx3039)） |
| `Category` | 它在栈的 add 菜单里的位置 |
| `Description` | 模块的 tooltip |

没写显式栈列表的 `DynamicInput` 默认落在 particle spawn 与 update —— 这两个覆盖了绝大多数情况。
要更宽就用数组形式。

<a id="inputs"></a>

## `Inputs`

每一条变成一个带默认值和描述的 `Module.<Name>` 输入。默认值必须是字面量或枚举项：
模块输入的默认值存在资产上，引用不到模块外面的任何东西
（[DFX3044](../diagnostics/DFX3xxx.md#dfx3044)）。

属性：`[ Description="..." ]`、`[ Advanced ]`、`[ StaticSwitch ]`。

`[StaticSwitch]` 会被接受、被校验（bool / int / 枚举，常量默认值 —— DFX3034 / DFX3035），
然后**按普通输入 lower**，并由 [DFX5102](../diagnostics/DFX5xxx.md#dfx5102) 说明这件事。
一级生成把整个 body 放进一个 custom HLSL 节点，那里没有给开关选择的分支。body 读它的方式完全一样，
丢掉的只是编译期折叠。

<a id="body"></a>

## `Body`

body 是 HLSL，外加 DreamFX 的命名空间规则。

**它自己的输入写裸名。** 写 `SpinRate`，不是 `Module.SpinRate` —— 在模块内部这个命名空间是隐含的。
带前缀的写法会被接受并归一化掉，因为它对语言的理解没错；但裸名才是对正在发生的事情的描述：
这个输入是以一个引脚的形式到达的。

**`Engine.`、`User.`、`System.`、`Emitter.` 照写照读**，它们直接对参数映射解析。

**`Particles.*` 照写照读写**，引脚由 DreamFX 接：

```cpp
Body = {
    Particles.SpriteRotation += SpinRate * Engine.DeltaTime;   // 读与写都接好了
}
```

Niagara 已知的属性（`Particles.SpriteRotation`、`Particles.Color`…）什么都不用做。
自定义的属性要在**第一次使用时**写上类型，和 `.dfs` 声明新属性一样 —— 否则引脚会按猜出来的宽度接
（[DFX3046](../diagnostics/DFX3xxx.md#dfx3046)）：

```cpp
Body = {
    float Particles.Moon.SpinPhase = 0.0;
    Particles.Moon.SpinPhase += Engine.DeltaTime;
}
```

`Particles.Color.rgb` 解析成 `Particles.Color` 加一个 swizzle：最长的、是已知或已声明属性的
点号前缀胜出。

<a id="调用数据接口输入的函数"></a>

### 调用数据接口输入的函数

声明成 `DI<X> Name;` 的输入是 custom 节点上的一个引脚，函数按 Niagara custom HLSL 的写法调用：
`Name.Function(args)`。`out` 实参必须声明成**未初始化的普通局部变量**：

```cpp
Inputs = { DI<DreamWind> Wind; }
Body = {
    float3 WindVelocity;
    float  Gust;
    Wind.SampleWind(Particles.Position, WindVelocity, Gust);
    Particles.Velocity = WindVelocity;
}
```

**警告：**

调用前写 `float Gust = 1.0;` 会让 CPU VM 编译失败：
  `internal compiler error: out/inout parameters must be lvalues in call to 'SampleWind_Module_Wind'`
  （[DFX6006](../diagnostics/DFX6xxx.md#dfx6006)）。VM 的 HLSL 前端 hlslcc 会先把带初值的局部常量
  折进实参表，再去检查 out 形参 —— 而一个常量不是 lvalue。GPU 模拟两种写法都接受；
  **按未初始化写，一份 body 两端都通。**

<a id="模块吃语句动态输入吃一个表达式"></a>

### 模块吃语句，动态输入吃一个表达式

模块的 body 逐字输出：局部变量、分支、想写几条语句都行。**这正是
[DFX4030](../diagnostics/DFX4xxx.md#dfx4030) 一直在指的那件能力**，也是把 `.dfm` 生成解锁掉值得做的
理由 —— `.dfs` 里的内联 `hlsl { }` 是一个 rvalue，永远装不下更多。

动态输入的 body 被 Niagara 翻译器包成 `Output = (Type)( <body> );`，所以它必须是**一个表达式**，
写不写 `return` 都行。前面放语句只会产出无效 HLSL 而不是一条说清问题的错误，
所以 DreamFX 先接住它（[DFX3037](../diagnostics/DFX3xxx.md#dfx3037)），并指向模块形式。

<a id="backends"></a>

## 哪些引擎能生成 `.dfm`

把 HLSL 写到 Niagara custom 节点上需要 `UNiagaraNodeCustomHlsl::SetCustomHlsl`，
围着它建图还需要另外四个声明。MoonEngine 给这五个都打了导出宏，预编译的 stock 引擎一个都没有。
`DreamFXEditor.Build.cs` 探测引擎头文件，据此定义 `DREAMFX_HAS_CUSTOMHLSL_WRITE`。

但故事没在这里结束，因为**导出**和**可达**不是一回事：公有数据成员不需要导出宏，
公有虚函数走虚表，而作为 `UPROPERTY` 的私有字段可以按名字写。这五个声明背后的每一个行为
最后都被证明可以那样够到，于是有两个后端、三种结果：

| | 什么时候 | 行为 |
| --- | --- | --- |
| **direct** | 引擎导出了那五个声明 | 直接调它们；MoonEngine 走这条 |
| **reflection** | 没导出，但它依赖的形状都对得上 | 从公有面重建每一个操作 |
| **degraded** | 它依赖的某个形状动了 | 拒绝生成，并说清是哪一个 |

反射后端不是「假定等价」，是**对着直接后端验过**的：同一个 `.dfm` 两种方式建出来，
读回的 schema 逐字节相同，而且跨引擎成立 —— 在 stock UE 5.8 上生成的模块，
与在 MoonEngine 上生成的同一个模块相同。

`-DreamFXForceReflectionBackend`（或 `dfx.ps1 -ForceReflectionBackend`）在不需要它的引擎上
强制走反射后端，这就是两条路在一台机器上互相 diff 的方法。

**提示：**

**受限的从来不是产物。** 生成出来的模块是一个普通的 `UNiagaraScript`：任何引擎加载它、
  从 `.dfs` 引用它、cook 它、跑它。*在能生成的地方生成，提交资产，所有人消费* ——
  队伍里有 MoonEngine 的话，在那边生成仍是推荐路径，理由很简单：整个语料是照着它验的。

两个后端都跑不动的时候：

| 情况 | 发生什么 |
| --- | --- |
| 资产已提交且与源码匹配 | 构建跳过它，CI 保持绿 |
| 源码改了但没重新生成 | **[DFX5107](../diagnostics/DFX5xxx.md#dfx5107)** —— 去能跑后端的地方重建 |
| 根本没有资产 | **[DFX5100](../diagnostics/DFX5xxx.md#dfx5100)** —— 点名是哪一项检查没过 |

溯源检查两边都跑，区别只在补救办法。

<a id="一级生成的代价"></a>

## 一级生成的代价

整个 body 变成一个 `UNiagaraNodeCustomHlsl`，所以在节点编辑器里这个模块是个黑盒。
对一个文本优先的工作流来说这就是本意 —— 文本才是源。它比内联 `hlsl { }` 多买到的是
栈输入装不下的一切：语句、带默认值和 tooltip 的具名输入、跨系统按名复用。

把 body 铺成真正的节点图是二级，且不在计划内：那是 DreamShader 已经背着的那个约 1.3 万行的问题，
再来一遍。
