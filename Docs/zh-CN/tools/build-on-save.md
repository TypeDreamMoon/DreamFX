---
title: "保存时构建与最新状态检查"
description: "保存时构建与最新状态检查"
---

<a id="build-on-save-and-up-to-date-checks"></a>

# 保存时构建与最新状态检查

在编辑器中保存源文件默认使用增量构建。本页保留现有的网站地址 `plugin/tools/incremental-builds` 及其章节链接。完整的已发布参考为[增量构建](incremental-builds.md)。

<a id="incremental-builds"></a>

## 已发布的构建范围

保存操作会将新的构建计划与上一次成功构建存储的摘要进行比较，并仅写入差异部分。如果存储的状态不可信，则回退为完整重建。显式的编辑器重建命令以及不带 `-Incremental` 的命令行构建使用完整构建。

<a id="how-it-works"></a>

## 构建顺序

生成器解析源文件及其依赖项，将生成的计划与存储的摘要进行比较，应用所需的写入，验证 Niagara 编译，标记来源信息，并在验证允许的情况下保存。请参阅[增量构建的工作原理](incremental-builds.md#how-it-works)以及[构建管线](../generation/pipeline.md)。

<a id="saving-source-in-the-editor"></a>

## 在编辑器中保存源文件

监视器将更改的 `.dfs`、`.dfe` 和 `.dfm` 源文件排队，并先构建依赖项再构建其使用者。保存触发的系统重建默认使用增量生成。**Rebuild DFX**、**Rebuild from Source** 以及批量批处理 **Rebuild them now** 操作始终执行完整重建。超过 **Bulk Rebuild Threshold**（默认 **8**）的批次会等待确认；请参阅[批量批处理门控](editor-integration.md#the-bulk-batch)。

<a id="switching-it-on-and-off"></a>

## 可用的控制项

**Project Settings → Plugins → DreamFX → Workspace → Incremental Build on Save**（`bIncrementalBuildOnSave`，默认开启）控制保存触发的增量构建。命令行构建通过 `-Incremental` 选择启用，该项默认关闭，不应在 CI 门控中使用。请参阅[控制项表](incremental-builds.md#switching-it-on-and-off)。

<a id="unchanged-sources-can-be-skipped"></a>

## 未更改的源文件可以跳过

当资产的来源信息与其源文件匹配时，普通的无头构建可以报告 `up to date`。跳过构建与增量应用更改后的计划是两种独立的操作。

```powershell
# Build if the generated asset is out of date, using a full build.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
# Opt in to incremental generation for a local edit loop.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs -Incremental
# Request a full rebuild even when provenance matches.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs -Force
# Check provenance without repairing the asset.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 verify DFX/Effects/NS_Hello.dfs
```

在写入包的的无头命令之前关闭编辑器。有关源位置规则，请参阅[来源信息与验证](../generation/provenance.md)。

<a id="what-each-kind-of-edit-costs"></a>

## 源文件更改与构建成本

值编辑可以仅写入更改的输入。结构性更改可能会重建堆栈、发射器或整个系统；引擎的写入路径也会影响支持哪些部分编辑。有关已发布的测量数据和范围，请参阅[逐编辑表](incremental-builds.md#what-each-kind-of-edit-costs)。

<a id="compile"></a>

### Niagara 编译

增量构建请求普通的、非强制性的 Niagara 编译；完整重建则强制编译。两者都会验证生成的可执行数据，如果数据过时则强制重新编译。请参阅[无头编译](headless-compilation.md)。

<a id="module-probes-are-made-once"></a>

### 模块架构探测

规划过程为每个进程复用一个模块库，因此所需的探测每个编辑器会话或命令程序运行仅执行一次。首次构建可能较慢。使用 `dfx.ps1 schema <Module>` 检查真实的输入；请参阅[模块探测](incremental-builds.md#module-probes-are-made-once)。

<a id="when-it-falls-back-to-a-full-rebuild"></a>

## 完整重建行为

新资产、缺失或不兼容的摘要、外部更改的资产事实以及不支持的结构性更改都会触发完整重建。失败的增量写入也会在同一次构建中回退。Output Log 会记录原因；请参阅完整的[回退表](incremental-builds.md#when-it-falls-back-to-a-full-rebuild)。

<a id="reading-the-log"></a>

## 解读构建输出

监视器和命令程序会报告增量写入计数或每个文件的 `full rebuild (<reason>)`。`up to date` 表示来源信息跳过。请参阅[日志示例](incremental-builds.md#reading-the-log)。

<a id="protecting-editor-authored-values"></a>

## 保护编辑器编写的值

构建安全门控可以拒绝会丢失未声明资产状态的保存（`DFX8018`）。显式的 CLI `-Force` 可以在有警告的情况下允许该丢失；编辑器重建请求不会绕过该门控。在重建之前，使用 [pull](pull.md) 或 [Write Back to Source](editor-integration.md#write-back-to-source) 将符合条件的已调整资产值带入源文件。

<a id="verification"></a>

## 验证

CI 门控应使用完整构建路径：lint、build、verify 和 corpus。保持 `-Incremental` 不进入门控。请参阅[命令行](headless-compilation.md)和 [CI 序列](ci.md)。

<a id="the-equivalence-guarantee"></a>

### 这些检查确立了哪些内容

增量生成必须对同一文本产生与完整重建相同的资产。已发布的[增量等价性测试](incremental-builds.md#the-equivalence-guarantee)会比较资产事实、节点名称、输入和反编译文本。运行时视觉等价性是一项单独的检查；请参阅[往返与等价性](../generation/roundtrip.md)。

<a id="troubleshooting"></a>

## 诊断重建

阅读 `full rebuild (<reason>)` 行以了解意外的回退。在 DreamFX 之外编辑的资产在进行下一次可信的增量构建之前需要完整重建。如果资产看起来有问题，使用 **Rebuild from Source** 获取完整构建结果；如果两者结果不一致，请保留两者。请参阅[故障排除](incremental-builds.md#troubleshooting)。

<a id="see-also"></a>

## 相关参考

- [增量构建](incremental-builds.md)。
- [编辑器集成](editor-integration.md)。
- [无头编译](headless-compilation.md)。
- [来源信息与验证](../generation/provenance.md)。
- [诊断目录](../diagnostics/README.md)。
