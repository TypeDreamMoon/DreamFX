---
title: 日常循环
description: 保存即重建、实时预览、批量重建闸门、必须遵守的两条硬规矩，以及提交前的四步 CI。
---

<a id="保存即重建"></a>

## 保存即重建

编辑器监视每一个 `DFX/` 根。存盘 → 那个源码重建 → 生成的资产刷新。把生成的资产在 Niagara 编辑器里
开着，它就是实时预览，配置成本为零。

失败的构建会弹一个带 **Open in VSCode** 链接的 toast，直接跳到出错的那一行那一列。生成资产的
「这是自动生成的」警告条上也有一个指向源文件的链接。

**警告：**

<a id="重建之前先把实例弄掉"></a>

### 重建之前先把实例弄掉

这条不是可选项：**重建一个系统之前，先让它的实例消失。** 热替换的瞬间，活着的 `NiagaraActor`
可能看到一个建到一半的 renderer 布局，然后在渲染 worker 上断言 —— 编辑器跟着一起走。

这是引擎侧的竞态，DreamFX 挡不住。

<a id="批量重建有闸门"></a>

## 批量重建有闸门

重新导出一个内容包、切个分支、跑个脚本改写，一次会动很多源码，而每个系统的重建都会往编译队列里
塞一批 Niagara 编译作业。在这个项目上量过：24 个系统一起重建 = 237 个作业，编辑器直接躺下。

所以超过 **Bulk Rebuild Threshold**（*Project Settings ▸ Plugins ▸ DreamFX*，默认 8）个文件的批次
不会自动建，而是**问你**：

> DreamFX: *N* source files changed at once. Rebuilding them all now would queue hundreds of Niagara
> compiles. — **Rebuild them now**

文件是**留着**的，不是丢掉的 —— 丢掉会留下一堆过期资产而没人知道，那正是这道闸门要防的事，
只是更安静。toast 过期了也没关系，*Tools ▸ DreamFX ▸ Rebuild DFX* 做同一件事。

普通的一次保存永远不过闸（一个文件，或不超过阈值的批次都立刻建），显式点的 *Rebuild DFX* 也不过闸 ——
那是你要求的。

**提示：**

这道闸门最早是照另一个理论写的：以为危险来自「编辑器关着时改的东西在开机时被 watcher 重放」。
  测量否掉了它 —— `RegisterDirectoryChangedCallback_Handle` 从注册时刻开始监视，不报积压：
  关着编辑器改 12 个文件，重开后**什么都没有发生**。真正伤人的批次是编辑器**开着**时到达的那种。
  修之前用 12 个文件量到：54 次 Niagara 系统编译、8 次编译池饱和；修之后：一个 toast，
  在你点之前什么都不建。

<a id="提交之前"></a>

## 提交之前

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

四步，每一步抓的东西别的步骤抓不到：

| 步骤 | 抓什么 |
| --- | --- |
| `lint` | GPU emitter 没有 bounds、按速率生成没有上限、用了随机没设种子 |
| `build` | 任何让文本变不成资产的东西 |
| `verify` | **源码改了、没重建、连着提交了** —— 只跑 build 永远看不见，因为 build 会把它修好 |
| `corpus` | 行为变了而不是坏了：诊断挪位、拓扑漂移、反编译失去幂等 |

`verify` 是最值得理解的一步。它拿每个资产的溯源戳去比它的源码：对不上，意味着提交进去的资产和
提交进去的文本互相矛盾 —— 这件事换任何别的手段都看不见。

**警告：**

**先关编辑器。** `build` 和 `corpus` 写 `.uasset`，编辑器开着也在写同一批。谁后存谁赢，
  而且谁都不吭声。CI 看见有编辑器进程会警告，但它不知道那个编辑器开的是哪个项目，所以是建议不是门。

<a id="一天里的形状"></a>

## 一天里的形状

<a id="写"></a>

### 写

在 VSCode 里编辑 `.dfs`（*Tools ▸ DreamFX ▸ Open DreamFX Workspace* 一键开好 workspace）。
补全和 hover 来自 `DFX/.dfx-index.json`，换了引擎或启用了新内容插件之后跑一次 `dfx.ps1 index` 刷新。

<a id="存"></a>

### 存
存盘即重建。Niagara 预览窗口刷新。出错的话点 toast 上的 **Open in VSCode** 跳到那一行。

<a id="查"></a>

### 查
拿不准模块的输入名，跑 `dfx.ps1 schema <Module> -Stack <Stack>` —— 它跑的是和构建同一条探测，
静态开关揭示出来的输入也在里面。

<a id="提交"></a>

### 提交
关编辑器，跑 `ci.ps1`，绿了再提交。

<a id="想知道自己的内容能被覆盖多少"></a>

## 想知道自己的内容能被覆盖多少

这个问题不该靠别人的数字回答，自己量：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff       # L1 文本 + L2 编译
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff        # 资产事实，不经导出器
```

见[往返与等价](../generation/roundtrip.md)。
