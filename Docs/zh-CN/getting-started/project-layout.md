---
title: "布局与命名"
description: "DFX 源码根、Root 与 Name 如何解析为资产路径、模块搜索路径，以及哪些内容应纳入版本控制。"
---

<a id="source-roots"></a>

## 源码根

一个 `DFX/` 目录就是一个源码根。项目有一个，每个插件也可以有自己的：

```text
DevTest/
  DFX/
    Effects/
      NS_Hello.dfs
    Emitters/
      E_MoonFlashCard.dfe
    Modules/
      ToonSpin.dfm
    Decompiled/
    DreamFX.code-workspace
    .dfx-index.json
  Plugins/
    MoonToon/
      DFX/
```

每个根都会被发现有、被文件监视器监视、并被 `build -All` 扫描收录。多启用一个插件就多一个根；
无需注册任何东西。

<a id="root-and-name"></a>

## `Root` 与 `Name`

两个头部参数决定资产落在哪里：

```cpp
System(Name="Effects/NS_Spark", Root="Game")     // -> /Game/Effects/NS_Spark
Emitter(Name="Emitters/E_Card", Root="Plugin.MoonToon")  // -> /MoonToon/Emitters/E_Card
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
```

| `Root` | 挂载点 |
| --- | --- |
| `"Game"` 或 `""` | `/Game` |
| `"Plugin.<PluginName>"` | 该插件的内容根，例如 `/MoonToon` |

一个 `Root` 如果未指向任何已挂载的内容根，或者一个 `Name` 在最后一个斜杠之后没有任何内容，都是
[DFX3000](../diagnostics/DFX3xxx.md#dfx3000)。

> **注意：**
>
> **文件所在的位置并不等于资产去往的位置。** 目录只决定哪个源码根
> 拥有该文件；资产路径来自 `Root` + `Name`。保持二者平行是约定
> （`DFX/Effects/NS_Spark.dfs` 声明 `Name="Effects/NS_Spark"`），但语言本身并不要求
> 如此——唯一要求的是下面的 `Decompiled/` 命名空间。

<a id="the-extension-has-to-match-the-declaration"></a>

## 扩展名必须与声明匹配

| 文件 | 可声明 |
| --- | --- |
| `.dfs` | `System` |
| `.dfe` | `Emitter` |
| `.dfm` | `Module` 或 `DynamicInput` |

不匹配就是 [DFX2021](../diagnostics/DFX2xxx.md#dfx2021)，并且消息会给出要重命名成的
文件名。一个文件只能包含一个顶层对象；其后的任何内容都是
[DFX2022](../diagnostics/DFX2xxx.md#dfx2022)。

<a id="how-a-from-path-resolves"></a>

## `from` 路径如何解析

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard" { ... }
```

先相对于**引用文件**解析，然后依次对每个 DFX 根解析。扩展名
可选。

<a id="module-search-paths"></a>

## 模块搜索路径

短模块名（`GravityForce`）在搜索路径上解析。引擎的三个默认路径
始终在列表中：

```
/Niagara/Modules      /Niagara/DynamicInputs      /Niagara/Functions
```

你自己的模块文件夹通过系统 `Settings` 中的 `ModulePaths` **追加**（而非替换）：

```cpp
Settings = {
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

当短名称有歧义时，写一个更长的路径——像
`Spawn/Initialization/V2/InitializeParticle` 这样的部分路径就够了，很少需要完整资产路径。什么都没找到是
[DFX3001](../diagnostics/DFX3xxx.md#dfx3001)；找到了但没有这样的输入是
[DFX3003](../diagnostics/DFX3xxx.md#dfx3003)——这两种笔误会分别报告，因为否则
它们读起来一模一样。

<a id="the-decompiled-output-directory"></a>

## 反编译输出目录

*Export .dfs* / *Export .dfe* 写入 **Decompiled Output Directory**（默认
`DFX/Decompiled`，在 *Project Settings ▸ Plugins ▸ DreamFX* 中更改）。

那里的文件是**一等源码**：会被监视、构建、lint、进 CI。有一条规则适用于它们，
而不适用于其他任何文件——它们必须命名到 `Decompiled/` 命名空间中：

```
/Game/FX/NS_X  ──export──>  DFX/Decompiled/Game/FX/NS_X.dfs
                            Name="Decompiled/FX/NS_X"
                            ──build──>  /Game/Decompiled/FX/NS_X
```

因此，导出内容**在结构上不可能触及原始资产**，无论怎么编辑。早于该安排的旧导出会被
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) 拒绝，而不是照常执行；重新导出即可替换它们。完整
说明见 [Export vs Adopt](../generation/adopt.md)。

<a id="two-generated-files"></a>

## 两个生成文件

| 文件 | 由谁写入 | 用途 |
| --- | --- | --- |
| `DFX/DreamFX.code-workspace` | *Open DreamFX Workspace* | 一个 VSCode 工作区，每个源码根对应一个文件夹。**每次完全重写，从不合并**——将个人设置保留在 `DFX/.vscode/` 中 |
| `DFX/.dfx-index.json` | `dfx.ps1 index` | 搜索路径上的每个模块和动态输入，及其栈、分类、描述和**输入签名**；编辑器扩展读取它以实现补全 |

索引是一个命令，而不是自动的东西：它会启动引擎，在本机实测中，571 个模块带输入探测耗时 8.1 秒
（不带探测为 2.9 秒）。见
[VSCode and the module index](../tools/vscode.md)。

<a id="what-belongs-in-version-control"></a>

## 哪些内容应纳入版本控制

**每个 `.dfs`、`.dfe` 和 `.dfm`。** 它们就是源码。

生成的 `.uasset` 文件是团队决定，两种答案都可行：

- **不提交它们**——需要的人运行 `build -All`。树最干净，但每个人都
  必须能够生成。
- **提交它们**——检出后即可使用，并且 `ci.ps1` 中的 `verify` 会监视源码被
  编辑但未重建的情况。在无法生成 `.dfm` 图的引擎上工作的团队**必须**提交
  模块资产，否则使用者根本没有脚本
  （[DFX5100](../diagnostics/DFX5xxx.md#dfx5100) / [DFX5107](../diagnostics/DFX5xxx.md#dfx5107)）。

唯一应排除的文件是 `DFX/.dfx-index.json`：它记录引擎路径和
已启用插件列表，所以在下一台机器上就是错的。
