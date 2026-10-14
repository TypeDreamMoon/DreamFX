---
title: "日常循环"
description: "保存即重建、实时预览、批量重建闸门、两条硬规矩，以及提交前的四步 CI。"
---

<a id="save-to-rebuild"></a>

## 保存即重建

编辑器监视每一个 `DFX/` 根。保存源码，它就会重建，生成的资产随之刷新。
把这个资产留在 Niagara 编辑器中打开，它就是你的实时预览，无需任何配置成本。

构建失败时弹出的 toast 带有 **Open in VSCode** 链接，可直接跳转到出错的行和列。
生成资产上的“此资产为生成内容”警告也带有指向其源文件的链接。

> **警告：**
>
> ### 重建前先销毁实例
>
> 这一条不是可选项：**重建一个系统之前，先销毁它的实例。** 在热替换期间，
> 一个存活的 `NiagaraActor` 可能看到构建到一半的渲染器布局，并在渲染工作线程上触发断言，
> 这会把编辑器一起带走。
>
> 这是引擎的竞态问题，不是 DreamFX 能防住的。

<a id="the-bulk-batch-is-gated"></a>

## 批量批次有闸门

重新导出内容包、切换分支，或运行脚本化改写，都会一次改动许多源文件，
而每次重建都会为每个系统排入一次 Niagara 编译。在本项目上实测：24 个系统
一起重建，向编译队列排入 237 个作业，编辑器直接崩溃。

因此，包含超过 **Bulk Rebuild Threshold** 个文件（*Project Settings ▸ Plugins ▸
DreamFX*，默认 8）的批次会被**询问**而不是直接构建：

> DreamFX：一次有 *N* 个源文件发生变更。现在全部重建会排入数百个 Niagara
> 编译。 — **立即重建**

这些文件会被保留，而不是丢弃：丢弃它们会让资产停留在过期状态且无人察觉，
这正是这道闸门要防止的失败，只是更安静。如果 toast 过期了，*Tools
▸ DreamFX ▸ Rebuild DFX* 起到相同作用。

普通保存永远不会被闸门拦截——单个文件，或任何不超过阈值的批次，都会
立即重建，因为这条路径就是迭代循环。显式的 *Rebuild DFX* 也不会被拦截：
那是明确要求的。

> **提示：**
>
> 这道闸门最初是依据另一套理论写的——认为危险来自监视器在编辑器关闭期间
> 重放所有变更——而实测否定了它。
> `RegisterDirectoryChangedCallback_Handle` 在注册时就开始监视，且不报告任何积压：
> 编辑器关闭时改动十二个文件，重新打开后**什么都没有发生**。真正造成伤害的批次
> 是在编辑器打开时到达的。修复前用十二个文件实测：54 次
> Niagara 系统编译和八次编译池饱和。修复后：只弹 toast，在你要求之前什么都不构建。

<a id="before-you-commit"></a>

## 提交之前

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

四步，每一步都能抓到其他步骤抓不到的东西：

| 步骤 | 能抓到什么 |
| --- | --- |
| `lint` | 没有边界的 GPU 发射器、没有上限的生成速率、没有种子的随机性 |
| `build` | 任何阻止文本变成资产的问题 |
| `verify` | **源码被编辑并提交却没有重建**——只跑 build 会通过，因为 build 会把它修好 |
| `corpus` | *行为*发生变化而非破坏：诊断移动、拓扑漂移、反编译失去幂等性 |

`verify` 是最值得理解的一个。它把每个资产的溯源戳与其
源文件进行比对；不匹配意味着提交的资产与提交的文本不一致，这在
其他任何方式下都是不可见的。

> **警告：**
>
> **先关闭编辑器。** `build` 和 `corpus` 会写入 `.uasset` 文件，而打开同一项目的编辑器
> 也会写入这些文件——谁后保存谁就赢，而且没有任何提示。该运行时若检测到编辑器进程会发出警告，
> 但它无法判断那个编辑器打开的是哪个项目，所以这个警告只是建议，不是闸门。

<a id="the-shape-of-a-day"></a>

## 一天的形状


<a id="write"></a>

### 编写
在 VSCode 中编辑 `.dfs`（*Tools ▸ DreamFX ▸ Open DreamFX Workspace* 会为你设置好工作区）。
补全和悬停提示来自 `DFX/.dfx-index.json`；在引擎升级后
或启用新的内容插件时，重新运行 `dfx.ps1 index`。

<a id="save"></a>

### 保存
保存即重建。Niagara 预览会刷新。若失败，toast 上的 **Open in VSCode** 链接
会定位到对应行。

<a id="check"></a>

### 检查
当输入名称不确定时，运行 `dfx.ps1 schema <Module> -Stack <Stack>`——它运行
与构建相同的探测，因此由静态开关揭示的输入也会显示出来。

<a id="commit"></a>

### 提交
关闭编辑器，运行 `ci.ps1`，绿了就提交。


<a id="asking-the-coverage-question-of-your-own-content"></a>

## 针对你自己的内容提出覆盖率问题

这个问题不该用别人的数字来回答。自己测量：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff       # L1 text + L2 compile
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff        # facts, independent of the exporter
```

参见[往返与等价](../generation/roundtrip.md)。
