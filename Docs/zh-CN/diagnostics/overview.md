---
title: 诊断码
description: 全部 DFXnnnn 诊断码：怎么读一条诊断、八个族各自属于流水线的哪一段，以及按码速查。
---

DreamFX 的每一条诊断都是一个 `DFXnnnn`，带着**文件、行、列**：

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

| | |
| --- | --- |
| 日志分类 | `LogDreamFX` |
| 严重级别 | `error` / `warning` / `info` —— 三级都真实使用 |
| 位置 | 每个码都带文件、行、列 |
| 编辑器里 | 失败的构建弹一条带 **Open in VSCode** 链接的 toast，直接跳到那一行那一列 |

**首位数字就是抛出它的阶段。** 这既是一条阅读线索，也是排错时该往哪里看的指路牌：

| 段 | 阶段 | 意味着 |
| --- | --- | --- |
| [`DFX1xxx`](DFX1xxx.md) | 驱动与文件 I/O | 文件读不了，或者词法层面就断了 |
| [`DFX2xxx`](DFX2xxx.md) | 词法与语法 | 语法不对：括号、分号、关键字 |
| [`DFX3xxx`](DFX3xxx.md) | 声明与文档结构 | 语法对了，但声明的东西对不上真实资产 |
| [`DFX4xxx`](DFX4xxx.md) | 值、类型与表达式 | 等号右边有问题 |
| [`DFX5xxx`](DFX5xxx.md) | 生成与资产写入 | 写资产这一步出了问题，或者有意的降级提示 |
| [`DFX6xxx`](DFX6xxx.md) | Niagara 编译 | 资产写出来了，Niagara 自己不满意 |
| [`DFX7xxx`](DFX7xxx.md) | 溯源、漂移与 lint | 资产与源码失同步，或者 lint 觉得不对劲 |
| [`DFX8xxx`](DFX8xxx.md) | 反编译器 | 导出、Adopt、往返缺口 |

**提示：**

文档的一半是**机器写的**：严重级别、消息模板和抛出位置从插件源码里扫出来，
  `gen-diagnostics.ps1 -Check` 是防止它漂移的门。人写的那一半 —— 成因与修法 —— 只写一次。
  见 [ci.ps1 与门禁](../tools/ci.md)。

<a id="排错的顺序"></a>

## 排错的顺序

<a id="按码去查"></a>

### 按码去查
本页下面的表，或者对应的族页面。码是稳定的：`DFX3003` 永远是同一个失败模式。

<a id="拿真实签名核对"></a>

### 拿真实签名核对
`dfx.ps1 schema <Module> -Stack <Stack>` 打印构建看到的输入签名 ——
包括静态开关揭示出来的那些。名字的权威在资产上，不在记忆里。

<a id="还是不对就看边界"></a>

### 还是不对，就看边界
[能力边界](limitations.md) 列出不覆盖的东西与 1.0.0 的已知问题。

<a id="按码速查"></a>

## 按码速查


<a id="dfx1xxx--驱动与文件-io"></a>

### DFX1xxx — 驱动与文件 I/O

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX1000](DFX1xxx.md#dfx1000) | 错误 | Could not read source file '%s'. |
| [DFX1001](DFX1xxx.md#dfx1001) | 错误 | Unterminated string literal. |
| [DFX1002](DFX1xxx.md#dfx1002) | 错误 | Unterminated block comment. |
| [DFX1003](DFX1xxx.md#dfx1003) | 错误 | Unexpected character '%c' (U+%04X). |
| [DFX1004](DFX1xxx.md#dfx1004) | 错误 | Unterminated raw block: missing '}'. |
| [DFX1005](DFX1xxx.md#dfx1005) | 错误 | Unterminated back-quoted name. A `name` must close on the same line. |


<a id="dfx2xxx--词法与语法"></a>

### DFX2xxx — 词法与语法

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX2000](DFX2xxx.md#dfx2000) | 错误 | Unknown document type '%s'. Expected System, Emitter, Module or DynamicInput. |
| [DFX2001](DFX2xxx.md#dfx2001) | 错误 | Expected '&#123;' to open a raw block but found '%s'. |
| [DFX2002](DFX2xxx.md#dfx2002) | 错误 | Expected a name but found %s. |
| [DFX2003](DFX2xxx.md#dfx2003) | 错误 | Expected a number but found '%s'. |
| [DFX2004](DFX2xxx.md#dfx2004) | 错误 | Expected a value but found %s. |
| [DFX2005](DFX2xxx.md#dfx2005) | 错误 | '#EndRegion' has no matching '#Region'. |
| [DFX2006](DFX2xxx.md#dfx2006) | 错误 | Unknown directive '#%s'. Expected Region or EndRegion. |
| [DFX2007](DFX2xxx.md#dfx2007) | 错误 | Expected a version after '@'. |
| [DFX2008](DFX2xxx.md#dfx2008) | 错误 | Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'. |
| [DFX2010](DFX2xxx.md#dfx2010) | 警告 | '#Region \"%s\"' was never closed with '#EndRegion'. |
| [DFX2013](DFX2xxx.md#dfx2013) | 错误 | '%s' is a system-scope stack and must be written at the top level of a .dfs, not inside an emit… |
| [DFX2015](DFX2xxx.md#dfx2015) | 错误 | 'from' must be followed by a quoted path to a .dfe source file. |
| [DFX2016](DFX2xxx.md#dfx2016) | 错误 | (built at runtime) |
| [DFX2018](DFX2xxx.md#dfx2018) | 错误 | A Module or DynamicInput must declare a 'Body = &#123; }' block. |
| [DFX2019](DFX2xxx.md#dfx2019) | 错误 | Header argument '%s' must be a quoted string. |
| [DFX2020](DFX2xxx.md#dfx2020) | 错误 | Unknown curve key attribute '%s'. Expected Interp, Tangent, Arrive or Leave. |
| [DFX2021](DFX2xxx.md#dfx2021) | 错误 | File declares '%s' but its extension is '%s'. Rename the file to '%s' or change the declaration. |
| [DFX2022](DFX2xxx.md#dfx2022) | 错误 | Unexpected '%s' after the end of the document. A DreamFX file declares exactly one top-level ob… |
| [DFX2023](DFX2xxx.md#dfx2023) | 错误 | '%s' is a module call, so it cannot be given a type. Types are written only on assignments. |
| [DFX2024](DFX2xxx.md#dfx2024) | 错误 | 'disabled' can only prefix a module call, and '%s' is an assignment. |
| [DFX2025](DFX2xxx.md#dfx2025) | 错误 | Unknown OnEvent argument '%s'. Expected Source, Event, Mode, SpawnNumber, MaxEventsPerFrame, Up… |
| [DFX2026](DFX2xxx.md#dfx2026) | 错误 | Unknown Stage argument '%s'. Expected Iteration, DataInterface, NumIterations, ExecuteBehavior… |


<a id="dfx3xxx--声明与文档结构"></a>

### DFX3xxx — 声明与文档结构

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX3000](DFX3xxx.md#dfx3000) | 错误 | (built at runtime) |
| [DFX3001](DFX3xxx.md#dfx3001) | 错误 | (built at runtime) |
| [DFX3002](DFX3xxx.md#dfx3002) | 错误 | Could not read the input schema of module '%s': %s |
| [DFX3003](DFX3xxx.md#dfx3003) | 错误 | Module '%s' has no input named '%s'.%s Available inputs: %s |
| [DFX3004](DFX3xxx.md#dfx3004) | 错误 | Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, Light… |
| [DFX3005](DFX3xxx.md#dfx3005) | 错误 | Emitter '%s' is declared more than once. Emitter names are stable keys and must be unique. |
| [DFX3006](DFX3xxx.md#dfx3006) | 错误 | '%s' is neither an allowed inline function (%s) nor a dynamic input: %s |
| [DFX3007](DFX3xxx.md#dfx3007) | 错误 | Could not read the input schema of dynamic input '%s': %s |
| [DFX3008](DFX3xxx.md#dfx3008) | 错误 | Dynamic input '%s' has no input named '%s'. Available inputs: %s |
| [DFX3009](DFX3xxx.md#dfx3009) | 错误 | Dynamic input '%s' is pinned to version %s, which its asset does not offer. Available version(s… |
| [DFX3010](DFX3xxx.md#dfx3010) | 错误 | Property '%s': %s |
| [DFX3020](DFX3xxx.md#dfx3020) | 错误 | Unknown %s setting '%s'. Available settings: %s |
| [DFX3021](DFX3xxx.md#dfx3021) | 错误 | User parameter '%s' is declared more than once. |
| [DFX3030](DFX3xxx.md#dfx3030) | 错误 | A Module or DynamicInput must declare Settings.Usage -- it decides which stacks the module can… |
| [DFX3031](DFX3xxx.md#dfx3031) | 错误 | A DynamicInput must declare Settings.Output -- its return type cannot be inferred from the body. |
| [DFX3032](DFX3xxx.md#dfx3032) | 错误 | A DynamicInput's Usage must be DynamicInput, not '%s'. |
| [DFX3033](DFX3xxx.md#dfx3033) | 错误 | Input '%s' is declared more than once. |
| [DFX3034](DFX3xxx.md#dfx3034) | 错误 | Input '%s' is marked [StaticSwitch] but is a %s. A static switch must be a bool, an int or an e… |
| [DFX3035](DFX3xxx.md#dfx3035) | 错误 | Input '%s' is a [StaticSwitch], so its default must be a compile-time constant. |
| [DFX3036](DFX3xxx.md#dfx3036) | 错误 | The Body block is empty. |
| [DFX3037](DFX3xxx.md#dfx3037) | 错误 | (built at runtime) |
| [DFX3038](DFX3xxx.md#dfx3038) | 错误 | '%s' is not a stack a module can be placed in. Use one of: %s. |
| [DFX3039](DFX3xxx.md#dfx3039) | 错误 | A DynamicInput cannot return a data interface; its Output must be a value type. |
| [DFX3040](DFX3xxx.md#dfx3040) | 错误 | (built at runtime) |
| [DFX3041](DFX3xxx.md#dfx3041) | 错误 | '%s' could not be parsed; see the errors above. |
| [DFX3042](DFX3xxx.md#dfx3042) | 错误 | '%s' declares a %s, but 'from' needs an Emitter document. |
| [DFX3043](DFX3xxx.md#dfx3043) | 错误 | '%s' reads user parameters this system does not declare: %s. Add them to the Properties block. |
| [DFX3044](DFX3xxx.md#dfx3044) | 错误 | The default for input '%s' has to be a literal or an enum entry. A module input default is stor… |
| [DFX3045](history/DFX3045.md#dfx3045) | 错误 | '%s' is not a type a particle attribute can have. |
| [DFX3046](DFX3xxx.md#dfx3046) | 错误 | '%s' is not a particle attribute DreamFX knows the type of. Write the type at its first use in… |
| [DFX3047](DFX3xxx.md#dfx3047) | 错误 | A DynamicInput computes a value; it cannot write '%s'. Move the write into a Module. |


<a id="dfx4xxx--值类型与表达式"></a>

### DFX4xxx — 值、类型与表达式

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX4001](DFX4xxx.md#dfx4001) | 错误 | Input '%s' expects %s, but a number was written. |
| [DFX4002](DFX4xxx.md#dfx4002) | 错误 | Expression for '%s': a vector literal must have 2 to 4 components. |
| [DFX4003](DFX4xxx.md#dfx4003) | 错误 | Input '%s' is an integer, but %s was written. Narrowing is not implicit -- write int(%s) if tru… |
| [DFX4004](DFX4xxx.md#dfx4004) | 错误 | Input '%s' has no resolvable Niagara type. |
| [DFX4005](DFX4xxx.md#dfx4005) | 错误 | Property '%s': component %d is not a number. |
| [DFX4006](DFX4xxx.md#dfx4006) | 错误 | Input '%s' is a %s, which has no entry named '%s'. Valid entries: %s |
| [DFX4007](DFX4xxx.md#dfx4007) | 错误 | Input '%s' expects %s. '%s' is neither a literal of that type nor a parameter reference -- para… |
| [DFX4010](DFX4xxx.md#dfx4010) | 错误 | Input '%s' is set more than once on dynamic input '%s'. |
| [DFX4020](DFX4xxx.md#dfx4020) | 错误 | Parameter '%s': 'Object' needs an inner type, e.g. Object&lt;Texture>. |
| [DFX4021](DFX4xxx.md#dfx4021) | 错误 | Parameter '%s': no UObject class named '%s'. Object&lt;> takes a class name such as Object&lt;Texture… |
| [DFX4022](DFX4xxx.md#dfx4022) | 错误 | Cannot infer the type of '%s' from its value. A first assignment to a new attribute must use a… |
| [DFX4023](DFX4xxx.md#dfx4023) | 错误 | Property '%s': box() takes 6 numbers -- minX, minY, minZ, maxX, maxY, maxZ. |
| [DFX4024](DFX4xxx.md#dfx4024) | 错误 | Cannot type '%s = %s': the type of '%s' is not known here. Declare it in Properties, or assign… |
| [DFX4025](DFX4xxx.md#dfx4025) | 错误 | '%s' is not a valid assignment target. Parameter names are namespace-qualified, e.g. Particles.… |
| [DFX4026](DFX4xxx.md#dfx4026) | 错误 | 'Bind %s -> %s': the target must be a qualified parameter -- either a namespace such as Particl… |
| [DFX4027](DFX4xxx.md#dfx4027) | 错误 | '%s' is %s, but '%s' is %s. Linking binds a parameter directly -- there is no conversion. Decla… |
| [DFX4028](DFX4xxx.md#dfx4028) | 错误 | Default for '%s' needs a declared type. A default has no module signature to infer one from, so… |
| [DFX4029](DFX4xxx.md#dfx4029) | 错误 | Default for '%s' must be a literal, an enum or another parameter. A dynamic input computes a va… |
| [DFX4030](DFX4xxx.md#dfx4030) | 错误 | The hlsl block for '%s' must be a single expression: no statements, no local variables, no retu… |
| [DFX4031](DFX4xxx.md#dfx4031) | 错误 | '%s' is not an allowed inline function. Allowed: %s. Anything else belongs in a .dfm dynamic in… |
| [DFX4032](DFX4xxx.md#dfx4032) | 错误 | '%s' in the expression for '%s' is not a parameter. Only namespace-qualified parameters (User.X… |
| [DFX4033](DFX4xxx.md#dfx4033) | 错误 | Builtin '%s' takes positional arguments, not named ones. |
| [DFX4034](DFX4xxx.md#dfx4034) | 错误 | Builtin '%s' takes %d argument(s), but %d were written. |
| [DFX4035](DFX4xxx.md#dfx4035) | 错误 | Expression for '%s' contains a value that has no HLSL form. |
| [DFX4036](DFX4xxx.md#dfx4036) | 错误 | The hlsl block for '%s' is empty. |
| [DFX4037](DFX4xxx.md#dfx4037) | 错误 | '%s' is %s; a curve &#123; } literal only fits a curve data interface input. |
| [DFX4038](DFX4xxx.md#dfx4038) | 错误 | The curve for '%s' has no keys. |
| [DFX4039](DFX4xxx.md#dfx4039) | 错误 | Unknown curve interpolation '%s'. Expected Auto, Cubic, Linear or Constant. |
| [DFX4040](DFX4xxx.md#dfx4040) | 错误 | Parameter '%s': no asset at '%s'. |
| [DFX4041](DFX4xxx.md#dfx4041) | 错误 | Parameter '%s': '%s' is a %s, which is not a %s. |
| [DFX4042](DFX4xxx.md#dfx4042) | 错误 | Unknown curve tangent mode '%s'. Expected Auto, User, Break or None. |
| [DFX4091](DFX4xxx.md#dfx4091) | 错误 | Input '%s': dynamic inputs, hlsl blocks, curves and inline expressions are not available yet (p… |


<a id="dfx5xxx--生成与资产写入"></a>

### DFX5xxx — 生成与资产写入

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX5001](DFX5xxx.md#dfx5001) | 错误 | Stack '%s' has no Niagara script usage mapping. |
| [DFX5002](DFX5xxx.md#dfx5002) | 警告 | This system declares no emitters, so it will produce nothing. |
| [DFX5003](DFX5xxx.md#dfx5003) | 提示 | '%s' is not declared in this source, so its existing modules are left as-is: %s |
| [DFX5004](DFX5xxx.md#dfx5004) | 提示 | No Material was set, so the engine default was applied: %s. Write 'Material = \"...\";' to choo… |
| [DFX5030](DFX5xxx.md#dfx5030) | 错误 | SavePackage failed for '%s'. |
| [DFX5031](DFX5xxx.md#dfx5031) | 错误 | Emitter '%s' declares more than one OnEvent block. Only one event handler per emitter is repres… |
| [DFX5032](DFX5xxx.md#dfx5032) | 错误 | Emitter '%s' declares two Stage blocks named '%s'. Stages are identified by name; rename one. |
| [DFX5033](DFX5xxx.md#dfx5033) | 错误 | Emitter '%s' declares Stage blocks but simulates on the CPU. Simulation stages are a GPU featur… |
| [DFX5034](DFX5xxx.md#dfx5034) | 错误 | Emitter '%s' uses 'as %s' on two module calls. Node names are unique per emitter; rename one. |
| [DFX5093](DFX5xxx.md#dfx5093) | 错误 | 'MaterialParam' is reserved syntax and is not implemented in v1 (plan section 7). |
| [DFX5097](DFX5xxx.md#dfx5097) | 错误 | Only System documents can be generated right now; this file declares a %s. |
| [DFX5098](DFX5xxx.md#dfx5098) | 错误 | Data interface parameter '%s' takes its configuration as a quoted JSON object, the form the dec… |
| [DFX5099](history/DFX5099.md#dfx5099) | 提示 | [Group] and [SortPriority] are kept in source only: the external edit API's user variable struc… |
| [DFX5100](DFX5xxx.md#dfx5100) | 错误 | '%s' is a %s with no generated asset at '%s', and %s |
| [DFX5101](DFX5xxx.md#dfx5101) | 错误 | (built at runtime) |
| [DFX5102](DFX5xxx.md#dfx5102) | 提示 | Input '%s' is marked [StaticSwitch]. Tier-one generation (plan 3.3) lowers the whole Body to a… |
| [DFX5103](DFX5xxx.md#dfx5103) | 错误 | Package '%s' exists on disk but could not be loaded. |
| [DFX5104](DFX5xxx.md#dfx5104) | 错误 | Package '%s' exists but holds no Niagara script named '%s'. Refusing to overwrite it. |
| [DFX5105](DFX5xxx.md#dfx5105) | 错误 | Could not create package '%s'. |
| [DFX5106](DFX5xxx.md#dfx5106) | 错误 | Could not wire the module graph. The Niagara schema rejected a parameter map connection. |
| [DFX5107](DFX5xxx.md#dfx5107) | 错误 | '%s' no longer matches the module asset at '%s', and this build cannot regenerate it. Rebuild i… |


<a id="dfx6xxx--niagara-编译"></a>

### DFX6xxx — Niagara 编译

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX6001](DFX6xxx.md#dfx6001) | 错误 | (built at runtime) |
| [DFX6002](DFX6xxx.md#dfx6002) | 警告 | (built at runtime) |
| [DFX6003](DFX6xxx.md#dfx6003) | 错误 | (built at runtime) |
| [DFX6004](DFX6xxx.md#dfx6004) | 警告 | (built at runtime) |
| [DFX6005](DFX6xxx.md#dfx6005) | 错误 | Niagara compilation of '%s' did not succeed (status %s). |
| [DFX6006](DFX6xxx.md#dfx6006) | 错误 | Niagara could not compile the body of '%s': %s |
| [DFX6007](DFX6xxx.md#dfx6007) | 提示 | Nothing in this source writes that parameter, so there is no graph parameter for a default to s… |
| [DFX6008](DFX6xxx.md#dfx6008) | 错误 | '%s' finished its compile with stale scripts: %s. The compiled VM was not rebuilt from the grap… |


<a id="dfx7xxx--溯源漂移与-lint"></a>

### DFX7xxx — 溯源、漂移与 lint

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX7001](DFX7xxx.md#dfx7001) | 错误 | Asset '%s' carries no DreamFX provenance stamp: it was never generated from this source, or it… |
| [DFX7002](DFX7xxx.md#dfx7002) | 错误 | Asset '%s' is stale: it was generated from a different revision of this source. Run the DreamFX… |
| [DFX7003](DFX7xxx.md#dfx7003) | 警告 | Asset '%s' was generated by DreamFX %s; the current generator is %s. A rebuild is recommended. |
| [DFX7004](DFX7xxx.md#dfx7004) | 错误 | Asset '%s' does not exist: this source has never been built. |
| [DFX7005](DFX7xxx.md#dfx7005) | 错误 | (built at runtime) |
| [DFX7101](DFX7xxx.md#dfx7101) | 警告 | Emitter '%s' simulates on the GPU but declares no FixedBounds. GPU emitters cannot compute thei… |
| [DFX7102](DFX7xxx.md#dfx7102) | 警告 | Emitter '%s' spawns by rate ('%s') with no upper bound. Set AllocationMode = Fixed and PreAlloc… |
| [DFX7103](DFX7xxx.md#dfx7103) | 警告 | Emitter '%s' uses randomness ('%s') but does not set Determinism = true, so it will look differ… |
| [DFX7104](DFX7xxx.md#dfx7104) | 警告 | This renderer has no Material and DreamFX knows no default for its type, so it will not draw. S… |


<a id="dfx8xxx--反编译器"></a>

### DFX8xxx — 反编译器

| 码 | 级别 | 消息 |
| --- | --- | --- |
| [DFX8000](DFX8xxx.md#dfx8000) | 错误 | Cannot decompile a null system. |
| [DFX8001](DFX8xxx.md#dfx8001) | 错误 | Could not read emitters: %s |
| [DFX8002](DFX8xxx.md#dfx8002) | 警告 | Skipping emitter '%s': %s |
| [DFX8003](DFX8xxx.md#dfx8003) | 错误 | Cannot decompile a null emitter. |
| [DFX8004](DFX8xxx.md#dfx8004) | 错误 | Could not create a host system to read the emitter through: %s |
| [DFX8005](DFX8xxx.md#dfx8005) | 错误 | Could not copy emitter '%s' into a host system: %s |
| [DFX8006](DFX8xxx.md#dfx8006) | 错误 | Could not read emitter '%s': %s |
| [DFX8010](DFX8xxx.md#dfx8010) | 错误 | '%s' cannot be adopted: '%s' has no DreamFXLang form yet, so adopting would destroy it on the f… |
| [DFX8011](DFX8xxx.md#dfx8011) | 错误 | '%s' is already generated by an existing source, so a second one would silently overwrite it on… |
| [DFX8012](DFX8xxx.md#dfx8012) | 错误 | '%s' was adopted, but re-exporting the rebuilt asset does not reproduce this file. First differ… |
| [DFX8013](DFX8xxx.md#dfx8013) | 错误 | This file sits in the decompiled output directory but Name=\"%s\" builds '%s', outside the '%s/… |
| [DFX8014](DFX8xxx.md#dfx8014) | 警告 | Emitter '%s' inherits from '%s'. The export flattens the inheritance: the merged stack is carri… |
| [DFX8015](DFX8xxx.md#dfx8015) | 警告 | Emitter '%s' carries %d event handler(s) this export cannot represent%s. The rebuilt emitter wi… |
| [DFX8016](DFX8xxx.md#dfx8016) | 警告 | Emitter '%s' carries %d simulation stage(s) this export cannot represent (custom stage class or… |
