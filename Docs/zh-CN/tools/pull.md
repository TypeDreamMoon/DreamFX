---
title: "`pull`：将当前资产值写回源"
description: "`pull`：将当前资产值写回源"
---

<a id="pull-write-current-asset-values-back-to-source"></a>

# `pull`：将当前资产值写回源

> 记录于 2026-10-09，分支 `feat/pull`。工具：`dfx pull`，实现于
> `Source/DreamFXEditor/Private/WriteBack/DreamFXPull.cpp`。
>
> [写回覆盖范围](write-back-coverage.md)记录了实测差距。另一份研究笔记
> `write-back-from-dreamgui.md` 描述了值得复用的 DreamGUI/DUI 技术。本文档
> 涵盖 `pull` 本身的语义、边界和测量；它不替代那些笔记。

<a id="0-summary"></a>

## 0. 概述

**`pull` 将值从资产的快速迭代存储复制到源码中已声明的字面量。**

工作流程是“在文本中编写结构，在编辑器中调整值”。最后一步需要谨慎：
调整后的值存在于资产中，而文本要么完全省略该输入（因此重建
会恢复模块默认值），要么包含的是**读取路径所看到的**内容，而非**存储
所包含的**内容。对于 `NS_Effects1_Mesh` 和 `Sprite_Atlas_Size.MeshYaw`，存储包含 `-90`，然而
每次引脚读取都返回 `0.0`。`pull` 使存储的、调整后的值成为源码的一部分。

<a id="1-usage"></a>

## 1. 用法

```powershell
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# Dry run: report proposed line edits without writing any source bytes (the default).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Engine $eng

# Apply: back up the source before writing values.
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Apply -Engine $eng

# Read another asset: exported source targets a mirror, but the tuned values are in the original.
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Asset /Game/FX/NS_Spark -Apply -Engine $eng

# Structural edits: add/remove lines only when correspondence is unambiguous (section 4d).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Structure -Apply -Engine $eng

# Ignore the baseline and compare all declared values (section 4c; also used when no baseline exists).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -NoBaseline -Apply -Engine $eng
```

| 参数 | 含义 |
|---|---|
| `pull <file.dfs>` | 要更新的源；恰好一个 `.dfs` 系统文档 |
| `-Apply` | 写入源。**没有它，命令为试运行** |
| `-Asset=<package path>` | 读取**此**资产，而非源 `Name=` 所指定的资产 |
| `-Structure` | 允许**结构编辑**，增加或删除行；否则仅报告 |
| `-NoBaseline` | 忽略先前的 `-Apply` 基线并比较所有内容（第 4c 节） |

`-Asset` 不仅仅是便利：**导出无法以结构方式指向它所导出的资产**。
Plan-v4 V1 将所有导出放在 `Decompiled/` 命名空间中，因此重建无法
触碰原始资产。因此，要从原始资产读取调整后的值到其导出中，需要此
覆盖。源的 `Name=` 仍决定 `build` 写入的内容；`-Asset` 仅选择
`pull` 读取的内容。

<a id="output-locations"></a>

### 输出位置

| 内容 | 位置 |
|---|---|
| 写入前的源，每个文件一个备份 | `Saved/DreamFX/Pull/<source path relative to project root>`，例如 `Saved/DreamFX/Pull/DFX/Decompiled/Game/FX/NS_Spark.dfs` |
| 本次运行的报告，包括试运行 | 相同路径，扩展名为 `.report.txt` |

备份和报告保存在 `Saved/` 中，**位于 `DFX/` 之外**。存储于资产中但未在文本中声明的
输入会**逐一报告，不截断**。真实资产可能有数十个：
`破空灰尘` 有 94 个。这些名称仅出现在此报告中，每个代表一个输入，
重建必须从其模块默认值恢复。截断会隐藏该风险。

编辑器通过资产上下文菜单中的 **Write Back to Source**
以及 Niagara 系统编辑器工具栏的 DreamFX 菜单暴露相同的操作。它等同于 `-Apply`，其中 `-Structure`
由 `bPullStructuralEdits` 控制。资产保存时的可选写回使用 `bPullOnAssetSave`
（默认关闭）。写入文件后，编辑器通知源监视器，使其不会重建
它刚刚读取的资产。参见[编辑器集成](editor-integration.md#write-back-to-source)。

<a id="2-what--apply-means"></a>

## 2. `-Apply` 的含义

- **没有 `-Apply`**，读取资产、解析源并计算每个替换字面量。
  打印 `file(line,column): info DFX7116: pull: <module>.<input>: <old literal> -> <new value>`
  并停止。源字节不变；报告文件仍会写入。
- **有 `-Apply`**，备份源并仅替换**每个字面量自身的字节范围**。
  结果**必须成功解析**才能保存。解析失败会报告 `DFX7110`
  并保持源不变。
- **幂等**：第二次 `-Apply` 不写入任何字节（第 5 节，情况 1）。比较是**按值**，
  而非拼写：使用与 `build` 相同的 `FValueLowering::Lower` 将字面量小写，然后将其
  字节与存储比较。`0` 和 `0.0` 相等且不会被重新格式化。
- **`pull` 从不写入资产。** 它使用 `LoadObject` 和只读 API，没有 `Save` 路径。

<a id="3-relationship-to-decompile-and-the-build-safety-gate"></a>

## 3. 与 `decompile` 和构建安全门的关系

这些操作覆盖双向写回的不同部分：

| 操作 | 方向 | 读取 | 作用 |
|---|---|---|---|
| `decompile` | 资产 → 文本 | 实时输入**引脚** | 写入结构；`-NoDefaults` 包括**每个输入名称**，带有引脚派生的值 |
| **`pull`** | 资产 → 文本 | 快速迭代**存储** | 更新文本中**已声明**的字面量值 |
| `build` | 文本 → 资产 | 源文本 | 写入资产；**安全门 DFX8018** 阻止保存会丢失文本无法表达的事实的重建 |

1. **`decompile` 提供键；`pull` 提供值。** `pull` 不发明未声明的输入：
   那会改变结构。首先使键显式化，最容易的是使用 `decompile -NoDefaults`。
   [写回覆盖范围，第 6.4 节](write-back-coverage.md#64-read-path-export-every-input-explicitly)
   称之为写回的读取模式；`pull` 依赖它。
2. **`pull` 补充安全门。** 安全门捕获已更改的事实，其结构地址
   在两侧都存在，但其输入在源中缺失。`pull` 使该输入显式化。
   对于 `NS_Effects1_Mesh`，以原始资产为目标会在两个
   `MeshYaw: 0000B4C2 -> 00000000` 事实上触发安全门（覆盖范围第 6.8 节，第 6 行）。使用 `pull` 将 `-90.0` 写入
   源，然后 `build` 成功：源声明了更改后的值，因此规则 3 允许它。
3. **已存储但未声明的值会被报告，而非添加**（`DFX7106`）。它们是否
   真的会消失是安全门（`DFX8018`）的决定。等于模块默认值的值会在重建时
   恢复而不丢失事实：在第 6.8 节测量的 37 个未声明条目中，只有 8 个真正丢失。`pull` 不重新计算默认值，那会引入
   另一个探针和默认表，可能偏离安全门。

<a id="4-what-can-and-cannot-change"></a>

## 4. 可以改变和不可以改变的内容

<a id="initial-scope-literal-arguments-of-existing-inputs-in-existing-module-calls"></a>

### 初始范围：现有模块调用中现有输入的字面量参数

所有四项对应检查都验证身份而非猜测：

1. **节点顺序**：源语句对应于资产中同一发射器和
   堆栈中的相同序数节点。
2. **模块身份**：由 `FModuleLibrary::FindModule` 解析的 `UNiagaraScript` 必须
   与该节点使用的脚本具有完全相同的资产路径。
3. **节点名称**：当语句包含 `as <name>` 时，资产节点必须具有该名称。
4. **折叠赋值**：连续赋值，由 L2 折叠为一个 Set Parameters
   模块，计为**一个**节点；其对应物必须是 `bIsSetParameters`。

任何不匹配都会拒绝**整个堆栈**（`DFX7109`，列出两侧的节点）。通过差一对应的方式
编辑字面量比拒绝编辑更危险。

对于值：

- 使用**输入的实际类型**通过 `FValueLowering::Lower` 降低参数。失败或非字面量形式（链接、动态输入、`hlsl {}`、`curve {}`、DI、对象引用）会报告 `DFX7111` 并保持其不变。**更改值模式是编辑，而不是写回。**
- 属于数据接口或对象引用的存储值也会保持不变。
- DSL 会规范化空格、连字符和大小写。`Scale RGB` （Vector3）及其内联条件 `ScaleRGB` （bool）因此成为**同一标识符**。**值的类型**会选择预期的输入，与生成时完全一致。如果它实际上表示另一个输入，则该存储条目超出其职责范围，会被静默跳过。多个可接受候选会产生 `DFX7111`；命令不会猜测。

<a id="initial-exclusions"></a>

### 初始排除项

下表描述 v1；第 4b 节记录后续覆盖扩展。

| 排除项 | 结果 |
|---|---|
| 新参数、新增/删除的模块、任何结构性变更 | 对于文本中不存在的已存储输入，`DFX7106` |
| 事件栈 `OnEvent` 和模拟阶段 `Stage` | `DFX7111`；v1 不处理其单独的焦点切片读取路径 |
| 渲染器属性以及系统/发射器 `Settings` | 初始时不属于调用参数；由安全门 `DFX8018` 覆盖 |
| 赋值值（Set Parameters 条目和 `Defaults = {}`） | v1 中不处理 |
| 通过 `from "..."` 继承但不存在于此文件中的栈 | 仅读取此文件中写入的栈；`Emitter.FromPath` 发出 Verbose 消息 |
| 非模块输入事实族（`di` / `compiled` / `user`） | 既不读取也不报告 |

<a id="whitespace-line-endings-and-comments"></a>

### 空白、行尾和注释

替换仅覆盖 `FValue::StartOffset/EndOffset`，即词法分析器记录的字节跨度（第 6 节）。尾随注释、缩进和行尾风格保持不变。结果仍必须能够解析。

<a id="4b-coverage-the-sources-declarable-values-second-pass-2026-10-09"></a>

## 4b. 覆盖范围：源文件的可声明值（第二遍，2026-10-09）

第二遍将 v1 的模块调用字面量扩展到源文件已有的声明表面。规则保持不变：仅写入已声明的地址，拒绝歧义，仅替换字面量字节。

| 类型 | 示例地址 | 值来源 | 源文件拼写 |
|---|---|---|---|
| 模块参数 | `Module.Input` | 快速迭代存储，`Constants.<emitter>.<node>.<input>` | `LiteralToSource` / `EnumEntryToSourceToken` |
| 渲染器属性 | `MeshRenderer.FacingMode` | `GetRendererProperties`，即由 `SetRendererProperties` 消费的 JSON | `RenderJsonPropertyAsSource` |
| `Bind X -> Y` 目标 | `Bind CustomSorting -> ...` | `GetRendererBindings`，通过适配器读取实时结构体 | 直接写入名称 |
| 系统/发射器 `Settings` | `Settings.WarmupTime` | `GetSystemProperties` / `GetEmitterProperties` | `RenderSettingSource` |
| 折叠赋值，即 Set Parameters 条目 | `<node>.<parameter name>` | 该节点的常量，**按参数名**匹配 | 同上 |
| `Defaults = { }` | `Defaults.Particles.Foo` | `GetParameterDefaults` | 同上，包括链接形式 |

有三个实现决策很重要：

1. **设置回读使用生成器自身的表。** `RenderSettingSource` 反转 `PlanSettings`，并与 `SystemSettings` / `EmitterSettings` 并列。第二个表会引入另一个漂移源，正是覆盖范围第 3.8 节记录的反编译器字段表故障。别名必须反向运行：写入映射 `CPU -> CPUSim`；读取必须映射 `CPUSim -> CPU`，否则 `pull` 会发出该表不接受的拼写。
2. **渲染器值只有一个拼写实现**，即 `WriteBack/DreamFXSourceValue.h`，由反编译器和 `pull` 共享。不同的拼写会在每次重写正确的行。该提取本意是一次纯粹移动；14 个 RoundTrip 测试夹具捕获了一个错误：替换旧的 `switch` 后遗留的 `break` 语句导致只发出第一个渲染器属性，使五个夹具失败。
3. **比较值，而不是字符串。** `LowerDeclaredPropertyToJson` 使用生成器代码在与资产 JSON 比较之前降低源值。字符串比较会将简写 `Material = "Plugin.MoonToon:..."` 替换为绝对路径，并会反复重写 `0.1f`，因为属性转换器会将 `float` 扩展为 `double`。整数精确比较；其他数字按该语言可表达的 `float` 精度比较。

渲染器属性、`Settings`、赋值和 `Defaults` 现在已覆盖，并带有断言。

<a id="4c-dirty-values-write-only-what-actually-changed-in-the-asset-dfx7115"></a>

### 4c. 脏值：仅写入资产中实际发生变化的内容（`DFX7115`）

直接比较资产和源文件在首次运行时是正确的，但之后的差异可能意味着**有人编辑了源文件**。在这种情况下，资产较旧，将其写回会丢弃更新的决定。与 DreamGUI 一样，`pull` 仅写入已更改的值，并带有可复现的基线。

最后一个 `-Apply` 记录 `Saved/DreamFX/Pull/**/<file>.baseline.txt`：排序后的 `scope<TAB>key<TAB>value` 三元组，使用与安全门相同的 scope/key 划分。替代方案无法回答所需的逐值问题：

- `UPackage::IsDirty` 是整个资产的一个位，可通过加载设置，并且对在全新进程中打开资产的 commandlet 没有任何有用说明。
- 编辑器修改事件也缺乏所需的粒度，并且无法跨进程保留。

| 条件 | 结果 |
|---|---|
| 值不同；基线包含此条目且等于资产的当前值 | 源文件较新：保留它，报告 `DFX7115`，不写入 |
| 值不同；没有基线条目（新值或首次运行） | 写入 |
| 值不同；基线条目与当前资产值不同 | 资产再次发生变化：写入 |
| 记录基线 | 只有 `-Apply` 会记录它。试运行尚未协调差异，因此不得将其标记为已处理 |
| 磁盘上没有基线 | 比较所有内容并报告 `DFX7114`；`-NoBaseline` 明确请求相同行为 |

边界：

- 只有**源文件声明的地址**会进入脏集或获得基线保护。
- 基线记录的是**观察到的**值，而不一定是写入的值。以 `DFX7111` 拒绝的值仍会记录为资产的当前状态，并在下次计为未更改。这有意避免每次运行都询问相同的拒绝。
- 渲染器地址是位置性的（`emitter X renderer N`）；更改顺序会导致基线不匹配，因此值会被视为脏值。
- 删除基线始终会恢复完整比较。

<a id="4d-structural-editing--structure-report-only-by-default"></a>

### 4d. 结构编辑：`-Structure`（默认仅报告）

V1 会拒绝结构不同的整个栈（`DFX7109`）。现在可用显式开关允许无歧义的编辑。首先计算对应关系：猜测会移动模块，而不仅仅是文本。

1. 按模块资产对齐节点。连续的赋值计为**一个**哨兵键节点；两个 Set Parameters 节点通过**条目**区分，而不是位置。
2. **仅当最长公共子序列唯一时才使用它**。候选对 `(i,j)` 在 `LCS(prefix) + 1 + LCS(suffix) == LCS(total)` 时位于最优对齐上。符合条件的对数量必须恰好等于 LCS 长度，并在两侧严格递增，使其成为强制对。重复模块加上另一个删除可能产生多个 LCS 对齐；这些会被拒绝。
3. 对齐后验证每一对的 `as <name>`，因为节点名不是键的一部分。

支持的编辑：

- 源文件中缺失的资产模块，在其按执行顺序的下一个源语句之前**插入一行**，若它是最后一个，则插入在栈的 `}` 之前。形式为 `<module>@<major.minor>[ as <node name>](<inputs>);`。版本固定保留该资产的模块版本。仅当节点名称与模块资产名称不同时才输出 `as`；否则引擎的默认命名已足够。仅按声明顺序输出实际存在于存储中的输入。
- 资产中缺失的源语句，仅当该行归其所有时才**删除其行**：前后为空白，无尾随注释。注释不是资产拥有的结构，不得删除；拒绝时说明原因。
- **复用文件风格。** 缩进取自该栈中最近的语句，其次取本文档中另一个栈，最后取四个空格。行尾取自插入行。
- 拒绝并说明原因：无法从该调用重建的、带 DI/对象引用输入的模块；其折叠赋值缺少支持夹具的未声明 Set Parameters 节点；无字节范围的语句；共享一行的语句；无记录闭合范围的栈。

没有 `-Structure` 时，可寻址差异会产生 `DFX7113`，描述双方以及可应用它们的切换。**该栈中不会更改任何字节**，因为在其结构未解决时，位置值寻址无法进行。`DFX7109` 仍为“完全无法建立对应关系”。`DFX7112` 报告每个**已应用**的结构编辑。

<a id="4e-editor-entry-point-the-bridge-pull-action"></a>

## 4e. 编辑器入口点：桥接 `pull` 操作

命令行 `pull` 启动无头编辑器并**从磁盘**读取资产。VS Code 扩展的情况不同：编辑器已在运行，在 Niagara 中调整的值可能未保存，且文本缓冲区可能比磁盘上的文件更新。桥接的 `pull` 操作（`Saved/DreamFX/Bridge/`；参见[编辑器集成](editor-integration.md)）通过相同的实现 `FPuller::PullBuffer` 处理此情况。

请求使用与其他操作相同的 `protocol`：

| 字段 | 含义 |
|---|---|
| `action` | `"pull"` |
| `sourceFile` | 此 `.dfs` 的绝对路径。仅接受系统文档（`.dfs`）；`.dfe`、`.dfm` 及所有其他扩展名返回 `ok:false` |
| `text` | 可选。客户端的**完整缓冲区**，可能未保存 |
| `assetPath` | 可选。等同于 `-Asset`：从此资产读取值 |
| `structure` | 可选布尔值。等同于 `-Structure` |
| `apply` | 可选布尔值。其含义如下所述 |

**`text` 是否存在决定行为：**

| | 有 `text` | 无 `text` |
|---|---|---|
| 处理的输入 | `text`（缓冲区） | 磁盘上的 `sourceFile` |
| 源文件 | **永不写入。** 客户端拥有缓冲区并决定何时保存 | 同命令行：仅 `apply:true` 写入，备份到 `Saved/DreamFX/Pull/` 之后 |
| 结果文本 | 返回在 `pull.newText` 中，包括预览，无论 `apply` 如何 | 无 `newText` |
| `apply:true` 的效果 | **仅记录基线**：客户端承诺将 `newText` 放入其缓冲区 | 写入文件并记录基线 |
| 缺少 `sourceFile` | 允许：缓冲区可能从未保存过 | `ok:false`、`No such source file` |

资产是**编辑器进程中**的对象：`LoadObject` 返回已打开资产的内存中对象。因此，在 Niagara 编辑器中更改但尚未保存的值就是这里读取的值。命令行 `pull` 只能看到磁盘上的资产。

**基线与命令行共享。** 其路径仅取决于 `sourceFile`（`Saved/DreamFX/Pull/<relative path>.baseline.txt`）。在命令行上应用，然后在编辑器中预览同一文件，使用的是同一条记录。其文本比资产更新的值仍会被 `DFX7115` 保留；缓冲区请求无法静默地将它们改回。没有 `apply` 的预览，如命令行试运行，**不会记录**基线。

响应保留桥接信封（`protocol`、`requestId`、`ok`、`durationMs`、`message`、`diagnostics`）。`message` 是一行摘要，例如 `1 value(s) differ; 1 withheld (text is newer) (preview)`。嵌套的 `pull` 对象添加结果：

```json
{
  "ok": true,
  "message": "1 value(s) differ (preview)",
  "diagnostics": [ ... ],
  "pull": {
    "changed": true,
    "newText": "...",
    "edits": 1,
    "compared": 3,
    "changedValues": 1,
    "undeclared": 5,
    "unwritable": 0,
    "unaddressable": 0,
    "withheld": 0,
    "added": 0,
    "removed": 0,
    "structureRefused": 0,
    "baselineUsed": false,
    "baselineWritten": false,
    "wroteFile": false,
    "reportPath": "…/Saved/DreamFX/Pull/….report.txt",
    "report": ["…"]
  }
}
```

- 有 `text` 时，`changed` 表示 `newText` 与请求的 `text` 不同；无 `text` 时，表示文件已写入。`changedValues` 是 `FPullResult::Changed`：与资产不同的值的数量。
- `newText` 仅在请求包含 `text` 时出现。在 `ok:false` 上，例如解析失败，它等于请求的 `text` 不变，`changed` 为 `false`，且 `diagnostics` 解释原因。
- 预览实际上会生成重写后的文本。若该文本解析失败（`DFX7110`），预览返回 `ok:false`。命令行试运行不生成重写文本，因此没有此失败模式。
- 报告文件（`reportPath`）即使在预览期间也会写入，与命令行相同。

<a id="5-measurements-2026-10-09-raw-logs-in-savedpull-proberuns"></a>

## 5. 测量（2026-10-09；原始日志在 `Saved/pull-probe/runs/` 中）

所有四个案例都使用了真实资产以及 `Saved/pull-probe/` 下的源副本；项目的 `DFX/` 树未被改动。原始文件 `NS_Effects1_Mesh.uasset` 和 `破空灰尘.uasset` 未更改：其 SHA256 哈希与 `Saved/DreamFX/gate-probe/asset-backup/` 逐字节匹配，修改时间分别为 10-08 17:38 和 03-04 22:47，均早于本次测试。

<a id="case-1-ns_effects1_mesh-sprite_atlas_sizemeshyaw-store--90-pin-00"></a>

### 案例 1：`NS_Effects1_Mesh`、`Sprite_Atlas_Size.MeshYaw`（存储 `-90`，固定 `0.0`）

```powershell
# Export every key. A normal export has no MeshYaw line (the shape tested in case 2).
pwsh -File $dfx decompile /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
# Read the original: this source's Name= targets a mirror whose store contains 0.
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -Apply
pwsh -File $dfx build Saved/pull-probe/NS_Effects1_Mesh.dfs
pwsh -File $dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
```

<a id="case-1-results"></a>

### 案例 1 结果

```text
$ dfx decompile <original asset> -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
  106: MeshYaw = 0.0                     <- pin value; the store contains -90
  Export SHA256 = 3E3A1DC4...

$ dfx pull <source> -Asset <original asset>          # dry run
  NS_Effects1_Mesh.dfs(106,27): info DFX7116: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  pull: 1 literal(s) would be written and nothing was: this is a dry run.
  === DreamFX pull: 14 value(s) compared, 1 to write, 5 not declared, 1 not writable, 0 stack(s) not addressed | dry run ===
  SHA256 unchanged

$ dfx pull <source> -Asset <original asset> -Apply
  pull: wrote 1 literal(s) into '...NS_Effects1_Mesh.dfs'. The file it was is at
        'C:/CrossingVoid/Saved/DreamFX/Pull/Saved/pull-probe/NS_Effects1_Mesh.dfs'.
  NS_Effects1_Mesh.dfs(106,27): info DFX7116: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  === DreamFX pull: 14 value(s) compared, 1 written, 5 not declared, 1 not writable, 0 stack(s) not addressed | applied ===

  Whole-file Compare-Object (backup vs result): exactly one changed line
      => MeshYaw = -90.0
      <= MeshYaw = 0.0

$ dfx build <source>                       # source targets the mirror, so rebuild the mirror
  === DreamFX done: 1 built, 0 up to date, 0 failed | 0 error(s), 0 warning(s) ===
  No build safety messages: the source declares this input, so rule 3 allows it.

$ dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
  DIFF ... original 186 fact(s), mirror 186; 1 only-original, 1 only-mirror
  Only system FixedBounds differs (original +/-100, mirror +/-300); MeshYaw is 0000B4C2 on both sides.

$ dfx pull <source> -Asset <original asset> -Apply    # second run
  info DFX7107: ... already holds every value ... (14 compared, 5 not declared, 1 not writable). No bytes written.
  === DreamFX pull: 14 value(s) compared, 0 to write, ... | applied ===
  SHA256 and mtime unchanged (idempotent)
```

解读：

- **仅一行更改。** `-NoDefaults` 导出保持 123 行；缩进、其他字面量、注释和行尾均被保留。
- **构建确实重新构建了**（`1 built`，而非 `up to date`）并干净通过。这证明了 `pull` 与门控的协作：将 `0` 更改为 `-90` 是允许的，因为源声明了它。
- **`asset-diff` 在 `MeshYaw` 上不再有差异**；两个值均为 `0000B4C2`。其余 `system FixedBounds` 差异早于本次测试：先前门控探测的 `Saved/DreamFX/BuildSafety/NS_Effects1_Mesh.{before,after}.facts` 已包含 ±300 的镜像边界。两侧均有 `bFixedBounds = False`，因此边界被禁用，且源从未声明 `FixedBounds`。重建未改变它，因此门控未看到变化。这既非此处引入，也非 `pull` 的职责；解决它需要决定源是否应声明系统的 `FixedBounds`。
- **幂等性**：第二次 `-Apply` 报告 `DFX7107` 且不写入任何字节。

同一文件也产生了真正的拒绝：

```text
SpawnBurst_Instantaneous.Age is declared but was not written back: the text writes it as a linked parameter, not as a literal, and changing the value MODE is an edit rather than a write-back
```

源中有 `Age = Emitter.Age`，一个链接，而存储中有一个常量。`pull` 报告它而不更改它（`DFX7111`）。

<a id="case-2-破空灰尘--stored-but-undeclared-values"></a>

### 案例 2：`破空灰尘` — 已存储但未声明的值

普通导出在**未**使用 `-NoDefaults` 时，遗漏了八条仅存在于原始文件中的模块输入事实（覆盖范围第 3.4 节和第 6.8 节，第 7 行）：四条 `ScaleColor.Scale RGBA` 事实（两个发射器 × 生成/更新脚本），加上四条 `EmitterState.Min/MaxDistance` 事实。

摘自 `Saved/pull-probe/runs/c2-pull-apply.log`；完整报告对每个未声明输入各有一行：

```text
info DFX7106: pull: ScaleColor.ScaleRGBA is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MaxDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MinDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
...
info DFX7107: ... (19 compared, 94 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 19 value(s) compared, 0 to write, 94 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

- **六条报告行代表那八条事实。** 同一名称出现在 `ParticleSpawnScriptInterpolated` 和 `ParticleUpdateScript` 中且值相等，因此 `pull` 按名称去重。值不相等的重复项会被拒绝（第 6 节）。这些脚本服务于 `ParticleSpawn` 和 `ParticleUpdate`；每条报告行都有自己的 `(line,column)`。
- `ScaleColor.ScaleRGBA` 是 `Scale RGBA` 的源标识符，由 `ToInputIdentifier` 生成。
- **源 SHA256 在空运行和 `-Apply` 之后均未改变**，证明未添加任何结构。它们的摘要仅在 `| dry run` 与 `| applied` 上有所不同。
- 94 个未声明值中的大多数是引擎从默认值物化而来的常量。第 6.8 节指出，37 个受检条目中实际仅丢失了 8 个。决定丢失与否的是门控，而非 `pull`。

<a id="case-3-already-matching-source-clean-mirror-of-atlasfxtemplatesns_atlas2d_mesh"></a>

### 案例 3：已与源匹配（`/AtlasFX/Templates/NS_Atlas2D_Mesh` 的干净镜像）

源就是镜像自身的导出，其中 `Name=` 指向 `/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh`：

```text
info DFX7107: pull: '...NS_Atlas2D_Mesh.dfs' already holds every value
  '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' stores for the input(s) it declares
  (2 compared, 18 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 2 value(s) compared, 0 to write, 18 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

SHA256 未改变。所有 18 个未声明条目都是物化的默认值（`MeshSize`、`MinSize`、`UniformScale`、`MeshYaw = 0` 等）。此模板的 `MeshYaw` 确实为零（覆盖范围第 2.1 节），并且 `pull` 不触碰这些值。

<a id="case-4-gates"></a>

### 案例 4：门控

```text
$ dfx lint -All
  === DreamFX done: 0 built, 0 up to date, 0 failed | 0 error(s), 89 warning(s) ===
  dfx: OK (exit 0)                      # 89 pre-existing content lint warnings
$ dfx corpus
  dfx: corpus OK (55 passed)            # same baseline as the safety-gate pass
$ Build.bat CrossingVoidEditor Win64 Development -MaxParallelActions=2 -WaitMutex
  Result: Succeeded                     # only the existing DreamFXCorpusTest.cpp:572 C4996 remains
```

原始日志：`Saved/pull-probe/runs/c4-lint.log` 和 `c4-corpus.log`。

<a id="6-known-limitations"></a>

## 6. 已知限制

1. **`pull` 读取的是存储，而非引脚。** 对于 `MeshYaw` 之类的输入，二者不一致（覆盖范围第 3.5 节）。`pull` 遵循存储，与 `asset-diff` 和门控保持一致。如果不存在存储条目，例如对于一直处于连接状态的输入，它不会凭空捏造值。
2. **同名但值不同的重复项会被拒绝**（`DFX7111`）。`pull` 无法确定引擎实际读取哪一个；猜测会写入资产不支持的值。
3. **对应关系是强制的。** 手动添加或移除模块会在普通值模式下拒绝该堆栈。拉取前请使用 `build` 对齐源与资产，或使用第 4d 节中无歧义的结构化路径。
4. **节点名称来自资产，无需在源中显式声明。** 当文本必须命名某个身份时，`as <name>` 才有意义，例如 `Output.<node>.<value>`。因此像 `Foo_1` 这样的引擎名称无需额外声明。对应关系依赖同一堆栈、序数和模块资产，与 `build` 完全一致：语句顺序 = 插入顺序 = 节点顺序。在序数 `i` 处拉取的值会在重建时应用于同一序数。
5. **`-NoDefaults` 提供的是键，而非权威值。** 由引脚派生的 `MeshYaw` 初始为 `0.0`；`pull` 会用存储值替换它。此实测导出重建结果为 `1 built, 0 error, 0 warning`，但那是测试结果，并非对每个资产的承诺。如果此类导出包含写入器无法接受的输入，那是 `decompile -NoDefaults`/写入器的问题。普通导出加上手动添加的 `<input> = <current value>` 同样有效且更小。
6. **执行 `-Apply` 后，源的 `Name=` 仍然选择构建目标。** `-Asset` 可以读取原始文件，而 `build` 写入镜像；它绝不意味着“写回原始资产”。
7. **`GetEmitterInfo` 会追加到其输出参数。** 在多个发射器间复用 `FEmitterInfo` 会把第一个发射器的堆栈泄漏到第二个中。`pull` 会为每个发射器创建全新实例。加固该共享适配器被有意排除在本实现阶段之外。
8. **仅处理六个固定堆栈。** 事件/模拟阶段堆栈需要单独的焦点切片机制。渲染器属性、设置、赋值和 `Defaults` 从第二轮处理开始覆盖（第 4b 节）。
9. **结构化编辑仅覆盖模块调用。** “设置参数”的添加/移除会被明确拒绝（第 4d 节）。拆分折叠的赋值是另一种编辑；不正确的边界可能改变哪些条目能看到彼此，且目前没有支持它的测试夹具。
10. **渲染器数量/类型不匹配会拒绝该发射器的渲染器编辑。** 源的 `SpriteRenderer` 块与资产渲染器数量不同，或某位置类型改变，都会产生 `DFX7109`。`-Structure` 不会插入渲染器块。
11. **位置基线无法在渲染器重排序后保持**（第 4c 节）。在此类编辑后，属性可能被视为脏项并被再次写入。
12. **结构后测试保证的是重建时无事实丢失，而非重新编译后的渲染器完全相同。** 语料库断言区分：地址仍存在但事实缺失（失败）；整个地址被移除（源决定，同样被门控规则 1 忽略）；以及地址消失但在移除脚本标签后同一地址处相同值仍存续（重建合并了重复常量）。第三种会被报告但不算失败，因为没有值丢失。安全门控不区分它：这是二者标准中唯一已知的差异。

<a id="5b-second-pass-measurements-2026-10-09-gamedreamfxprobens_pullprobe"></a>

## 5b. 第二轮测量（2026-10-09，`/Game/DreamFXProbe/NS_PullProbe`）

探针使用 `DFX/DreamFXProbe/NS_PullProbe_T1.dfs`（三个模块）和 `_T2.dfs`（四个模块，增加了 `ScaleColor();`），并使用相同的 `Name=`。构建 **T2** 以模拟编辑器添加的模块，然后拉取到 **T1**。两个探针源和探针资产在验证后均已删除。

| # | 命令 | 观察结果 |
|---|---|---|
| 1 | `pull T1`，不带 `-Structure` 的空运行 | `warning DFX7113: pull: Motes.ParticleUpdate: the asset has ScaleColor that the text does not ... so -Structure writes that difference ... Nothing in this stack was addressed.`；`info DFX7107: ... (4 compared, 6 not declared, 0 not writable, 0 withheld). No bytes written.`；**T1 SHA256 未改变** |
| 2 | `pull T1 -Structure -Apply` | `info DFX7112: pull: Motes.ParticleUpdate: +ScaleColor:             ScaleColor@1.1(ScaleRGB = (1.0, 1.0, 1.0), ScaleAlpha = 1.0);`；`=== DreamFX pull: 5 compared, 0 written, 12 not declared, 0 not writable, 0 withheld, 0 stack(s) not addressed, 1 line(s) added, 0 removed, 0 structure(s) refused \| applied \| structure \| baseline recorded ===`；34 → 35 行，**Compare-Object 中仅有该行** |
| 3 | `build T1 -Force`，结构化编辑后 | `=== DreamFX done: 1 built, 0 up to date, 0 failed \| 0 error(s), 1 warning(s) ===`，退出码 0；**无构建安全消息或 DFX8018**，且无 `Saved/DreamFX/BuildSafety/` 目录（仅在门控发现时创建）。唯一的警告是已有的 `DFX7102`，无界 SpawnRate |
| 4 | `pull T1 -Apply`，已对齐 | `info DFX7107: ... (7 compared, 12 not declared, 0 not writable, 0 withheld). No bytes written.`；**SHA256 和 mtime 相同**（`32072A08…` / `16:39:41`），已记录基线 |
| 5 | 将源 `WarmupTime = 0.5` 改为 `0.9`，然后 `pull T1 -Apply` | `info DFX7115: pull: Settings.WarmupTime differs from this text but the asset has not moved since the last apply, so the text's value stands.`；`(7 compared, 12 not declared, 0 not writable, 1 withheld)`；**SHA256 未改变**，仍为 `WarmupTime = 0.9;` |
| 6 | `lint -All` | `0 error(s), 91 warning(s)`，退出码 0；89 条已有树警告加上来自探针文件的两条 |
| 7 | `corpus` | `dfx: corpus OK (64 passed)`，从 55 增加，新增九个 `DreamFX.Corpus.WriteBack` 用例 |

`Tests/Corpus/WriteBack/` 中的九个用例覆盖第 1–5 行，外加**字节完全一致的空操作写回**（`NoOp`，四种类型各一个值；比较实际字节，而不仅仅是编辑计数为零）；`ModuleInput`、`RendererProperty`、`SystemSetting` 和 `Assignment` 的**恰好一行改变**，随后第二次拉取零编辑；以及在三种结构化情况下就地重建后无事实丢失。

<a id="7-reproduction"></a>

## 7. 复现

```powershell
# 0) Ensure no UnrealEditor is running; each command starts one editor process.
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# All four cases; logs go to Saved/pull-probe/runs/.
pwsh -File C:\CrossingVoid\Saved\pull-probe\run-pull-cases.ps1

# Gates
pwsh -File $dfx lint -All -Engine $eng
pwsh -File $dfx corpus -Engine $eng
& 'D:\UnrealEngine-5.8.2\Engine\Build\BatchFiles\Build.bat' CrossingVoidEditor Win64 Development `
    -Project='C:\CrossingVoid\CrossingVoid.uproject' -MaxParallelActions=2 -WaitMutex
```
