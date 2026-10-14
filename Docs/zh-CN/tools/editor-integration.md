---
title: 编辑器集成
description: Tools 菜单、关卡工具栏、Content Browser 右键、Niagara 编辑器工具栏、toast、file watcher 与项目设置。
---

DreamFX 挂在编辑器 UI 上的每一个地方。

| | |
| :-- | :-- |
| 注册者 | `DreamFXEditor` 模块启动时，经 `UToolMenus::RegisterStartupCallback` |
| ToolMenu owner | `DreamFXEditor` |
| 关闭方式 | `-NoDreamFXEditor`，以及任何 commandlet 运行 |
| 设置 | *Project Settings ▸ Plugins ▸ DreamFX* |

注册是幂等的（跑第二遍什么都不加），编辑器关闭过程中会跳过。

**提示：**

**每一个条目调用的都是 commandlet 调用的同一份代码。** 菜单是入口，不是第二套实现：
  *Rebuild DFX* 能用，`dfx build -All` 就能用，反之亦然。

<a id="tools-菜单"></a>

## Tools 菜单

*Tools ▸ DreamFX*，扩展 `LevelEditor.MainMenu.Tools` 的 `DreamFX` 分区。

| 标签 | 作用 |
| :-- | :-- |
| **Rebuild DFX** | 把每个 `.dfs` / `.dfe` / `.dfm` 压进 watcher 的构建队列，就像它们各自被保存过一样。队列排空后一条汇总 toast |
| **Verify DFX** | 对照生成资产校验每个源码，什么都不写。一条 toast：干净，或者一个指向 Output Log 的漂移计数 |
| **Open DreamFX Workspace (VSCode)** | 按当前源码根重写 `DFX/DreamFX.code-workspace` 并启动它 |

关卡编辑器工具栏上有两个同样的按钮：**DFX**（= Rebuild）和 workspace。

<a id="content-browser-右键"></a>

## Content Browser 右键

加在每类资产上下文菜单原有的 `GetAssetActions` 分区里。

**警告：**

**每一个 DreamFX 右键条目都要求恰好选中一个资产。** 选了两个或更多，这个分区是空的，
  而且不会有任何解释。右键单个资产。

<a id="niagara-system"></a>

### Niagara System

内容取决于这个资产有没有 DreamFX [溯源戳](../generation/provenance.md) —— 生成出来的资产和
导入进来的资产需要的是相反的两件事。

**没有溯源戳**（DreamFX 没做过它），*Decompiler* 分区：

| 条目 | 作用 |
| :-- | :-- |
| **Export .dfs** | 反编译到 `<DecompiledOutputDirectory>/<包路径>.dfs` 并打开它。这份文件重建的是 `Decompiled/` 下的镜像，**原资产永不修改** |
| **Adopt (take over as a DFX source)** | 见 [Export 与 Adopt](../generation/adopt.md) |

**有溯源戳**，*Source* 分区：

| 条目 | 作用 |
| :-- | :-- |
| **Open Source** | 打开戳里记的那个 `.dfs` |
| **Rebuild from Source** | 只把那一个文件排进队列 |
| **Verify** | 拿这个资产对照它的源码检查 |

<a id="niagara-emitter"></a>

### Niagara Emitter

只有一个条目 —— **Export .dfe**。一个独立 emitter 没有往返：`.dfe` 自身不产资产，
所以没有东西可以盖戳、重建或校验。

导出会把 emitter 拷进一个一次性的 `/Temp` 宿主系统，在那里读，再写文件。这一步是它诚实而不是
猜测的原因：外部编辑 API 的每个读取器都通过一个拥有它的系统寻址。

<a id="niagara-系统编辑器工具栏"></a>

### Niagara 系统编辑器工具栏

`AssetEditor.Niagara.ToolBar` 上的一个 **DreamFX** 组合按钮，内容与上面的 System 子菜单相同。

**提示：**

`AssetEditor.Niagara.ToolBar` 是共享的：`FNiagaraSystemToolkit`、`FNiagaraSimCacheToolkit`、
  `FNiagaraParameterDefinitionsToolkit`、`FNiagaraParameterCollectionToolkit` 和
  `FNiagaraStatelessEmitterTemplateToolkit` 返回的 toolkit 名字都是 `"Niagara"`。
  所以这个条目先在正在编辑的对象里找 `UNiagaraSystem`，找不到就什么都不加 ——
  打开一个 Sim Cache 是看不到 DreamFX 按钮的。

<a id="toast"></a>

## toast

| toast | 条件 |
| --- | --- |
| `DreamFX: {Built} built, {Skipped} up to date.` | 没有失败 |
| `DreamFX build failed: {FirstError}` + **Open in VSCode** | 有失败。链接跳到第一处错误的文件、行、列 |
| `DreamFX found no .dfs/.dfe/.dfm sources to rebuild.` | 一个源码都没有 |
| `DreamFX: {N} source(s) verified, all assets in step.` | verify 干净 |
| `DreamFX: {Drifted} of {N} source(s) out of step ({Failed} failed).` | verify 有漂移 |

诊断一直带着位置，在这之前它只到得了日志。

<a id="file-watcher"></a>

## file watcher

每个 `DFX/` 根都被监视。存盘 → 排进队列 → 防抖 ticker 按**模块 → emitter → 系统**的顺序建，
和 commandlet 一致；`.dfm` 和调用它的 `.dfs` 一起排队时，这个顺序是承重的。

批次超过 **Bulk Rebuild Threshold**（默认 8）会改成询问而不是直接建 ——
理由与测量见[日常循环](../getting-started/workflow.md)。

<a id="设置"></a>

## 设置

*Project Settings ▸ Plugins ▸ DreamFX*，存在 `Config/DefaultEditor.ini`。

| 设置 | 默认 | 作用 |
| --- | --- | --- |
| **Open Workspace In New Window** | `true` | 只有 workspace 启动器读它 |
| **Decompiled Output Directory** | `DFX/Decompiled` | *Export .dfs* / *Export .dfe* 写到哪（相对项目目录）。这里的文件是普通源码：被发现、被监视、被构建，但必须命名到 `Decompiled/` 命名空间（[DFX8013](../diagnostics/DFX8xxx.md#dfx8013)） |
| **Bulk Rebuild Threshold** | `8` | 超过这个数量的一批改动改成询问 |

<a id="关掉整套集成"></a>

## 关掉整套集成

```powershell
UnrealEditor.exe "<Project>.uproject" -NoDreamFXEditor
```

| 关掉的 | 仍然有效的 |
| --- | --- |
| 本页所有菜单、工具栏、右键条目 | `-run=DreamFX` commandlet（它从来不用这些） |
| source watcher 与存盘即重建 | 解析器、生成器、反编译器、设置对象 |
| 生成资产守卫 | 已经生成并存在磁盘上的资产 |

commandlet 运行会经另一条路到达同样的状态：`IsRunningCommandlet()` 为真，模块什么都不注册。
