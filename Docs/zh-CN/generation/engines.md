---
title: 引擎与后端
description: 同一份插件源码怎么在自维护引擎和预编译安装版上都跑 —— 零 #if 分叉、三种 .dfm 生成结果，以及实验性 API 的漂移策略。
---

<a id="一份源码两个家"></a>

## 一份源码，两个家

DreamFX 在**自维护的源码引擎**（MoonEngine，5.8.1）和**预编译安装版**（stock installed 5.8.1）上
都构建、都验证，而且是**同一份插件源码零 `#if` 分叉**。

这不是口号，是一条约束。它导致的设计选择很具体：能力差异不靠预处理器分叉，而是**运行时选择后端**，
并且两条路互相对拍。

<a id="dfm-的三种结果"></a>

## `.dfm` 的三种结果

把 HLSL 写到 Niagara custom 节点上需要 `UNiagaraNodeCustomHlsl::SetCustomHlsl`，
围着它建图还要另外四个声明。MoonEngine 给这五个都打了导出宏，stock 引擎一个都没有。

但**导出**和**可达**不是一回事 —— 公有数据成员不需要导出宏，公有虚函数走虚表，
作为 `UPROPERTY` 的私有字段可以按名字写。于是：

| 后端 | 什么时候 | 行为 |
| --- | --- | --- |
| **direct** | 引擎导出了那五个声明 | 直接调用；MoonEngine 走这条 |
| **reflection** | 没导出，但依赖的形状都对得上 | 从公有面重建每一个操作 |
| **degraded** | 依赖的某个形状动了 | **拒绝生成**，并说清是哪一个 |

反射后端是**对着直接后端验过的**，不是假定等价：同一个 `.dfm` 两种方式建出来，
读回的 schema 逐字节相同，跨引擎也成立。

```bash
# 在不需要它的引擎上强制走反射后端 —— 两条路在一台机器上互相 diff 的方法
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Modules/ToonSpin.dfm -ForceReflectionBackend
```

**提示：**

**受限的从来不是产物。** 生成出来的模块是普通 `UNiagaraScript`，任何引擎都能加载、引用、cook、跑。
  *在能生成的地方生成，提交资产，所有人消费。*

两个后端都跑不动时：

| 情况 | 结果 |
| --- | --- |
| 资产已提交且与源码匹配 | 构建跳过，CI 绿 |
| 源码改了没重新生成 | [DFX5107](../diagnostics/DFX5xxx.md#dfx5107) |
| 根本没有资产 | [DFX5100](../diagnostics/DFX5xxx.md#dfx5100)，点名是哪项检查没过 |

<a id="实验性-api漂移要显形"></a>

## 实验性 API：漂移要显形

DreamFX 依赖引擎的 Niagara 外部编辑 API（`UNiagaraExternalEditUtilities`），它在引擎侧标着
EXPERIMENTAL。**启动自检把漂移显形，绝不静默吸收。**

这条选择的代价是：引擎升级后可能需要动 DreamFX。收益是：它不会在你不知情的时候，
用一个降级过的路径生成出一个看起来对、其实不对的资产。

<a id="宿主项目的内容插件必须对齐"></a>

## 宿主项目的内容插件必须对齐

模块探针需要基线。创作项目启用了 `NiagaraFluids` 这类内容插件，消费方也必须启用；
否则引用其模块的源会以缺口或编译错的形式显形。

**一整族解析不了的名字共享同一个前缀，说明的是挂载点没挂，不是源码写错了。**
这是环境要求，不是缺陷。

<a id="老引擎56--57"></a>

## 老引擎（5.6 / 5.7）

外部编辑 API 是 5.8 才有的。较老引擎走一层 `Compat/`：补上同名 API，adapter 一行不改。
它属于**未发布**的工作，1.0.0 的发布验证跑在 5.8 上；已知的两个修复记在
[更新日志](../changelog/index.md)里，其中一个值得知道 —— 那层 compat 枚举模块输入时曾在第一个输入就停手，
而模块输入是一棵**层级树**：开关揭示的输入是那个开关的**子节点**，不是兄弟。
于是静态开关看起来像没生效，它揭示的每个实参都被当成拼写错误（DFX3003）。
5.8 与 MoonEngine 用引擎自带 API，从不走这条路，不受影响。

<a id="发布验证快照"></a>

## 发布验证快照

1.0.0（2026-08-13，commit `4e23c55`）两个引擎上的门：

| 门 | Moon（源码 5.8.1） | stock（installed 5.8.1） |
| --- | --- | --- |
| ci（lint→build→verify→corpus） | OK，55 verified / corpus 55 | corpus 55/55（单跑） |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0（连续两跑） |
| mirror-diff | L1 45/45，L2 45/45 | L1 24/24，L2 24/24 |
| asset-diff | 6 same / 39 different（全部为已宣告族） | — |
| L3 运行时等价 | 45 对：7 exact / 0 differ / 38 undecidable | — |
