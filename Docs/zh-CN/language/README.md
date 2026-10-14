---
title: 文件模型
description: 每个 DreamFXLang 文件的形状：头、块、语句、注释、属性，以及八条 L 规则的索引。
---

DreamFXLang 只有一种文件形状。三种扩展名的区别在于顶层声明的是什么，不在于语法。

```text
<Kind>(Name = <string>[, Root = <string>])
{
    <block> …
}
```

| 记号 | 含义 | 示例 |
| --- | --- | --- |
| `<x>` | 占位符——替换成实际内容，尖括号本身不写出来。 | `Name = <string>` |
| `[ x ]` | 可选——整段可以整体省略。 | `[, Root = <string>]` |
| `{ a \| b }` | 多选一——从竖线分隔的写法里取其中一个。 | `{ Node( … ) \| Comment( … ) }` |
| `…` | 可重复——前一项可以出现任意多次。 | `<property-declaration> …` |

| `Kind` | 扩展名 | 产物 |
| --- | --- | --- |
| `System` | `.dfs` | 一个 `UNiagaraSystem` |
| `Emitter` | `.dfe` | 无（由 `.dfs` 的 `from` 拷入） |
| `Module` / `DynamicInput` | `.dfm` | 一个 `UNiagaraScript` |

`Kind` 与扩展名不一致是 [DFX2021](../diagnostics/DFX2xxx.md#dfx2021)；一个文件只有一个顶层对象，
后面还有别的是 [DFX2022](../diagnostics/DFX2xxx.md#dfx2022)。`Root` 的取值见
[目录与命名](../getting-started/project-layout.md)。

<a id="块语句属性"></a>

## 块、语句、属性

```cpp
Settings = {                       // 块：Name = { … }
    WarmupTime = 0.0;              // 语句以 ; 结尾
}

Properties = {
    int SparkCount = 24 [ Group="Burst"; SortPriority=10 ];   // 属性挂在声明后面的方括号里
}

// 行注释
/* 块注释 */
```

- **块**是 `Name = { ... }`；
- **语句**以 `;` 结尾；
- **属性**是声明后面方括号里的 `Key=Value`，用 `;` 分隔；
- **注释**是 `//` 和 `/* */`。

<a id="region-只是注释"></a>

### `#Region` 只是注释

```cpp
ParticleUpdate = {
    #Region "Forces"
    GravityForce(Gravity = (0, 0, -680));
    #EndRegion
}
```

`#Region` / `#EndRegion` 供人和编辑器折叠用，**不会到达资产**（这是 L5）：外部编辑 API 没有给
栈加注记的函数。没闭合会警告（[DFX2010](../diagnostics/DFX2xxx.md#dfx2010)），
多出来的 `#EndRegion` 是错（[DFX2005](../diagnostics/DFX2xxx.md#dfx2005)）。

<a id="不是标识符的名字"></a>

## 不是标识符的名字

Niagara 的名字来自一个不做任何限制的 UI。真实内容里有叫 `PillarPower(0~1)` 的 user 参数，
也有叫 `Ring/DiscDistributionMode` 的模块输入 —— 它们在任何语言里都不是标识符。反引号包住一个：

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

反引号里的是**一个 token**，点号也包括在内 —— 写 `` `User.PillarPower(0~1)` ``，
不是 ``User.`PillarPower(0~1)` ``。只在需要时用：`` `Speed` `` 合法，但不如 `Speed` 好读，
反编译器就是照这条规则输出的。名字里带反引号的没有书写形式，只能改名。
没闭合是 [DFX1005](../diagnostics/DFX1xxx.md#dfx1005)。

<a id="八条-l-规则"></a>

## 八条 `L*` 规则

语言的骨架是八条规则，文档里按名字引用它们：

| | 规则 |
| --- | --- |
| **L1** | 栈是有序语句块。六个：`SystemSpawn`、`SystemUpdate`、`EmitterSpawn`、`EmitterUpdate`、`ParticleSpawn`、`ParticleUpdate`。**写的顺序就是模块顺序**。 |
| **L2** | 语句只有两种：模块调用和赋值。**连续的赋值折叠进同一个 Set Parameters 模块**，一次模块调用打断这一串。新属性由第一次写入声明。 |
| **L3** | 值有四种形态：字面量、linked、dynamic input、HLSL。 |
| **L4** | 模块名通过 `Settings.ModulePaths` 解析；只有短名有歧义时才写长路径。 |
| **L5** | `#Region` 是注释，不到达资产。 |
| **L6** | 内联表达式降到一条 HLSL 表达式，且只允许白名单里的函数。 |
| **L7** | 数值转换只加宽不收窄。`int -> float` 隐式；`float -> int` 要写 `int(...)`。 |
| **L8** | renderer 属性是 schema 驱动的通用赋值；每种 renderer 类型都被支持，没有逐类型语法。 |

L2、L3、L6、L7 的展开在[值与规则](values.md)，L1 在
[`.dfs`](dfs.md)，L8 在 [`.dfs` 的 renderer 一节](dfs.md#renderers)。

<a id="三种文件各自的页"></a>

## 三种文件各自的页

- [`.dfs` — 一个系统](dfs.md) — Settings、Properties、六个栈、renderer、`from`、`Defaults`。
  - [`.dfe` — 可复用 emitter](dfe.md) — 一个 `Emitter` 块，以及 `from` 的合并粒度。
  - [`.dfm` — 模块与动态输入](dfm.md) — `Usage`、`Inputs`、HLSL `Body`，以及哪些引擎能生成它。
  - [值、类型与 `L*` 规则](values.md) — 四种值形态、`curve { }`、内联表达式、类型表。
  - [事件与 Simulation Stage](events-and-stages.md) — `OnEvent` 与 `Stage`，以及它们各自的硬限制。

<a id="有意不在这里的东西"></a>

## 有意不在这里的东西

- **emitter 继承。** `from` 是拷贝不是继承。改 `.dfe` 不会反向影响已经拷过它的系统，直到重建。
- **`MaterialParam`。** 语法保留，未实现（[DFX5093](../diagnostics/DFX5xxx.md#dfx5093)）。
- **任何需要通用表达式编译器的东西。** 见 [L6](values.md#l6)。
