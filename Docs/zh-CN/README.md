---
title: "DreamFX 文档"
description: "DreamFX 文档"
---

<a id="dreamfx-documentation"></a>

# DreamFX 文档

[English](../README.md) · 简体中文

这些英文 Markdown 文档是插件及其[文档站](https://fx.toolchain.64hz.cn/en/docs)维护的原文。请在此处编辑源文件。网站会将这些文件映射到稳定的页面 URL，并发布两种语言。中文翻译的 pull request 在本插件仓库中发起和审阅。移动源文件不需要更改其公开 URL。

<a id="getting-started"></a>

## 入门

- [概览](overview.md)：文件类型、编写模型和支持的功能。
- [DreamFX 是什么](getting-started/about.md)：目标与边界。
- [安装](getting-started/installation.md)：前置条件和探活检查。
- [第一个特效](getting-started/first-effect.md)：引导式的第一个系统。
- [快速上手](getting-started/quickstart.md)：精简的端到端演练。
- [项目结构与命名](getting-started/project-layout.md)：根目录、路径和源代码管理。
- [日常流程](getting-started/workflow.md)：编辑器迭代与无头检查。

<a id="language"></a>

## 语言

- [文件模型与语言规则](language/README.md)。
- [系统（`.dfs`）](language/dfs.md)、[可复用发射器（`.dfe`）](language/dfe.md)，以及
  [模块与动态输入（`.dfm`）](language/dfm.md)。
- [值与类型](language/values.md)。
- [事件与模拟阶段](language/events-and-stages.md)。
- [系统源继承](language/system-inheritance.md)。
- [原生发射器继承](language/native-emitter-inheritance.md)。

<a id="generation-and-round-trip"></a>

## 生成与往返

- [构建流水线](generation/pipeline.md)。
- [溯源与验证](generation/provenance.md)。
- [往返与等价](generation/roundtrip.md)。
- [Export 与 Adopt](generation/adopt.md)。
- [引擎与后端](generation/engines.md)。

<a id="tools"></a>

## 工具

- [无头命令行](tools/headless-compilation.md)和 [CI 门禁](tools/ci.md)。
- [编辑器集成](tools/editor-integration.md)、[增量构建](tools/incremental-builds.md)，
  以及[保存时构建与最新性检查](tools/build-on-save.md)。
- [资产比较与导出](tools/asset-comparison-and-export.md)。
- [将资产值拉取到源代码](tools/pull.md)。
- [写回覆盖研究](tools/write-back-coverage.md)：注明日期的证据和实现
  后续工作；历史限制与后续修复分开记述。
- [Python 与蓝图](tools/scripting.md)。
- [VSCode 与模块索引](tools/vscode.md)。
- [Agent 技能](tools/skills.md)。

<a id="diagnostics-and-releases"></a>

## 诊断与发布

- [诊断工作流与查询](diagnostics/overview.md)。
- [生成的诊断参考](diagnostics/README.md)，包含 `DFX1xxx.md` 至
  `DFX8xxx.md` 系列。在区分大小写的平台上请保留准确的文件名大小写。
- [限制与已知问题](diagnostics/limitations.md)。
- [发布概览](changelog/index.md)和[插件更新日志](../../CHANGELOG.md)。

<a id="maintenance"></a>

## 维护

使用普通 Markdown，保留代码和 API 标识符，并使用相对路径链接到其他源文件。网站呈现属于站点渲染器的职责。原先的 `Docs/Website/`
内容现在位于上述主题目录中。

诊断系列文件和 `diagnostics/README.md` 由插件的诊断
工具生成；请遵循其生成文件说明，更新所属的数据/生成器，而不是
编辑其生成的部分。手工编写的诊断概览单独维护。

网站的 `docs-sync/config.json` 会将源文件映射到现有页面 ID，包括旧版
`plugin/` 路由。重组源路径时请保留这些 ID。此导航文件被排除
在自动页面发现之外，因此不会创建另一个网站路由。

`zh-CN/` 目录树镜像英文源路径。导入的翻译未经审阅；[TRANSLATIONS.md 快照](TRANSLATIONS.md)保留了初始迁移清单，`.translations/` 记录源版本，但不会把导入视为批准。仅网站使用的说明留在网站仓库中。

在本地用 `python -B Docs/translation-status.py` 检查翻译状态；添加 `--all` 以包含所有源文件，或使用 `--json` 获取结构化输出。此只读工具不需要网站仓库或 Git 检出。
