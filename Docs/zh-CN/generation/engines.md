---
title: "引擎与后端"
description: "同一份插件源码如何在源码构建版与安装版引擎上运行——零 #if 分叉、.dfm 生成的三种结果，以及实验性 API 的漂移策略。"
---

<a id="one-source-tree-two-homes"></a>

## 一份源码树，两个归属

DreamFX 在**源码构建版引擎**（MoonEngine，5.8.1）和**安装版引擎**（原版 5.8.1）上均能构建并通过验证——来自**同一份零 `#if` 分叉的插件源码树**。

这是一条约束，而非口号，并驱动了一个具体的设计选择：能力差异不在预处理器中分叉，而是**在运行时选择后端**，且两个后端会相互比对。

<a id="three-outcomes-for-dfm"></a>

## `.dfm` 的三种结果

将 HLSL 写入 Niagara 自定义节点需要 `UNiagaraNodeCustomHlsl::SetCustomHlsl`，而围绕它构建图表还需要另外四个声明。MoonEngine 为全部五者都提供了导出宏；原版引擎则一个都不导出。

但*已导出*与*可达*并非一回事——公有数据成员无需导出宏，公有虚函数通过虚表分发，而作为 `UPROPERTY` 的私有字段可以按名称写入。因此：

| 后端 | 何时 | 行为 |
| --- | --- | --- |
| **direct** | 引擎导出这五个声明 | 调用它们；即 MoonEngine |
| **reflection** | 未导出，但其依赖的所有形态均通过检查 | 从公有接口重建每个操作 |
| **degraded** | 其依赖的某个形态已变动 | **拒绝生成**，并指明是哪一个 |

反射后端是**对照直接后端验证的**，而非假定等价：同一个 `.dfm` 以两种方式构建后读回的 schema 逐字节相同，且跨引擎均成立。

```bash
# force the reflection backend on an engine that does not need it —
# this is how the two paths are diffed on one machine
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Modules/ToonSpin.dfm -ForceReflectionBackend
```

> **注意：**
>
> **产品从来不是受限的部分。** 生成的模块是普通的 `UNiagaraScript`：任何引擎都能加载它、引用它、cook 它并运行它。*在能工作的地方生成，提交资产，所有人皆可使用。*

当两个后端都无法运行时：

| 情况 | 结果 |
| --- | --- |
| 资产已提交且与源码匹配 | 构建跳过，CI 保持通过 |
| 源码已修改但未重新生成 | [DFX5107](../diagnostics/DFX5xxx.md#dfx5107) |
| 完全没有资产 | [DFX5100](../diagnostics/DFX5xxx.md#dfx5100)，并指明未通过的检查 |

<a id="an-experimental-api-drift-must-surface"></a>

## 实验性 API：漂移必须显形

DreamFX 依赖引擎的 Niagara 外部编辑 API（`UNiagaraExternalEditUtilities`），引擎将其标记为 EXPERIMENTAL。**启动自检会显形漂移；它绝不静默吸收。**

这一选择的代价是，引擎升级可能需要改动 DreamFX。其收益是，它不会在无人察觉的情况下，通过一条看似正确实则不然的降级路径生成资产。

<a id="host-projects-must-align-their-content-plugins"></a>

## 宿主项目必须对齐其内容插件

模块探针需要一个基线。如果创作项目启用了诸如 `NiagaraFluids` 之类的内容插件，消费者也必须启用它；否则引用其模块的源码会以缺口或编译错误的形式显形。

**一整族无法解析的名称共享同一前缀，说明缺失的是挂载点，而非源码有误。** 这是环境要求，不是缺陷。

<a id="older-engines-56--57"></a>

## 较旧引擎（5.6 / 5.7）

外部编辑 API 于 5.8 引入。较旧引擎通过一个 `Compat/` 层，以另一条途径提供相同的 API，因此适配器完全无需改动。该工作**尚未发布**——1.0.0 发布轮验证于 5.8——目前的两项修复记录在[更新日志](../changelog/index.md)中。

其中一项值得了解。兼容层枚举模块输入并在第一个输入处便停止，但模块输入是一棵**层级树**：由开关揭示的输入是该开关的**子节点**，而非其兄弟。因此一个静态开关看起来毫无效果，而它所揭示的每一个参数都被报告为拼写错误（DFX3003）。5.8 与 MoonEngine 使用引擎自身的 API，从不走这条路径。

<a id="release-verification-snapshot"></a>

## 发布验证快照

1.0.0 门禁（2026-08-13，commit `4e23c55`）在两个引擎上：

| 门禁 | Moon（源码 5.8.1） | 原版（安装版 5.8.1） |
| --- | --- | --- |
| ci（lint→build→verify→corpus） | OK，55 已验证 / corpus 55 | corpus 55/55（单独运行） |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0（连续两次） |
| mirror-diff | L1 45/45，L2 45/45 | L1 24/24，L2 24/24 |
| asset-diff | 6 same / 39 different（全部为已声明族） | — |
| L3 运行时等价 | 45 对：7 exact / 0 differ / 38 undecidable | — |
