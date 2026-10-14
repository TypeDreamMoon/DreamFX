# DreamFX 简体中文文档

[English](../README.md) · 简体中文

从原文档站迁入的文档目前为**待核对的旧译文**，尚未确认与当前英文一致。本次新增译文单独列在下方，待本 PR 审阅。英文仍是技术内容的原文；中文修订由插件仓库中的翻译 PR 审阅，合并后再同步到网站。

网站独有页面和 `docs-sync:notes` 补充保留在网站，不属于这里的翻译。

## 已迁入，待核对

- [更新日志](changelog/index.md)
- [DFX1xxx — 驱动与文件 I/O](diagnostics/DFX1xxx.md)
- [DFX2xxx — 词法与语法](diagnostics/DFX2xxx.md)
- [DFX3xxx — 声明与文档结构](diagnostics/DFX3xxx.md) — 有 10 个编号尚未翻译
- [DFX4xxx — 值、类型与表达式](diagnostics/DFX4xxx.md)
- [DFX5xxx — 生成与资产写入](diagnostics/DFX5xxx.md) — 有 3 个编号尚未翻译
- [DFX6xxx — Niagara 编译](diagnostics/DFX6xxx.md)
- [DFX7xxx — 溯源、漂移与 lint](diagnostics/DFX7xxx.md)
- [DFX8xxx — 反编译器](diagnostics/DFX8xxx.md)
- [DFX3045 · 历史诊断](diagnostics/history/DFX3045.md)
- [DFX5099 · 历史诊断](diagnostics/history/DFX5099.md)
- [能力边界](diagnostics/limitations.md)
- [诊断码](diagnostics/overview.md)
- [Export 与 Adopt](generation/adopt.md)
- [引擎与后端](generation/engines.md)
- [构建流水线](generation/pipeline.md)
- [溯源与 verify](generation/provenance.md)
- [往返与等价](generation/roundtrip.md)
- [DreamFX 是什么](getting-started/about.md)
- [第一个特效](getting-started/first-effect.md)
- [安装与探活](getting-started/installation.md)
- [目录与命名](getting-started/project-layout.md)
- [日常循环](getting-started/workflow.md)
- [.dfe — 可复用 emitter](language/dfe.md)
- [.dfm — 模块与动态输入](language/dfm.md)
- [.dfs — 一个系统](language/dfs.md)
- [事件与 Simulation Stage](language/events-and-stages.md)
- [文件模型](language/README.md)
- [值、类型与规则](language/values.md)
- [概览](overview.md)
- [ci.ps1 与门禁](tools/ci.md)
- [编辑器集成](tools/editor-integration.md)
- [dfx.ps1 命令行](tools/headless-compilation.md)
- [Python 与蓝图接口](tools/scripting.md)
- [AI 技能](tools/skills.md)
- [VSCode 与模块索引](tools/vscode.md)

## 本次新增译文，待本 PR 审阅

- [DFX5015、DFX5020 · 旧链接兼容说明](diagnostics/history/unpublished-codes.md) — 这两个编号不属于当前已发布的 172 条诊断目录。

## 尚未翻译

以下链接指向英文原文；不会把英文副本标为中文译文。

- [diagnostics/README.md](../diagnostics/README.md)
- [getting-started/quickstart.md](../getting-started/quickstart.md)
- [language/native-emitter-inheritance.md](../language/native-emitter-inheritance.md)
- [language/system-inheritance.md](../language/system-inheritance.md)
- [tools/asset-comparison-and-export.md](../tools/asset-comparison-and-export.md)
- [tools/build-on-save.md](../tools/build-on-save.md)
- [tools/pull.md](../tools/pull.md)
- [tools/write-back-coverage.md](../tools/write-back-coverage.md)

## 现有译文中的缺译段落

- [diagnostics/DFX3xxx.md](../diagnostics/DFX3xxx.md)：`DFX3048`、`DFX3049`、`DFX3050`、`DFX3051`、`DFX3052`、`DFX3053`、`DFX3054`、`DFX3055`、`DFX3056`、`DFX3057`
- [diagnostics/DFX5xxx.md](../diagnostics/DFX5xxx.md)：`DFX5027`、`DFX5035`、`DFX5036`

## 翻译记录

`.translations/` 按英文源文件保存迁入记录。`sourceHash: null` 表示尚未核对；`importedSourceHash` 只记录迁入时看到的英文版本，不表示译文已经匹配该版本。这个导航页不是原文翻译，不建立伪造的审阅记录。

本次新增译文记录实际采用的英文全文指纹，随本 PR 审阅；该指纹不表示译文已经合并或发布。

在插件根目录运行 `python -B Docs/translation-status.py` 可只读查看翻译状态；`--all` 列出全部原文，`--json` 输出结构化结果。无需文档站仓库，打包后没有 Git 的插件目录也可以运行。
