---
title: 溯源与 verify
description: 每个生成资产上盖的那枚戳里有什么，verify 报的三种漂移，以及 lint 的四条规则。
---

<a id="那枚戳"></a>

## 那枚戳

每个 DreamFX 生成的资产上都带着一份溯源记录：

| 字段 | 用来回答 |
| --- | --- |
| 源文件路径 | 这个资产是从哪个 `.dfs` 来的 |
| 源码 hash | 那份源码从此变过没有 |
| 生成器版本 | 是哪一版 DreamFX 建的 |
| 模块版本 GUID | 它调用的那些模块从此变过没有 |

这枚戳有三个直接用途：**跳过没改的源码**（构建）、**发现失同步**（verify）、
**认出这个资产是生成的**（编辑器右键菜单据此在「反编译」和「打开源码」两套里切换）。

<a id="verify-抓什么"></a>

## `verify` 抓什么

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 verify -All
```

它什么都不写 —— 不写资产，不写戳 —— 只比。`.dfe` 会跳过（它不产资产，没什么可比的）。

| 码 | 意思 |
| --- | --- |
| [DFX7001](../diagnostics/DFX7xxx.md#dfx7001) | 资产没有 DreamFX 戳：它从来不是这份源码生成的，或者是手建的 |
| [DFX7002](../diagnostics/DFX7xxx.md#dfx7002) | 资产**过期**：它是照这份源码的另一个版本生成的 |
| [DFX7003](../diagnostics/DFX7xxx.md#dfx7003) | 资产是旧版 DreamFX 生成的（警告，建议重建） |
| [DFX7004](../diagnostics/DFX7xxx.md#dfx7004) | 资产不存在：这份源码从没建过 |
| [DFX7005](../diagnostics/DFX7xxx.md#dfx7005) | 模块版本漂移（`-StrictVersions` 下升级为错误） |

**提示：**

**`verify` 是四步 CI 里唯一能抓到那件事的一步**：有人改了 `.dfs`、没重建、把两个都提交了。
  单跑 `build` 永远不会失败 —— 因为 build 会把它修好。只有拿戳去比源码，
  才看得见「提交进去的资产」和「提交进去的文本」互相矛盾。

`-StrictVersions` 把 R7 模块版本不一致从警告升成错误。发布门用它：资产是照另一批模块建出来的、
而文本描述的是另一批，这种东西不该发出去。

<a id="lint-的四条规则"></a>

## `lint` 的四条规则

lint 只看源码，不碰资产，所以它是四步里最快失败的那一步：

| 码 | 抓什么 |
| --- | --- |
| [DFX7101](../diagnostics/DFX7xxx.md#dfx7101) | GPU emitter 没写 `FixedBounds`。GPU emitter 算不出自己的 bounds |
| [DFX7102](../diagnostics/DFX7xxx.md#dfx7102) | 按速率生成（`SpawnRate` / `SpawnPerUnit` / `SpawnPerFrame`）却没有上限 |
| [DFX7103](../diagnostics/DFX7xxx.md#dfx7103) | 用了随机却没设 `Determinism = true`，于是每次跑都不一样 |
| [DFX7104](../diagnostics/DFX7xxx.md#dfx7104) | renderer 没有 Material，而 DreamFX 也不知道这个类型的默认值 —— 它不会画 |

**提示：**

DFX7102 不把 `SpawnParticlesInGrid` 当按速率生成：它和 `SpawnBurst` 一样是每循环一次的
  burst（X·Y·Z），上限由循环决定。判据要贴着语义走，不然 lint 就变成噪音。

<a id="编辑器侧的同一件事"></a>

## 编辑器侧的同一件事

*Tools ▸ DreamFX ▸ Verify DFX* 跑的是同一趟，结果是一条 toast：

| toast | 条件 |
| --- | --- |
| `DreamFX: {N} source(s) verified, all assets in step.` | 干净 |
| `DreamFX: {Drifted} of {N} source(s) out of step ({Failed} failed). See the Output Log.` | 有漂移或失败 |

<a id="手改了生成的资产会怎样"></a>

## 手改了生成的资产会怎样

下一次重建把它铲掉，不问你。这不是缺陷，是这个模型的定义：**文本是唯一真相**。

生成资产在编辑器里带一条警告条，上面有一个指向源文件的链接 —— 从那里改。
