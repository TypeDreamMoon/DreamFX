---
title: 贡献指南
description: 仓库结构、两个模块、在测试宿主中构建、ci.ps1 门禁，以及生成文件、文档和发布版本的约定。
---

本页面向修改插件本身的人员。如何报告 bug、拉取请求需要什么，以及 AI 辅助工作的条款见 [CONTRIBUTING.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/CONTRIBUTING.md)。编码代理应从 [AGENTS.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/AGENTS.md) 开始。

| | |
| :-- | :-- |
| 仓库 | <https://github.com/TypeDreamMoon/DreamFX> |
| 已发布引擎 | Unreal Engine 5.8，源码构建和安装版构建 |
| 许可证 | MIT |
| 问题 | [讨论](https://github.com/TypeDreamMoon/DreamFX/discussions) |

<a id="repository-layout"></a>

## 仓库结构

| 路径 | 内容 |
| :-- | :-- |
| `Source/DreamFX/` | 运行时模块：解析器、语言类型、诊断 |
| `Source/DreamFXEditor/` | 编辑器模块：生成、Niagara 适配器、反编译器、lint、schema 探测、commandlet、编辑器 UI、写回、自动化测试 |
| `.skill/` | `dfx.ps1`（无头驱动程序）、`ci.ps1`（门禁）、`gen-diagnostics.ps1`、`sync-skills.ps1`、`close-round.ps1`，以及四个代理技能 |
| `Tests/Corpus/` | 自动化套件的测试夹具：`Parse`、`Generate`、`RoundTrip`、`WriteBack`、`Support`。`*.bad.*` 文件是故意无效的；它们是测试用例，不是示例 |
| `Tests/Tools/` | 无需引擎即可运行的驱动程序和 L3 回归测试 |
| `Tools/TestHost/` | `New-DreamFXTestHost.ps1` 以及最小测试项目的模板 |
| `DFX/` | 插件自带的示例、发射器和模块 |
| `Docs/` | 本手册的英文版；`Docs/zh-CN/` 是中文翻译 |
| `Content/` | 在仓库中为空；构建会将生成的资产写入此处，而这些资产会被 Git 忽略 |

<a id="modules"></a>

## 模块

| 模块 | 类型 | 加载阶段 |
| :-- | :-- | :-- |
| `DreamFX` | 运行时 | Default |
| `DreamFXEditor` | 编辑器 | Default |

描述符启用一个插件依赖项：`Niagara`。同一份源码树既可在源码引擎上构建，也可在安装版引擎上构建；差异在运行时检测，而不是通过 `#if`——参见 [引擎与后端](../generation/engines.md)。

<a id="build-in-a-test-host"></a>

## 在测试宿主中构建

在一个将此仓库作为 git worktree 保存的最小项目中构建和测试，而不是在你日常工作的项目中构建和测试。打开你项目的编辑器会锁定插件的 DLL，并写入与门禁相同的包。宿主只需创建一次：

```powershell
pwsh -NoProfile -File Tools/TestHost/New-DreamFXTestHost.ps1 -Root <HostDir> -RepoPath <RepoDir> -Detach
```

构建编辑器目标：

```powershell
& "<EngineDir>/Engine/Build/BatchFiles/Build.bat" DreamFXTestHostEditor Win64 Development "-Project=<HostDir>/DreamFXTestHost.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
```

切勿构建游戏目标 `DreamFXTestHost`。参数、退出码以及如何试用拉取请求见 [Tools/TestHost/README.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/Tools/TestHost/README.md)。

<a id="the-gate"></a>

## 门禁

在每次拉取请求前，关闭编辑器后运行它：

```powershell
pwsh -NoProfile -File <HostDir>/Plugins/DreamFX/.skill/ci.ps1 -Project <HostDir>/DreamFXTestHost.uproject
```

它会在第一次失败时停止，并在全部通过时以 `ci: OK` 结束。各步骤按顺序如下：

1. `diagnostics`：生成的诊断页面是最新的（`gen-diagnostics.ps1 -Check`）。
2. `driver regressions`: `Tests/Tools/Test-Driver.ps1`.
3. `lint -All`，然后 `build -All -Force`，然后 `verify -All`。
4. `DreamFX automation`：每个 `DreamFX` 自动化套件，包括语料库。

每个步骤会捕获什么，以及有哪些开关，见 [ci.ps1 与门禁](../tools/ci.md)。有两项检查完全不需要引擎：

```powershell
pwsh -NoProfile -File Tests/Tools/Test-Driver.ps1
python -B Tests/Tools/test_l3_equivalence.py
```

<a id="generated-files-and-versions"></a>

## 生成的文件和版本

- **诊断页面。** `Docs/diagnostics/DFX1xxx.md` 到 `DFX8xxx.md` 以及 `Docs/diagnostics/README.md` 是半生成的。添加或更改 `DFXnnnn` 后，运行 `pwsh -NoProfile -File .skill/gen-diagnostics.ps1` 并手动编写原因和修复。切勿编辑 `generated:begin` 和 `generated:end` 标记之间的内容。
- **生成器版本。** 当生成的资产结构发生变化时，在 `Source/DreamFXEditor/Private/Generation/DreamFXProvenance.cpp` 中递增 `FProvenance::GetGeneratorVersion`。之后每个资产都会重新构建一次。
- **技能。** 在 `.skill/` 中编辑它们。`pwsh -File .skill/sync-skills.ps1` 会将它们发布到项目的 `.claude/skills/` 中；`-Check` 会报告偏差。新的 `dfx.ps1` 命令也要加入 `.skill/dream-fx-verify/SKILL.md` 的命令表。

<a id="docs-and-translations"></a>

## 文档和翻译

`Docs/` 下的英文是本站的源内容。`Docs/zh-CN/` 下的中文页面使用 [dreamdocs](https://github.com/TypeDreamMoon/dreamdocs) 逐段翻译：英文更改到达 `main` 后，CI 会翻译更改过的段落并发起拉取请求。要更正翻译，请编辑中文页面；下一次运行会保留该修改。

CI 使用 `dreamdocs check --plugin .` 检查手册，并使用 `Docs/dreamdocs.json` 中指定的 dreamdocs 版本。在该版本上检出 dreamdocs 后，从仓库根目录运行它：

```bash
PYTHONPATH=<dreamdocs checkout> python3 -B -m dreamdocs check --plugin .
```

新页面还需要在文档站点的页面映射中添加一个条目，之后才会出现在站点上。

<a id="changelog-and-releases"></a>

## 变更日志与发布

- `CHANGELOG.md` 用中文撰写。用户可见的变更在 `## Unreleased` 下添加一行。
- 版本号是 `DreamFX.uplugin` 中的 `VersionName`。发布时把 `## Unreleased` 改为 `## <VersionName> — <date>`，并打上 `v<VersionName>` 标签，如同 `v1.0.0` 那样。
- 发布轮次还会运行更深入的检查：`verify -All -StrictVersions`、`mirror-diff`、`asset-diff` 和 `coverage`（参见 [ci.ps1 与 gates](../tools/ci.md#deeper-checks)），或者 `.skill/close-round.ps1`，它会针对同一个二进制运行重建、`mirror-diff` 和语料库。

<a id="reporting-problems"></a>

## 报告问题

缺陷报告通过 issue 表单提交。表单会要求提供以下命令的输出：

```powershell
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 info
```

它会打印插件版本与提交、引擎版本与类型、操作系统、UI 语言以及 PowerShell 版本。它不启动任何东西，也不打印路径，因此可以直接粘贴。
