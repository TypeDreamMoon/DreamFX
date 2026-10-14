---
title: "ci.ps1 与门禁"
description: "四步 CI 各自能抓住什么问题、在什么场合用哪个开关、文档漂移检查，以及一条最小流水线。"
---

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

一条命令，四步，顺序安排为最快的失败最先出现：


<a id="lint--all"></a>

### `lint -All`
只扫描源码，不访问资产，所以它失败得最快。能抓住：没有边界（bounds）的 GPU 发射器、
没有上限的生成速率、没有设种子的随机、没有材质的渲染器。

<a id="build--all--force"></a>

### `build -All -Force`
每个 `.dfs` 和 `.dfm` 都生成，并且它的 Niagara 编译干净。`-Force` 是关键 —— 门禁
不能因为「哈希没变」而跳过任何东西。

<a id="verify--all"></a>

### `verify -All`
每个生成资产的戳记都与它的源码匹配。**这一步抓的是没人会注意的那种情况**：有人改了 `.dfs`，
没有重新构建，却把两者都提交了。单跑构建会通过，
因为构建会把它修好。

<a id="corpus"></a>

### `corpus`
`Tests/Corpus` 套件：按代码与位置比对诊断、黄金拓扑、反编译
幂等性。**它抓的是行为发生变化，而不是行为损坏。**


退出码 0 表示四步全部通过；其他任何值都是第一个失败步骤的退出码。

> **注意：**
>
> 为什么第 4 步值得付出这个代价：DreamFX 关于 Niagara 的几乎全部知识都是通过
> **实验**确立的，而不是由某个类型保证的。正是语料让其中任何一处悄然变化都会失败。

<a id="switches"></a>

## 开关

| 开关 | 何时使用 |
| --- | --- |
| `-SkipBuild` | 只检查、不得写入工作树的门禁。此时 `verify` 会报告任何尚未构建的源码 |
| `-SkipCorpus` | 快速的本地检查。**绝不**用于门禁 —— 语料会启动编辑器，开销比另外三步加起来还大，因此跳过它就等于跳过行为回归 |
| `-CleanNew` | 删除构建过程中新建的资产，用于不留痕迹的门禁。这种情况下会跳过 `verify`：它要检查的资产刚刚被有意删除了 |
| `-Project` / `-Engine` | 显式指定项目和引擎根目录 |

> **警告：**
>
> **请在关闭编辑器的情况下运行。** 第 2 步和第 4 步会写入 `.uasset` 文件，而在同一项目上打开的编辑器
> 也会写入它们 —— 谁后保存谁赢，而双方都不会说明这一点。

<a id="the-docs-must-not-drift"></a>

## 文档不得漂移

```bash
pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
```

诊断文档有一半是**机器生成**的：扫描源码中的
`Diagnostics.Error/Warning/Info(TEXT("DFXnnnn"), ...)`，并为每个千位范围写一页，
每个代码一节，包含其严重级别、消息模板以及抛出它的位置。人工编写的那一半 —— 原因与修复方法 —— 原样保留。

`-Check` 把它变成一道门禁：如果页面过期就退出码 1。这就是 CI 发现某个新代码无人记录的方式。

> **注意：**
>
> 这样拆分的理由很直接：**会腐烂的那一半（存在哪些代码、它们说什么）是重新生成的，而不会腐烂的那一半（为什么发生）只写一次。**

<a id="a-minimal-pipeline"></a>

## 一条最小流水线

```yaml
# Sketch: any CI is these four steps plus the docs gate
steps:
  - run: pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
  - run: pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

它首先需要：一台 Windows 机器、一个可解析的引擎（`UE_ENGINE_ROOT` 或已注册的
`EngineAssociation`）、与创作项目所启用的相同内容插件，以及**没有编辑器打开该项目**。

<a id="deeper-checks"></a>

## 更深入的检查

除了这四步之外，有几件事值得在发布轮次而不是每次提交时运行：

```bash
dfx.ps1 verify -All -StrictVersions   # module version drift becomes an error
dfx.ps1 mirror-diff                    # L1 text + L2 compile
dfx.ps1 asset-diff                     # asset facts, routing around the exporter
dfx.ps1 coverage                       # coverage, bucketed by feature
```

参见[往返与等价](../generation/roundtrip.md)。
