---
title: "Export 与 Adopt"
description: "两者都会反编译；区别在于之后发生的事 —— 镜像命名空间、接管流程，以及三条被拒绝的理由。"
---

**Export** 与 **Adopt** 都会反编译。它们的区别在于之后会发生什么，而这个区别正是全部意义所在。

| | **Export .dfs** | **Adopt** |
| :-- | :-- | :-- |
| 写入位置 | `DecompiledOutputDirectory`（默认 `DFX/Decompiled`） | 真正的源码根 —— `<Project>/DFX/…` 或 `<Plugin>/DFX/…` |
| `Name=` 名称 | `Decompiled/<original directory>/<asset>` —— 一个**镜像** | 原始资产 |
| 是否被构建拾取 | 是 —— 保存其中一个会重建它的镜像 | 是 |
| 是否修改资产 | **从不** | 是：根据新源码重建它并打上来源标记 |
| 无法表示的特性 | 警告，并在文件中列出它们 | **拒绝** |
| 含义 | “让我把它当作文本来阅读和试验” | “从今往后，这段文本是唯一的真相” |

<a id="what-keeps-them-apart-is-structural-not-procedural"></a>

## 让两者分开的是结构，而非流程

一次导出**无法命名**它来自的资产：

```
/Game/FX/NS_X
   └─ export ─> DFX/Decompiled/Game/FX/NS_X.dfs
                Name="Decompiled/FX/NS_X"
                   └─ build ─> /Game/Decompiled/FX/NS_X      (the original is untouched)
```

所以整棵反编译树都是**普通源码** —— 被监视、保存即构建、被 lint 并进入 CI —— 而无论文件如何被编辑，原始资产都保持不变。

在此约定之前写出的导出仍然会命名原始资产。它们会被以
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) **拒绝**，而不是被执行；重新导出即可替换它们。

对一个*已经是*镜像的资产执行导出同样会被拒绝 —— 那会留下两份源码争夺一个资产 —— 并且提示会链接到该镜像所基于的源码。

<a id="adopt-in-six-steps"></a>

## Adopt 的六个步骤


<a id="decompile"></a>

### 反编译
如果有任何无法表示的内容 → **拒绝**，并列出是什么
（[DFX8010](../diagnostics/DFX8xxx.md#dfx8010)）。

<a id="work-out-the-source-path-from-the-mount-point"></a>

### 根据挂载点推导出源码路径
`/Game/FX/NS_X` → `<Project>/DFX/FX/NS_X.dfs`；`/MoonToon/FX/NS_X` → `<MoonToon>/DFX/FX/NS_X.dfs`。

<a id="check-nothing-else-already-declares-this-asset"></a>

### 检查是否已有其他东西声明了此资产
如果有 → **拒绝**（[DFX8011](../diagnostics/DFX8xxx.md#dfx8011)）。两份源码会在交替构建时互相覆盖。

<a id="confirm"></a>

### 确认
列出要写入的文件和要重建的资产。（在 Python 中，`bSkipConfirmation` 会抑制该模态框。）

<a id="write-the-file-rebuild-the-asset-stamp-provenance"></a>

### 写入文件、重建资产、打上来源标记

<a id="re-decompile-and-compare-byte-for-byte"></a>

### 重新反编译并逐字节比较
不一致即为 [DFX8012](../diagnostics/DFX8xxx.md#dfx8012)，并记录第一处不同的行。


第 1 步正是第 6 步有意义的原因：在没有已知缺口的情况下，不一致是**真正的缺陷**，
而不是预期内的丢失。

> **注意：**
>
> `DFX8012` 会保留源码文件 —— 资产已经根据它重建，所以文本无论如何都是权威的 ——
> 并记录第一处不同的行供你判断。

<a id="which-one-to-use"></a>

## 该用哪一个

| 目标 | 使用 |
| --- | --- |
| 理解别人做的特效 | **Export** |
| 将现有特效作为新特效的模板 | **Export**，然后把 `Name=` 改成你自己的命名空间 |
| 将特效**迁移**到文本工作流 | **Adopt** |
| 扫描整棵树以查看覆盖了多少 | `dfx decompile-all` + `mirror-diff` |

> **警告：**
>
> Adopt 之后，该资产就是构建产物：手动编辑会在下次重建时被抹掉。这正是你选择采用 Adopt 所要求的 ——
> 只要确保团队知晓即可。

<a id="the-same-thing-from-the-command-line"></a>

## 从命令行做同一件事

```bash
# print it
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion

# write it to a file
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion -Out DFX/Decompiled/NS_Explosion.dfs

# a whole tree
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
```

`-NoDefaults` 会打印每一个输入，包括与全新模块默认值相等的那些。这**仅用于诊断** ——
其结果并非旨在维护；它回答的是“基线隐藏了什么？”。
