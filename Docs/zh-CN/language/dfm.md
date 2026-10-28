---
title: "`.dfm` — 模块与动态输入"
description: "`.dfm` — 模块或动态输入"
---

<a id="dfm--a-module-or-a-dynamic-input"></a>

# `.dfm` — 模块或动态输入

生成一个 `UNiagaraScript`。两种，差别在两行：

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

从 `.dfs` 里像其他任何东西一样使用：

```cpp
ToonSpin(SpinRate = 220.0, bClockwise = true);
ScaleSpriteSize(ScaleSpriteSizeMode = Uniform, UniformScaleFactor = ToonPulse(Frequency = 4.0));
```

---

<a id="which-engines-can-generate-one"></a>

## 哪些引擎能生成

把 HLSL 写到 Niagara 自定义节点上需要 `UNiagaraNodeCustomHlsl::SetCustomHlsl`，而围绕它构建图表还需要另外四个声明。MoonEngine 给这五个都加了导出宏；原版引擎一个都不导出。`DreamFXEditor.Build.cs` 探测引擎头文件并据此定义 `DREAMFX_HAS_CUSTOMHLSL_WRITE`。

故事并未就此结束，因为*导出*和*可达*不是一回事。公有数据成员不需要导出宏，公有虚函数通过虚表分发，而作为 `UPROPERTY` 的私有字段可以按名称写入。这五个声明背后的每一个行为最终都被证明可以这样够到，因此有两个后端和三种结果：

| | 何时 | 行为 |
| --- | --- | --- |
| **direct** | 引擎导出这五个声明 | 调用它们；这就是 MoonEngine |
| **reflection** | 没有导出，但它依赖的形状都能对上 | 从公有表面重建每个操作 |
| **degraded** | 它依赖的某个形状发生了变化 | 拒绝生成，并说明是哪一个 |

反射后端是与直接后端对照验证的，而不是假定等价：同一个 `.dfm` 用两种方式构建，读回的 schema 逐字节相同，而且这在跨引擎时也成立——在原版 UE 5.8 上生成的模块与在 MoonEngine 上生成的同一模块相匹配。

`-DreamFXForceReflectionBackend`（或 `dfx.ps1 -ForceReflectionBackend`）在不需要它的引擎上选择反射后端，这就是在同一台机器上对两者进行 diff 的方式。

**产物从来不是受限的部分。** 生成的模块是一个普通的 `UNiagaraScript`：任何引擎都能加载它、从 `.dfs` 引用它、cook 它并运行它。*在能运行的地方生成，提交资产，所有人消费它*——而在有 MoonEngine 的团队里，在那里生成仍然是推荐路径，只因为整个语料库都是照着它验证的。

当两个后端都无法运行时：

| 情况 | 会发生什么 |
| --- | --- |
| 资产已提交且与源文件匹配 | 构建跳过它，CI 保持绿 |
| 源文件已编辑但未重新生成 | **DFX5107** —— 在后端能运行的地方重新生成 |
| 完全没有资产 | **DFX5100** —— 指出是哪项检查失败了 |

溯源检查两种情况下都会运行；区别只在于补救办法。

---

<a id="settings"></a>

## `Settings`

| 键 | 含义 |
| --- | --- |
| `Usage` | 模块可以被放入哪个（哪些）栈，或 `DynamicInput`。取六个栈名之一（L1），或它们的数组：`Usage = [ParticleSpawn, ParticleUpdate];` |
| `Output` | `DynamicInput` 的返回类型。必填（DFX3031）；是值类型，不是数据接口（DFX3039） |
| `Category` | 它在栈的添加菜单中的位置 |
| `Description` | 模块的 tooltip |

没有显式栈列表的 `DynamicInput` 默认为粒子生成和更新——这两个覆盖了几乎所有情况。用数组形式来拓宽它。

<a id="inputs"></a>

## `Inputs`

每一条都会变成一个带声明默认值和描述的 `Module.<Name>` 输入。默认值必须是字面量或枚举项，因为模块输入的默认值存储在资产上，不能引用模块之外的任何东西（DFX3044）。

普通的整数输入可以用 `int(1.9)` 作为默认值：它会在存储前被求值为 `1`。参数必须是一个有限数值字面量，其截断值能放入 `int32`（DFX4044）。对于 `[StaticSwitch]` 默认值，直接写出结果字面量。

数据接口（`DI<X>` 以及诸如 `Texture2D` 的简写）和仅实例对象类型（标记为 `DefaultToInstanced` 的 actor、组件和类）不能声明输入默认值（DFX4043）。声明输入时不带 `= ...`，并从系统或栈输入提供它的实例。这些值不是资产路径。`Object<Texture>` 和其他资产引用类型仍然与数据接口不同；普通资产引用在系统参数和栈输入上是支持的。

属性：`[ Description="..." ]`、`[ Advanced ]`、`[ StaticSwitch ]`。

`[StaticSwitch]` 会被接受、校验（bool/int/enum，常量默认值——DFX3034/DFX3035），然后**按普通输入 lower**，并由 DFX5102 说明这一点。一级生成把整个 body 放入一个自定义 HLSL 节点，那里没有供 switch 选择的分支。body 读取它的方式相同；丢失的是编译期折叠。

<a id="body"></a>

## `Body`

body 是 HLSL，上面加了 DreamFX 的命名空间。

**它自己的输入是裸名称。** `SpinRate`，而不是 `Module.SpinRate` —— 在模块内部命名空间是隐含的。限定形式会被接受并归一化掉，因为它对语言的描述并没有错，但裸名才是描述实际发生情况的写法：输入以引脚的形式到达。

**`Engine.`、`User.`、`System.` 和 `Emitter.` 照写照读。** 它们直接对参数映射解析。

**`Particles.*` 照写照读写**，DreamFX 为它连接引脚：

```cpp
Body = {
    Particles.SpriteRotation += SpinRate * Engine.DeltaTime;   // read and write, both handled
}
```

Niagara 已经知道的属性（`Particles.SpriteRotation`、`Particles.Color`……）不需要任何东西。自定义属性需要在首次使用时声明其类型，就像 `.dfs` 声明新属性一样——否则引脚会按猜测的宽度连接（DFX3046）：

```cpp
Body = {
    float Particles.Moon.SpinPhase = 0.0;
    Particles.Moon.SpinPhase += Engine.DeltaTime;
}
```

`Particles.Color.rgb` 通过 swizzle 解析为 `Particles.Color`：最长的、已知或已声明的点号前缀胜出。

整值赋值、swizzle 和索引分量支持 `=`、算术复合赋值（`+=`、`-=`、`*=`、`/=`、`%=`）、整数复合赋值（`&=`、`|=`、`^=`、`<<=`、`>>=`），以及前缀/后缀 `++` 和 `--`。诸如 `==` 和 `<=` 的比较仍是读取。普通 HLSL 操作数类型规则仍然适用。注释可以分隔声明、属性和运算符。原版 CPU VM 编译器拒绝整数取余（`int %= int`）；在合适的地方使用浮点取余。不支持的操作会报告 Niagara 编译错误，而不是丢弃其写入。

对已有属性的条件写入在分支不执行时保留其传入值。新的自定义属性必须先接收一个无条件的整值初始化器，然后才能有分支、循环、预处理器条件或可能的提前返回（DFX3057）；后续的读取和分量写入使用该已初始化值，而不需要传入属性。要使用由较早模块提供的自定义属性，只需在条件更新前声明其类型：

```cpp
float Particles.Moon.Existing;
if (bEnabled) { Particles.Moon.Existing = 1.0; }
```

生成的 HLSL 引脚名称是内部的：诸如 `Particles.Moon.Spin` 和 `Particles.Moon_Spin` 这样的不同名称保持不同，包括当输入或局部变量使用类似名称时。

<a id="calling-a-data-interface-input"></a>

### 调用数据接口输入

声明为 `DI<X> Name;` 的输入是自定义节点上的一个引脚，其函数按 Niagara 自定义 HLSL 的方式调用：`Name.Function(args)`。将 `out` 参数声明为**普通的、未初始化的局部变量**：

```cpp
Inputs = { DI<DreamWind> Wind; }
Body = {
    float3 WindVelocity;
    float  Gust;
    Wind.SampleWind(Particles.Position, WindVelocity, Gust);
    Particles.Velocity = WindVelocity;
}
```

`float Gust = 1.0;` 会在调用前让 CPU VM 编译失败，报 `internal compiler error: out/inout
parameters must be lvalues in call to 'SampleWind_Module_Wind'`（DFX6006）：VM 的 HLSL 前端
hlslcc 会先把带初值的局部常量折进实参表，再去检查 out 形参，而常量不是 lvalue。GPU 模拟两种写法都接受；
按未初始化写，一份 body 两端都通。

<a id="modules-take-statements-dynamic-inputs-take-one-expression"></a>

### 模块吃语句，动态输入吃一个表达式

模块的 body 逐字输出 —— 局部变量、分支、想写几条语句都行。**这正是
DFX4030 一直在指的那件能力**，也是把 `.dfm` 生成解锁掉值得做的
理由：`.dfs` 里的内联 `hlsl { }` 是一个 rvalue，永远装不下更多。

动态输入的 body 被 Niagara 翻译器包成 `Output = (Type)( <body> );`，所以它必须是**一个表达式**，
写不写 `return` 都行。前面放语句只会产出无效 HLSL 而不是一条说清真正问题的错误，
所以 DreamFX 先接住它（DFX3037），并指向模块形式。

---

<a id="what-tier-one-costs"></a>

## 一级生成的代价

整个 body 变成一个 `UNiagaraNodeCustomHlsl`，所以在节点编辑器里这个模块是个黑盒
（plan 3.3）。对一个文本优先的工作流来说这就是本意 —— 文本才是源。它比内联 `hlsl { }`
多买到的是栈输入装不下的一切：语句、带默认值和 tooltip 的具名输入、跨系统按名复用。

把 body 铺成真正的节点图是二级，且不在计划内：那是 DreamShader 已经背着的那个约 1.3 万行的问题，
再来一遍。
