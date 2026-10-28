---
title: "安装与探活"
description: "启用 DreamFX 插件、确认驱动脚本能摸到引擎，以及运行环境的三条硬要求。"
---

DreamFX 是一个 Unreal 插件，装法和别的插件没有区别：把 `DreamFX/` 放进项目的
`Plugins/` 目录（或引擎的 `Engine/Plugins/`），重启编辑器。它依赖
`Niagara`，`.uplugin` 里声明了这条依赖。

| | |
| --- | --- |
| 模块 | `DreamFX`（Runtime）、`DreamFXEditor`（Editor，`LoadingPhase = Default`） |
| 默认启用 | 是（`EnabledByDefault: true`） |
| 依赖插件 | `Niagara` |
| 引擎 | Unreal Engine `5.8` |
| 平台 | Win64（驱动脚本是 PowerShell，`dfx.ps1` 需要 `pwsh`） |

两个模块都是**编辑器侧**的：生成、反编译、诊断都发生在编辑器或 commandlet 进程里。打包出去的游戏
里没有 DreamFX，只有它生成的 Niagara 资产 —— 那些是普通资产，谁都能加载。

<a id="sanity-check-can-the-driver-reach-the-engine"></a>

## 探活：驱动脚本能不能摸到引擎

装完先跑这一条，它比打开编辑器快得多，也更能说明问题：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list
```

它会打印默认搜索路径上的每一个 Niagara 模块。**打印出东西 = 引擎解析成功、commandlet 跑起来了、
模块索引读得到**，三件事一次性验完。

如果它在打印任何东西之前就报错，那是引擎没解析出来。`dfx.ps1` 按这个顺序找引擎：

1. `-Engine <root>` 参数；
2. `UE_ENGINE_ROOT` 环境变量；
3. `.uproject` 的 `EngineAssociation` —— 源码引擎查注册表
   `HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds`，安装版查
   `HKLM:\SOFTWARE\EpicGames\Unreal Engine\<version>`。

项目本身是从目标文件（再是当前工作目录）向上走找出来的，所以在 `DFX/` 里的任何位置
调用驱动脚本都能找对项目；找不到就传 `-Project`。

<a id="confirming-it-editor-side"></a>

## 编辑器侧确认

编辑器起来以后，*Tools ▸ DreamFX* 里应该有三项：**Rebuild DFX**、**Verify DFX**、
**Open DreamFX Workspace (VSCode)**。关卡编辑器工具栏上还有一个 **DFX** 按钮。

没有这些项，只有两种可能：模块没加载，或者进程带了 `-NoDreamFXEditor`。后者是有意为之的开关 ——
它关掉全部交互面（菜单、file watcher、生成资产守卫），但**不**关 commandlet 和解析器。调试 DreamFX
自己的时候用它。

```powershell
UnrealEditor.exe "<Project>.uproject" -NoDreamFXEditor
```

> **警告：**
>
> `-NoDreamFXEditor` 只认这一种拼写 —— 它是按裸命令行参数解析的。

<a id="environment-requirements"></a>

## 运行环境要求

三条，都不是可选项。

<a id="the-engine-api-it-builds-on-is-experimental"></a>

### 引擎依赖的是实验性 API

DreamFX 建在引擎的 Niagara 外部编辑 API（`UNiagaraExternalEditUtilities`）上，这个 API 在引擎侧
标着 EXPERIMENTAL。它漂移了会怎样？**启动自检把漂移显形，而不是静默吸收** —— 这是有意的设计，
一个悄悄降级的生成器比一个停下来明说的生成器危险得多。

<a id="host-projects-must-enable-the-same-content-plugins"></a>

### 宿主项目要启用相同的内容插件

如果创作项目启用了 `NiagaraFluids` 这类**内容**插件，消费这些源码的项目也必须启用。
插件没挂载时模块探针没有基线，引用其模块的源码会以缺口注释或编译错误的形式显形
（[DFX3001](../diagnostics/DFX3xxx.md#dfx3001)）。

一整族解析不了的名字共享同一个前缀 —— `/NiagaraFluids/*` —— 说明的是**挂载点没挂**，不是源码写错了。

<a id="package-writing-commands-need-the-editor-closed"></a>

### 写包的命令要关编辑器

`build`、`corpus`、`mirror-diff`、`decompile-all` 都写 `.uasset` 文件。编辑器开着
同一个项目的时候它也写同一批包，谁后存谁赢，两边都不吭声。`dfx.ps1` 看见有编辑器进程会警告，
但它没法知道那个编辑器开的是哪个项目，所以那是**建议不是门**。

<a id="where-source-lives"></a>

## 目录：源码放哪

源码放在 `DFX/` 下 —— 项目自己的，或者任意插件的：

```
DevTest/DFX/                        Root=""  or  Root="Game"     -> /Game
DevTest/Plugins/MoonToon/DFX/       Root="Plugin.MoonToon"       -> /MoonToon
```

每个 `DFX/` 目录都是一个源码根：被发现、被 watcher 监视、被 `build -All` 收进来。展开讲在
[目录与命名](project-layout.md)。

<a id="next"></a>

## 下一步

  - **[第一个特效](first-effect.md)** — 十五分钟从零到一个能跑的 `NS_Hello`。
  - **[日常循环](workflow.md)** — 保存即重建、批量闸门、提交前的四步 CI。
