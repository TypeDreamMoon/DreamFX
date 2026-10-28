---
title: "编辑器集成"
description: "编辑器集成"
---

<a id="editor-integration"></a>

# 编辑器集成

> [DreamFX](../../../README.md) » **编辑器集成**

DreamFX 挂接到 Unreal 编辑器 UI 的每一处：Tools 菜单、关卡编辑器工具栏、Content Browser 资产上下文菜单，以及 Niagara 系统与脚本编辑器工具栏。

| | |
| :-- | :-- |
| 注册者 | 模块启动时的 `DreamFXEditor`，经由 `UToolMenus::RegisterStartupCallback` |
| ToolMenu owner | `DreamFXEditor` |
| 关闭方式 | `-NoDreamFXEditor`，以及任何 commandlet 运行 |
| 设置 | *Project Settings ▸ Plugins ▸ DreamFX* |

注册是幂等的——跑第二遍什么都不加——并且在编辑器关闭过程中会跳过。**每一个条目调用的都是 commandlet 调用的同一份代码。** 菜单是入口，从来不是第二套实现：如果 *Rebuild DFX* 能用，`dfx build -All` 就能用，反之亦然。

<a id="tools-menu"></a>

## Tools 菜单

*Tools ▸ DreamFX*，扩展 `LevelEditor.MainMenu.Tools`，分区 `DreamFX`。

| 条目名 | 标签 | 图标 | 作用 |
| :-- | :-- | :-- | :-- |
| `DreamFX.RebuildAll` | **Rebuild DFX** | `Icons.Refresh` | 把每个 `.dfs` / `.dfe` / `.dfm` 压进 watcher 的构建队列，就像它们各自被保存过一样。队列排空后一条汇总 toast |
| `DreamFX.VerifyAll` | **Verify DFX** | `Icons.Adjust` | 对照生成资产校验每个源码，什么都不写。一条 toast：干净，或者一个指向 Output Log 的漂移计数 |
| `DreamFX.OpenWorkspace` | **Open DreamFX Workspace (VSCode)** | `Icons.OpenInExternalEditor` | 按当前源码根重写 `DFX/DreamFX.code-workspace`，然后启动它 |

<a id="level-editor-toolbar"></a>

## 关卡编辑器工具栏

扩展 `LevelEditor.LevelEditorToolBar.AssetsToolBar`，分区 `DreamFX`。

| 条目名 | 标签 | 图标 | 作用 |
| :-- | :-- | :-- | :-- |
| `DreamFX.RebuildAllToolbar` | **DFX** | `Icons.Refresh` | 与 *Rebuild DFX* 完全相同 |
| `DreamFX.OpenWorkspaceToolbar` | **Open DreamFX Workspace (VSCode)** | `Icons.OpenInExternalEditor` | 与 *Open DreamFX Workspace* 完全相同 |

<a id="content-browser-context-menus"></a>

## Content Browser 上下文菜单

加在每类资产上下文菜单原有的 `GetAssetActions` 分区里。

> [!WARNING]
> **每一个 DreamFX 上下文菜单条目都要求恰好选中一个资产。** 选了两个或更多时，这个分区是空的，而且不会有任何解释。右键单个资产。

| 资产类 | 动态条目 | 出现的内容 |
| :-- | :-- | :-- |
| `UNiagaraSystem` | `DreamFX.SystemAssetActions` | 子菜单 **DreamFX** —— [System 子菜单](#system-submenu) |
| `UNiagaraEmitter` | `DreamFX.EmitterAssetActions` | 子菜单 **DreamFX** —— [Emitter 子菜单](#emitter-submenu) |
| `UNiagaraScript` | `DreamFX.ScriptAssetActions` | 子菜单 **DreamFX** —— [Script 子菜单](#script-submenu)，仅在 DreamFX 生成的脚本上 |

<a id="system-submenu"></a>

### System 子菜单

`DreamFX.SystemActions` —— 标签 **DreamFX**，图标 `Icons.Settings`。它的内容取决于该资产是否带有 DreamFX [溯源戳](../../../README.md)，因为生成出来的资产和导入进来的资产需要的是相反的两件事。

**没有溯源戳**（DreamFX 没做过它），分区 `DreamFX.DecompileActions`，标题为 *Decompiler*：

| 条目 | 标签 | 图标 | 作用 |
| :-- | :-- | :-- | :-- |
| `DreamFX.ExportDfs` | **Export .dfs** | `Icons.Save` | 反编译到 `<DecompiledOutputDirectory>/<package path>.dfs` 并打开它。这份文件重建的是 `Decompiled/` 下的镜像；原资产永不修改 |
| `DreamFX.Adopt` | **Adopt (take over as a DFX source)** | `Icons.Import` | [见下文](#adopt) |

**有溯源戳**，分区 `DreamFX.SourceActions`，标题为 *Source*：

| 条目 | 标签 | 图标 | 作用 |
| :-- | :-- | :-- | :-- |
| `DreamFX.OpenSource` | **Open Source** | `Icons.OpenInExternalEditor` | 打开戳里记的那个 `.dfs` |
| `DreamFX.RebuildFromSource` | **Rebuild from Source** | `Icons.Refresh` | 只把那一个文件排进队列并立即构建它 —— [见下文](#rebuild-from-source) |
| `DreamFX.PullIntoSource` | **Write Back to Source** | `Icons.Save` | 把资产里调好的值写回它那个 `.dfs` 的字面量 —— [见下文](#write-back-to-source) |
| `DreamFX.VerifyAsset` | **Verify** | `Icons.Adjust` | 拿这个资产对照它的源码检查 |

<a id="emitter-submenu"></a>

### Emitter 子菜单

`DreamFX.EmitterActions` —— 只有一个条目，因为一个独立 emitter 没有往返：一个 `.dfe` 自身不产资产，所以没有东西可以盖戳、重建或校验。

| 条目 | 标签 | 图标 | 作用 |
| :-- | :-- | :-- | :-- |
| `DreamFX.ExportDfe` | **Export .dfe** | `Icons.Save` | 把 emitter 拷进一个一次性的 `/Temp` 宿主系统，在那里读它，再写 `<DecompiledOutputDirectory>/<package path>.dfe`，像 *Export .dfs* 一样命名到 `Decompiled/` 命名空间 |

正是这个宿主系统让这一步是诚实的而不是猜测：Niagara 外部编辑 API 的每个读取器都通过一个拥有它的系统寻址，所以拿回来的就是系统使用它时这个 emitter 所贡献的东西。

<a id="script-submenu"></a>

### Script 子菜单

`DreamFX.ScriptActions` —— 标签 **DreamFX**，图标 `Icons.Settings`。用于独立的 `UNiagaraScript` 资产——也就是 `.dfm` 构建的那种资产。只有 Source 状态存在，因为脚本没有反编译器：这个子菜单仅在资产带有溯源戳时才出现，所以原版模块或手工做的脚本完全不会有 DreamFX 条目，而且它针对戳里记的 `.dfm` 提供 System 菜单的那三条 Source 命令——**Open Source**、**Rebuild from Source**、**Verify**。

<a id="niagara-system-editor-toolbar"></a>

## Niagara 系统编辑器工具栏

`DreamFX.SystemEditorToolbarActions`，是 `AssetEditor.Niagara.ToolBar` 的 `DreamFX` 分区里的一个动态条目。加一个标签为 **DreamFX** 的组合按钮，其内容与上面的双状态 [System 子菜单](#system-submenu)相同。

> [!NOTE]
> `AssetEditor.Niagara.ToolBar` 是共享的。`FNiagaraSystemToolkit`、`FNiagaraSimCacheToolkit`、`FNiagaraParameterDefinitionsToolkit`、`FNiagaraParameterCollectionToolkit` 和 `FNiagaraStatelessEmitterTemplateToolkit` 返回的 toolkit 名字都是 `"Niagara"`。所以这个条目先在正在编辑的对象里找 `UNiagaraSystem`，找不到就什么都不加——打开一个 Sim Cache 是看不到 DreamFX 按钮的。

<a id="niagara-script-editor-toolbar"></a>

## Niagara 脚本编辑器工具栏

`DreamFX.ScriptEditorToolbarActions`，是 `AssetEditor.NiagaraScriptEditor.ToolBar` 的 `DreamFX` 分区里的一个动态条目——这就是独立 `UNiagaraScript` 打开的编辑器。加一个标签为 **DreamFX** 的组合按钮，其内容是 [Script 子菜单](#script-submenu)，而且仅在脚本带有溯源戳时才加：同一个编辑器会打开每一个原版模块和动态输入，它们在那菜单里都没有任何东西。

<a id="command-semantics"></a>

## 命令语义

<a id="rebuild-dfx"></a>

### Rebuild DFX

先让源码根缓存失效（这样新启用的插件才会被捡到），再把每个源码文件盖戳进 watcher 的待处理队列。防抖 ticker 按模块在前、然后是 emitter、最后是系统的顺序构建它们——和 commandlet 一致的顺序，而且当一个 `.dfm` 和调用它的 `.dfs` 一起排队时，这个顺序是承重的。

反编译导出的文件和其他源码一样会被包含。见 [Export 与 Adopt](#export-vs-adopt)。

*Rebuild DFX* 永远是全量重建：它是从保存所用的[增量构建](#incremental-build-on-save)里逃出来的出口。

watcher 还会跟随 DFX 根之外的 `.dfs` `Parent` 链和发射器 `from` 引用。只有 DFX 根会被递归监视。每个根所在的目录、每个外部引用所在的目录以及该目录的父目录采用浅层监视——只监视直接子项——这样根或依赖目录被重命名、删除或替换时能被发现，而无需接收项目下的每一次写入（`Saved/`、`Intermediate/`、`DerivedDataCache/`）。缺失的引用通过其最近的现有祖先被监视，因此创建缺失的父目录或发射器源会恢复重建。目录移动、重命名、删除和重新扫描事件会使先前的依赖图和新发现的源都失效；一个被替换掉的被监视目录会重新订阅。外部源会触发其在根内的依赖项；它们不会仅仅因为被监视而获得独立构建目标。

源的位置是它的 DFX 根加上它在该根内的路径。移动或重命名一个源而不改变其内容，会使普通构建重新生成资产并更新其溯源。同一棵树在另一个绝对路径下签出——队友的机器、CI、测试主机——仍是相同位置，因此不会重建；从戳记打开或重建的命令会先解析记录的绝对路径，再解析相对于根的路径。Verify 始终是只读的。

| Toast | 条件 |
| :-- | :-- |
| `DreamFX: {Built} built, {Skipped} up to date.` | 没有失败 |
| `DreamFX build failed: {FirstError}` + 一个 **Open in VSCode** 链接 | 一个或多个失败 |
| `DreamFX found no .dfs/.dfe/.dfm sources to rebuild.` | 完全没有源 |

**Open in VSCode** 链接会跳转到第一个错误的文件、行和列。诊断信息一直携带着位置；在此之前它只到达日志。

<a id="rebuild-from-source"></a>

### 从源重建

将资产戳记中指定的源排入保存所使用的同一个 watcher 队列，并立即排空队列，而不是在防抖之后。因此它和保存是完全相同的强制重建，具有与 *Rebuild DFX* 相同的 toast，并且在保存文件会失败的情况下它也无法成功。它总是完整重建，从不增量，因此对于保存后看起来不对的资产，它也是获得参考答案的方式。

一个编辑器处于打开状态的独立脚本（`.dfm`）会多一个步骤。Niagara 脚本编辑器编辑的是资产的副本，并在 *Apply* 时复制回来，因此在其下重建不会显示出来，而下一次 *Apply* 会覆盖它。编辑器会先被关闭——它自己的提示决定尚未应用的编辑如何处理——并在构建后重新打开，无论构建是否成功。该提示上的 *Cancel* 会保留编辑器并跳过重建，并显示一个说明的 toast。编辑器未打开的脚本会在不打开编辑器的情况下重建。系统编辑器操作的是资产本身，会保持打开。

<a id="incremental-build-on-save"></a>

### 保存时增量构建

一次保存不会清除并重写整个系统。每次成功构建都会在资产的包元数据中存储一个 *计划摘要*（`DreamFX.PlanDigest`，紧邻溯源戳记），watcher 的下一次构建会将新计划与其比较，只写入差异：更改的参数值、模块发生变化的栈、新增的发射器——仅此而已。在一个 1230 行的系统上，单值编辑大约需要一秒，而在 MoonEngine 构建上完整重建需要 17 秒，在预编译的 UE 5.8 上需要 56 秒。

它由 *Project Settings ▸ Plugins ▸ DreamFX ▸ Workspace ▸ **Incremental Build on Save***（`bIncrementalBuildOnSave`，默认开启）控制，并且仅适用于 watcher 的保存触发重建。*Rebuild DFX*、*Rebuild from Source* 以及批量批处理的 **Rebuild them now** 链接始终完整重建。

只要摘要不可信——新资产、在 DreamFX 之外编辑过的资产、较旧 DreamFX 的构建、发射器重新排序——构建就会自动回退到完整重建，Output Log 会说明原因，每个文件一行：

```text
incremental: 0 emitter(s) and 1 stack(s) rebuilt, 3 input(s) written, 1 reset, nothing to recompile
full rebuild (the asset changed since its last build)
```

结果必须与对相同文本进行完整重建所生成的资产相同。每次编辑的代价、每个回退原因以及该保证如何被测试，都在[增量构建](incremental-builds.md)中。

<a id="write-back-to-source"></a>

### 写回源

[`dfx pull -Apply`](pull.md) 的编辑器侧入口：资产存储的值——Niagara 编辑器中的滑块所留下的值——会回到其 `.dfs` 已声明的字面量中，这样下一次重建就不会覆盖它们。它位于[系统子菜单](#system-submenu)中，Content Browser 和 Niagara 系统编辑器工具栏都会显示该菜单，并且对任何带有溯源戳记的系统都可用。源的解析方式与 *Open Source* 和 *Rebuild from Source* 完全相同，都是从戳记解析。

只替换字面量自身的字节，因此注释、缩进和行尾保持不变。自上次写回以来文本已更改的值会被保留（文本更新），文本未声明的值会被报告且永不添加，而资产与文本对栈的形状有分歧的栈会被整体拒绝。toast 会给出计数——已写入、已保留、未声明、已拒绝的栈，以及添加或删除的语句——其链接会打开 `Saved/DreamFX/Pull/` 下的报告文件。相同的数字会连同拉取的 `DFX71xx` 诊断一起进入 Output Log。打开资产中未保存的更改也会被包含：拉取读取的是内存中的资产。

两个设置，都在 *Project Settings ▸ Plugins ▸ DreamFX ▸ Workspace* 下，且默认都关闭：

- **Write Back Structural Edits**（`bPullStructuralEdits`）——还会添加和删除语句。关闭时，编辑器添加或删除的模块会被报告（`DFX7113`），整个栈保持不变；开启时它会被写入，但仅在资产与文本之间的对应关系唯一时（[`pull.md` §4d](pull.md)）。该命令和下面的保存钩子都会读取它。
- **Write Back to Source on Asset Save**（`bPullOnAssetSave`）——每当作者保存生成的系统时运行相同的写回。保存资产是触发条件，工作在下一次编辑器 tick 中完成，绝不在保存内部完成，并且仅在写入了某些内容或有栈被拒绝时才显示 toast。

保存时写回只会从作者自己的保存启动。它不会为 DreamFX 自己进行的保存启动：每次构建都会以保存其生成的系统结束，而在此之后的拉取会把构建刚刚应用的值写回构建刚刚读取的文件。生成器的保存会经过一个函数，该函数在运行时打开一个作用域；当作用域打开时，钩子会拒绝（并且包保存事件会在保存内部同步触发，因此该作用域恰好覆盖那次保存）。它也会对自动保存、cook 或任何程序化保存、commandlet 运行、自动化运行以及编辑器播放时拒绝。

**循环防护。** 写回会重写源 watcher 正在监视的文件，而 watcher 无法将其与作者的保存区分开：它会重建这些值刚刚来自的资产。因此命令会告诉 watcher 它写入了什么——`FSourceWatcher::IgnoreOwnWrite(File)` 会记录写入后该文件当时的哈希——而 watcher 会在文件仍哈希到该值期间丢弃该文件自身的重建。任何依赖该文件的内容（从它继承的子 `.dfs`）仍会被重建。这些规则有意针对内容，而不是通知：

- 作者再次编辑该文件会改变哈希，因此保存时会像往常一样重新构建，而记录则会被
  丢弃；
- 对同一次写入的多个通知（操作系统会报告不止一个，而监视器会按 tick 将它们批量合并）
  全部跳过，完全不跳过也不会有任何代价——记录会在 10 秒后自行过期；
- 重新扫描或目录移动无法说明内容是谁写入的，因此它仍会将文件加入队列；
- *Rebuild from Source* 和 *Rebuild DFX* 会直接将文件加入队列，永不跳过。

写回不会触及资产，因此其来源戳记仍记录着写入之前的文本。下一次完整构建会看到新文本，发现戳记已过时，并重新构建为相同的值。

<a id="the-bulk-batch"></a>

### 批量批次

重新导出内容包、切换分支或运行脚本化重写会一次性改变许多源文件，而重建每一个都会为每个系统排队一次 Niagara 编译。在这个项目中，这曾让编辑器崩溃：24 个系统同时重建，将 237 个作业放入了编译队列。

因此，当一个批次包含超过 **Bulk Rebuild Threshold** 个文件（Project Settings → Plugins → DreamFX，默认 8）时，会改为询问而不是直接构建：

> DreamFX：*N* 个源文件被同时更改。现在全部重建会排队数百次 Niagara
> 编译。—— **立即重建**

这些文件会被保留，而不是丢弃：丢弃它们会让资产过时却无人察觉，这正是该门槛旨在防止的失败，只是更隐蔽。如果提示过期，*Tools
→ DreamFX → Rebuild DFX* 会执行相同的操作。

普通保存永远不会被门槛拦截——单个文件，或任何等于或低于阈值的批次，都会立即重建（在启用该设置时增量进行），因为这条路径就是迭代循环。显式的 *Rebuild DFX* 同样永远不会被门槛拦截：它是被明确请求的。

> [!NOTE]
> 这个门槛最初是基于一种不同的理论编写的——即危险在于监视器会重放编辑器关闭期间发生变化的一切——而测量结果与之相矛盾。
> `RegisterDirectoryChangedCallback_Handle` 在注册时就开始监视，并且报告没有积压：
> 编辑器关闭时更改的十二个文件在重新打开时产生了*完全没有任何*结果。真正造成伤害的批次是在编辑器打开时到达的。修复前用十二个文件测量：54
> 次 Niagara 系统编译和八次编译池饱和。修复后：出现提示，在被请求之前什么都不会构建。

<a id="verify-dfx"></a>

### Verify DFX

以验证模式对每个源文件运行生成器。不会写入任何内容——不写资产，也不写来源戳记。`.dfe` 文件会被跳过：它们不生成资产，因此没有什么可比较的。

| Toast | 条件 |
| :-- | :-- |
| `DreamFX: {N} source(s) verified, all assets in step.` | 干净 |
| `DreamFX: {Drifted} of {N} source(s) out of step ({Failed} failed). See the Output Log.` | 漂移或失败 |

<a id="export-vs-adopt"></a>

### Export 与 Adopt

两者都会反编译。它们的区别在于之后会发生什么，而这个区别正是关键所在。

| | **Export .dfs** | **Adopt** |
| :-- | :-- | :-- |
| 写入到 | `DecompiledOutputDirectory`（默认 `DFX/Decompiled`） | 真实的源根目录——`<Project>/DFX/…` 或 `<Plugin>/DFX/…` |
| `Name=` 名称 | `Decompiled/<original directory>/<asset>`——一个**镜像** | 原始资产 |
| 被构建拾取 | 是——保存其中一个会重建其镜像 | 是 |
| 修改资产 | **永不** | 是：从新源文件重建它并标记来源 |
| 无法表示的功能 | 警告，并在文件中列出它们 | **拒绝** |
| 含义 | “让我以文本形式阅读和试玩这个” | “从现在起，这段文本是唯一的真相” |

将两者区分开的是结构性的，而非流程性的：导出无法命名它来自哪个资产。
`/Game/FX/NS_X` 导出到 `DFX/Decompiled/Game/FX/NS_X.dfs`，其 `Name=` 是 `Decompiled/FX/NS_X`，
因此构建会写入 `/Game/Decompiled/FX/NS_X`，而无论文件如何被编辑，原始资产都不会被触及。
整个反编译树因此就是普通源文件——被监视、保存时构建、被 lint 并纳入 CI。

在此安排之前写入的导出仍会命名原始资产。它们会被以
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) 拒绝而不是被照样执行；重新导出会替换它们。

导出*已经*是镜像的资产也会被拒绝——这会让两个源声明同一个
资产。桥接的 `decompile` 操作会针对这种拒绝、反编译失败或
文件写入失败报告 `ok: false`。成功响应会包含写入的 `outputPath`；桥接请求不会
启动文本编辑器。

<a id="adopt"></a>

### Adopt

1. 根据资产的挂载点算出源路径：`/Game/FX/NS_X` → `<Project>/DFX/FX/NS_X.dfs`，
   `/MoonToon/FX/NS_X` → `<MoonToon>/DFX/FX/NS_X.dfs`。
2. 如果该目标已存在，则**拒绝**，包括不完整或无效的源文件，或另一个
   `.dfs` 已经声明了相同的目标资产。
3. 反编译。如果有任何内容无法表示，则**拒绝**，并列出具体内容。
4. 确认，列出要写入的文件和要重建的资产。
5. 再次检查目标，并在不替换现有目标的情况下发布新文件，
   包括在确认对话框打开期间创建的文件。重建资产并标记来源。
6. 重新反编译重建后的资产并逐字节比较。

| 诊断 | 原因 |
| :-- | :-- |
| `DFX8010` | 该资产具有 DreamFXLang 无法表达的功能 |
| `DFX8011` | 源目标已存在，或另一个源已经生成此资产 |
| `DFX8012` | 已 adopt，但重建后的资产无法重新导出为相同的文本 |

第 3 步使第 6 步有意义：在没有已知缺口的情况下，不匹配是真正的缺陷，而不是预期中的损失。`DFX8012` 会保留源文件不动——资产是从它重建的，因此无论哪种情况文本都是权威的——并记录第一个不同的行。

<a id="open-dreamfx-workspace-vscode"></a>

### Open DreamFX Workspace (VSCode)

重写 `DFX/DreamFX.code-workspace`，然后通过 VSCode → 操作系统默认编辑器 →
Notepad 启动它。该文件**每次都会被完全重写，绝不合并**：手动添加的 `launch` 或 `tasks`
块会丢失。每用户的配置应放在 `DFX/.vscode/` 中。

```json
{
  "folders": [
    { "name": "DreamFX Source", "path": "." },
    { "name": "Plugin: DreamFX", "path": "../Plugins/DreamFX/DFX" }
  ],
  "settings": {
    "files.associations": { "*.dfs": "dreamfxlang", "*.dfe": "dreamfxlang", "*.dfm": "dreamfxlang" }
  },
  "extensions": {
    "recommendations": ["typedreammoon.dreamfxlang-language-support"]
  }
}
```

项目根目录始终排在最前，并且始终是 `"."`——工作区文件就位于其中，而稳定的
文件夹标识能让 VSCode 的每文件夹状态在重写后保持关联。位于另一个
盘符上的插件根在 Windows 上没有相对形式，会获得绝对路径。

`dreamfxlang` 是 [DreamFXLang
扩展](https://github.com/TypeDreamMoon/dreamfx-language-support) 注册的语言 id。无论该扩展是否已安装，都会写入该关联——没有它，VSCode 会回退到纯文本，这
无害——而推荐只是一个 VSCode 显示一次、若被忽略便不再显示的提示。

| Toast | 条件 |
| :-- | :-- |
| `DreamFX failed to create workspace: {Error}` | 文件无法写入 |
| `Opened DreamFX workspace in VSCode: {Path}` | VSCode 已启动 |
| `Opened DreamFX workspace: {Path}` | 操作系统默认编辑器已启动 |
| `Opened DreamFX workspace in Notepad: {Path}` | Notepad 已启动 |
| `DreamFX could not open workspace: {Path}` | 所有启动器都失败 |

<a id="vscode-discovery"></a>

## VSCode 发现

仅限 Windows，最具体的优先。磁盘上存在的第一个候选项胜出。

| # | 位置 |
| :-- | :-- |
| 1–2 | `%LOCALAPPDATA%\Programs\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 3–4 | `%LOCALAPPDATA%\Programs\Microsoft VS Code Insiders\{Code - Insiders.exe, bin\code-insiders.cmd}` |
| 5–6 | `%ProgramFiles%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 7–8 | `%ProgramFiles(x86)%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 9 | 每个 `PATH` 条目，检查 `code.cmd`、`code.exe`、`Code.exe`、`code-insiders.cmd`、`Code - Insiders.exe` |

打开**工作区**会遵循 *Open Workspace In New Window*。打开**文件**始终传递
`--reuse-window -g <path>:<line>:<col>`——每次诊断跳转都开一个新窗口会无法使用。

<a id="settings"></a>

## 设置

*Project Settings ▸ Plugins ▸ DreamFX*，存储在 `Config/DefaultEditor.ini` 中。

| 设置 | 默认 | 作用 |
| :-- | :-- | :-- |
| **Open Workspace In New Window** | `true` | 只有 workspace 启动器读它 |
| **Incremental Build on Save** (*Workspace*) | `true` | watcher 由存盘触发的重建只写入自上次构建以来发生变化的内容，无法做到时回退为完整重建。*Rebuild DFX*、*Rebuild from Source* 和 **Rebuild them now** 始终完整重建。见[增量构建](incremental-builds.md) |
| **Write Back to Source on Asset Save** (*Workspace*) | `false` | 保存一个生成系统时，把它的存储值写回它的 `.dfs`，就像 *Write Back to Source* 那样。DreamFX 自身的保存、自动保存、cook、commandlet 或运行时永远不会这样。见 [Write Back to Source](#write-back-to-source) |
| **Write Back Structural Edits** (*Workspace*) | `false` | 允许写回不仅重写值，还可以添加和删除语句；关闭时只会报告这些改动，stack 保持不动。由命令和保存钩子读取 |
| **Decompiled Output Directory** | `DFX/Decompiled` | *Export .dfs* / *Export .dfe* 的写入位置，相对项目目录。它是普通源码：被发现、被监视并被构建。这里的文件必须命名到 `Decompiled/` 命名空间（DFX8013） |

<a id="scripting-the-same-commands"></a>

## 用脚本调用同样的命令

`UDreamFXEditorLibrary` 把每条命令都暴露给 Python 和 Blueprint，转发到菜单调用的同一份实现。
Slate 条目无法从脚本调用，而只有人类点击才能触达的命令，就是永远不会被回归测试的命令。

```python
import unreal
lib = unreal.DreamFXEditorLibrary
lib.rebuild_all_sources()
lib.verify_all_sources()
print(lib.write_workspace_file())          # writes, does not launch
lib.export_system(unreal.load_asset("/Game/FX/NS_Spark"))
lib.adopt_system(unreal.load_asset("/Game/FX/NS_Spark"), True)   # True = skip the confirmation
```

`bSkipConfirmation` 只抑制模态对话框。两种拒绝仍然生效，只是改为以 toast 和日志行的形式报告，
而非对话框——一个没人能点的模态会阻塞调用线程。

<a id="disabling-the-integration"></a>

## 禁用整套集成

```powershell
UnrealEditor.exe "<Project>.uproject" -NoDreamFXEditor
```

按裸命令行参数解析，所以 `-NoDreamFXEditor` 是唯一被接受的拼写。

| 被禁用的 | 仍然有效的 |
| :-- | :-- |
| 本页所有菜单、工具栏和右键菜单条目 | `-run=DreamFX` commandlet，它从来不用这些 |
| source watcher 与存盘即重建 | 解析器、生成器、反编译器和设置对象 |
| 生成资产守卫 | 已经生成并保存在磁盘上的资产 |

commandlet 运行会经另一条路到达同样的状态：`IsRunningCommandlet()` 返回真，
模块什么都不注册。

<a id="the-module-index"></a>

## 模块索引

```powershell
pwsh -File .skill/dfx.ps1 index
```

写入 `DFX/.dfx-index.json`：搜索路径暴露的每个模块和动态输入，带其声明的 stack、分类、描述和 **input signature**。
编辑器扩展读取该文件以提供补全和悬停——它不能询问引擎，因为每次 `dfx` 调用都会启动引擎，
而有人正在打字时发生的任何事情都不能花掉几十秒。

两半，代价差异极大。stack 和元数据来自资产的 usage 位掩码，这是免费的——和引擎自带的 stack UI
用来过滤的是同一份列表。input signature 则是*探测*出来的，因为资产级 schema 完全遗漏了内联编辑条件和
static switch，而一个不含枚举形输入的补全列表比没有还糟。此处实测：571 个模块，不探测 2.9 秒，探测 8.1 秒。

> [!WARNING]
> **有些模块图无法遍历。** `/Niagara/Modules/Masks/ConeMask`——引擎自带内容——
> 会让 `UNiagaraGraph::ReferencesStaticVariable` 无界递归，栈溢出最终终止进程。
> 该模块上的 `dfx schema` 一直如此；索引只是走到第 45 个模块时撞了进去。
> 插件无法给引擎代码加 visited 集合，而栈溢出也不值得去捕获，所以遍历改为*可恢复*：
> 即将被探测的模块先被记录，下一次运行开始时仍然记录在案的模块会被隔离。`dfx.ps1` 反复重跑，
> 直到遍历完成，所以少数几个坏图只多花几次启动，而不是让功能失效。被隔离的模块保留其名称、路径和 stack；
> 只有它的输入缺失，并且它会如实说明。

| 标志 | |
| :-- | :-- |
| `-Out <path>` | 写到 `DFX/.dfx-index.json` 以外的地方 |
| `-NoInputs` | 跳过探测。快，足以回答“存在什么” |
| `-Retry` | 清除隔离并重新尝试每个模块——用于引擎升级之后 |

索引会记录引擎路径和已启用的插件列表，因为正是它们令索引失效：
不同的引擎就是不同的模块集合，而启用一个内容插件会多出一整个家族。
目前还没有任何东西比较它们——那需要有一个实时编辑器来对照——所以重建是一条命令，
而不是靠猜。

<a id="see-also"></a>

## 另见

- [快速上手](../getting-started/quickstart.md) —— 编辑器侧工作流，按顺序
- [语言参考](../language/README.md) —— 导出的文本是什么意思
- [增量构建](incremental-builds.md) —— 一次保存会写什么，以及何时回退为完整重建
- [pull](pull.md) —— *Write Back to Source* 背后的写回：它可以碰什么，拒绝什么
- [资产比较与导出](asset-comparison-and-export.md) —— 比较失败和导出覆盖范围的限制
- [诊断](../diagnostics/README.md) —— 每个 `DFXnnnn`，包括 `DFX8010`–`DFX8018`，那些
  说明反编译无法携带什么的 gap 代码
- `dfx.ps1 coverage` —— 对整个内容树问同样的问题，按功能分桶
