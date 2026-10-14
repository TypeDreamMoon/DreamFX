---
title: Export 与 Adopt
description: 两个都做反编译，区别在之后发生什么 —— 镜像命名空间、接管流程，以及三条拒绝的理由。
---

**Export** 和 **Adopt** 都是反编译。它们的区别是接下来会发生什么，而那个区别就是全部要点。

| | **Export .dfs** | **Adopt** |
| :-- | :-- | :-- |
| 写到哪 | `DecompiledOutputDirectory`（默认 `DFX/Decompiled`） | 真正的源码根 —— `<Project>/DFX/…` 或 `<Plugin>/DFX/…` |
| `Name=` 指向 | `Decompiled/<原目录>/<资产>` —— 一个**镜像** | 原资产 |
| 会被构建收进去吗 | 会 —— 存盘即重建它的镜像 | 会 |
| 会改动原资产吗 | **永远不会** | 会：照新源码重建它，并盖上溯源戳 |
| 遇到表达不了的东西 | 警告，并写进文件头 | **拒绝** |
| 含义 | 「让我把它当文本读一读、玩一玩」 | 「从现在起这段文本是它唯一的真相」 |

<a id="让两者分开的是结构不是流程"></a>

## 让两者分开的是结构，不是流程

一次导出**没有能力**命名它来自的那个资产：

```
/Game/FX/NS_X
   └─ export ─> DFX/Decompiled/Game/FX/NS_X.dfs
                Name="Decompiled/FX/NS_X"
                   └─ build ─> /Game/Decompiled/FX/NS_X      (原件毫发无损)
```

所以整棵反编译树就是**普通源码** —— 被监视、存盘即建、被 lint、进 CI —— 而无论你怎么改它，
原资产都碰不到。

不守这条规矩的旧导出（还指着原资产的那些）会被 [DFX8013](../diagnostics/DFX8xxx.md#dfx8013)
**拒绝**而不是执行，重新导出即可替换。

对一个**已经是镜像**的资产再导出也会被拒 —— 那会留下两份源码抢一个资产 ——
toast 会链接到这个镜像是从哪份源码建出来的。

<a id="adopt-的六步"></a>

## Adopt 的六步

<a id="反编译"></a>

### 反编译
有任何表达不了的东西 → **拒绝**，并列出是什么（[DFX8010](../diagnostics/DFX8xxx.md#dfx8010)）。

<a id="按挂载点推出源码路径"></a>

### 按挂载点推出源码路径
`/Game/FX/NS_X` → `<Project>/DFX/FX/NS_X.dfs`；`/MoonToon/FX/NS_X` → `<MoonToon>/DFX/FX/NS_X.dfs`。

<a id="检查有没有别人已经声明了这个资产"></a>

### 检查有没有别人已经声明了这个资产
有 → **拒绝**（[DFX8011](../diagnostics/DFX8xxx.md#dfx8011)）。两份源码交替构建会互相覆盖。

<a id="确认"></a>

### 确认
列出要写的文件和要重建的资产。（Python 侧可以用 `bSkipConfirmation` 跳过这个模态框。）

<a id="写文件重建资产盖戳"></a>

### 写文件、重建资产、盖戳

<a id="重导出逐字节比对"></a>

### 重导出，逐字节比对
对不上是 [DFX8012](../diagnostics/DFX8xxx.md#dfx8012)，并记录第一处差异所在的行。

第 1 步正是第 6 步有意义的原因：既然已知缺口为零，那么一处不匹配就是**真缺陷**，而不是预期内的丢失。

**提示：**

`DFX8012` **不会**把源文件删掉 —— 资产已经照它重建过了，所以文本无论如何都是权威的那一份。
  它记录第一处差异，交给你判断。

<a id="什么时候用哪个"></a>

## 什么时候用哪个

| 想做的事 | 用 |
| --- | --- |
| 读懂一个别人做的特效 | **Export** |
| 拿一个现有特效当模板改出新的 | **Export**，再改 `Name=` 到你自己的命名空间 |
| 把一个特效**迁移**到文本工作流 | **Adopt** |
| 整棵树扫一遍，看能覆盖多少 | `dfx decompile-all` + `mirror-diff` |

**警告：**

Adopt 之后，那个资产就是构建产物了：手改会在下一次重建时被铲掉。这正是你选择 Adopt 时要的东西 ——
  但要确认队伍里所有人都知道。

<a id="从命令行做同一件事"></a>

## 从命令行做同一件事

```bash
# 打印出来看
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion

# 写到文件
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion -Out DFX/Decompiled/NS_Explosion.dfs

# 整棵树
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
```

`-NoDefaults` 会把每一个输入都打印出来，包括与全新模块默认值相同的那些。这是**诊断用**的 ——
产出不适合维护，它回答的是「基线到底藏了什么」。
