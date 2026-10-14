---
title: 目录与命名
description: DFX 源码根、Root 与 Name 怎么解析成资产路径、模块搜索路径，以及哪些东西该进版本控制。
---

<a id="源码根"></a>

## 源码根

一个 `DFX/` 目录就是一个源码根。项目自己有一个，每个插件也可以有自己的：

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

每个根都会被发现、被 file watcher 监视、被 `build -All` 收进来。加一个启用了的插件就多一个根，
不需要注册什么。

<a id="root-与-name"></a>

## `Root` 与 `Name`

文件头的两个参数决定资产落在哪：

```cpp
System(Name="Effects/NS_Spark", Root="Game")     // -> /Game/Effects/NS_Spark
Emitter(Name="Emitters/E_Card", Root="Plugin.MoonToon")  // -> /MoonToon/Emitters/E_Card
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
```

| `Root` | 挂载点 |
| --- | --- |
| `"Game"` 或 `""` | `/Game` |
| `"Plugin.<PluginName>"` | 那个插件的内容根，如 `/MoonToon` |

`Root` 不是一个已挂载的内容根、或者 `Name` 的最后一段是空的，都是
[DFX3000](../diagnostics/DFX3xxx.md#dfx3000)。

**提示：**

**文件放在哪里 ≠ 资产生成到哪里。** 目录只决定它属于哪个源码根；路径由 `Root` + `Name` 决定。
  两者习惯上保持一致（`DFX/Effects/NS_Spark.dfs` 声明 `Name="Effects/NS_Spark"`），但语言不强制 ——
  唯一强制的是 `Decompiled/` 命名空间那条，见下。

<a id="扩展名必须与声明一致"></a>

## 扩展名必须与声明一致

| 文件 | 只能声明 |
| --- | --- |
| `.dfs` | `System` |
| `.dfe` | `Emitter` |
| `.dfm` | `Module` 或 `DynamicInput` |

不一致是 [DFX2021](../diagnostics/DFX2xxx.md#dfx2021)，报错里带上应该改成的文件名。
一个文件里只能有一个顶层对象，后面还有内容是 [DFX2022](../diagnostics/DFX2xxx.md#dfx2022)。

<a id="from-的路径怎么找"></a>

## `from` 的路径怎么找

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard" { ... }
```

先相对**引用它的文件**解析，再依次相对每一个 DFX 根解析。扩展名可以省略。

<a id="模块搜索路径"></a>

## 模块搜索路径

模块短名（`GravityForce`）在搜索路径上解析。引擎默认的三条永远在：

```
/Niagara/Modules      /Niagara/DynamicInputs      /Niagara/Functions
```

自己的模块目录用系统 `Settings` 里的 `ModulePaths` **追加**（不是替换）：

```cpp
Settings = {
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

短名撞车的时候写长一点的路径 —— `Spawn/Initialization/V2/InitializeParticle` 这种偏路径就够了，
不必写全资产路径。找不到是 [DFX3001](../diagnostics/DFX3xxx.md#dfx3001)；找到了但没这个输入是
[DFX3003](../diagnostics/DFX3xxx.md#dfx3003) —— 两种拼写错误分开报，否则它们读起来一模一样。

<a id="反编译输出目录"></a>

## 反编译输出目录

*Export .dfs* / *Export .dfe* 写到 **Decompiled Output Directory**（默认 `DFX/Decompiled`，
在 *Project Settings ▸ Plugins ▸ DreamFX* 里改）。

这个目录里的文件是**一等源码**：被监视、被构建、被 lint、进 CI。区别只有一条 —— 它们必须命名到
`Decompiled/` 命名空间里：

```
/Game/FX/NS_X  ──export──>  DFX/Decompiled/Game/FX/NS_X.dfs
                            Name="Decompiled/FX/NS_X"
                            ──build──>  /Game/Decompiled/FX/NS_X
```

于是导出出来的东西**结构上碰不到原资产**，随便改都只影响镜像。不守这条规矩的旧导出会被
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) 拒绝，重新导出即可。展开讲在
[Export 与 Adopt](../generation/adopt.md)。

<a id="两个生成出来的文件"></a>

## 两个生成出来的文件

| 文件 | 谁写的 | 干什么 |
| --- | --- | --- |
| `DFX/DreamFX.code-workspace` | *Open DreamFX Workspace* | 每个源码根一个 folder 的 VSCode workspace。**每次整体重写，不合并** —— 个人配置放 `DFX/.vscode/` |
| `DFX/.dfx-index.json` | `dfx.ps1 index` | 搜索路径上每个模块/动态输入的栈、分类、描述与**输入签名**，编辑器扩展读它做补全 |

索引是命令而不是自动的：它一次要 boot 引擎，带输入探测在本机测得 571 个模块 8.1 秒
（不探测 2.9 秒）。见 [VSCode 与模块索引](../tools/vscode.md)。

<a id="什么该进版本控制"></a>

## 什么该进版本控制

**每个 `.dfs` / `.dfe` / `.dfm` 都进。** 它们是源码。

生成的 `.uasset` 呢？两种做法都成立，取决于队伍：

- **不提交** —— 需要的人自己 `build -All`。树最干净，但每个人都得能跑生成。
- **提交** —— 拿到就能用，`ci.ps1` 的 `verify` 步会盯着「源码改了资产没重建」这件事。
  预编译引擎上跑不了 `.dfm` 生成的队伍**必须**提交模块资产，否则消费方拿不到脚本
  （[DFX5100](../diagnostics/DFX5xxx.md#dfx5100) / [DFX5107](../diagnostics/DFX5xxx.md#dfx5107)）。

不该提交的是 `DFX/.dfx-index.json`：它记录着引擎路径和启用插件列表，换台机器就不对了。
