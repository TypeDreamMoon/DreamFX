---
title: ci.ps1 与门禁
description: 四步 CI 各自抓什么、参数怎么选、文档漂移检查，以及一条能放进流水线的最小配置。
---

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

一条命令，四步，顺序是「最快失败的先跑」：

<a id="lint--all"></a>

### `lint -All`
只读源码，不碰资产，所以它失败得最快。抓：GPU emitter 没 bounds、按速率生成没有上限、
用了随机没设种子、renderer 没有材质。

<a id="build--all--force"></a>

### `build -All -Force`
每个 `.dfs` / `.dfm` 都生成，且 Niagara 编译干净。`-Force` 是关键 —— 门禁不能因为
「hash 没变」而跳过任何东西。

<a id="verify--all"></a>

### `verify -All`
每个生成资产上的戳都与它的源码匹配。**这一步抓的是没人会注意的那件事**：有人改了 `.dfs`、
没重建、把两个都提交了。单跑 build 会通过，因为 build 会把它修好。

<a id="corpus"></a>

### `corpus`
`Tests/Corpus` 套件：按码与位置比对诊断、黄金拓扑、反编译幂等。**它抓的是行为变了而不是坏了。**

退出码 0 表示四步全过，否则是第一个失败步骤的码。

**提示：**

第 4 步为什么值得付这个时间：DreamFX 关于 Niagara 的几乎每一条知识都是**实验**得来的，
  不是某个类型系统保证的。语料就是让这些知识里任何一条悄悄变化时会失败的那个东西。

<a id="参数"></a>

## 参数

| 参数 | 用在什么场合 |
| --- | --- |
| `-SkipBuild` | 只检查，不允许写工作树的门禁。`verify` 随后会报出所有没被建过的源码 |
| `-SkipCorpus` | 本地快速检查。**不要**用在门禁上 —— 语料要 boot 编辑器，比另外三步加起来还贵，跳过它就等于跳过了行为回归 |
| `-CleanNew` | 删掉构建新建的资产，让门禁不留痕迹。此时 `verify` 会被跳过（它要检查的资产刚被故意删掉了） |
| `-Project` / `-Engine` | 显式指定项目与引擎根 |

**警告：**

**关着编辑器跑。** 第 2 步和第 4 步写 `.uasset`，编辑器开着也在写同一批。谁后存谁赢，
  两边都不吭声。

<a id="文档不许漂移"></a>

## 文档不许漂移

```bash
pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
```

诊断文档的一半是**机器写的**：扫源码里的
`Diagnostics.Error/Warning/Info(TEXT("DFXnnnn"), ...)`，按千位段一页，每个码一节，
写下它的严重级别、消息模板和抛出位置。人写的那一半 —— 成因与修法 —— 原样保留。

`-Check` 把它变成门：文档过期就 exit 1。这就是 CI 怎么发现「有人加了一个码但没人写它」。

**提示：**

这样切分的理由很直接：**会腐烂的那一半（有哪些码、它们说什么）被重新生成，
  不会腐烂的那一半（为什么会发生）只写一次。**

<a id="一个最小流水线"></a>

## 一个最小流水线

```yaml
# 概念示意：任何 CI 都是这四步 + 一个文档门
steps:
  - run: pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
  - run: pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

跑之前要有：Windows 机器、能解析到的引擎（`UE_ENGINE_ROOT` 或注册好的
`EngineAssociation`）、和创作项目相同的内容插件集合，以及**没有编辑器开着这个项目**。

<a id="更深的检查"></a>

## 更深的检查

这四步之外，还有几件事值得在发布轮而不是每次提交时跑：

```bash
dfx.ps1 verify -All -StrictVersions   # 模块版本漂移视为错误
dfx.ps1 mirror-diff                    # L1 文本 + L2 编译
dfx.ps1 asset-diff                     # 资产事实，绕开导出器
dfx.ps1 coverage                       # 覆盖率分桶
```

见[往返与等价](../generation/roundtrip.md)。
