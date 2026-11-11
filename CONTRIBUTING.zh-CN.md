# 参与 DreamFX

[English](CONTRIBUTING.md) · [简体中文](CONTRIBUTING.zh-CN.md)

感谢你愿意帮忙。本页说明可以提交什么、怎么测试、拉取请求(PR)需要带什么。
用编程 agent？先让它读 [AGENTS.md](AGENTS.md)。

## 可以怎么参与

- **报告问题**：结果不对、崩溃，或行为与文档不符。
- **功能或语法提案**：DreamFXLang 现在还表达不了的东西。
- **文档与翻译修正**：英文或中文里有错、看不懂或缺内容的地方。
- **小修**：错别字、改动很小的明确 bug。直接开 PR 即可。
- **功能 PR**：先开提案 issue，在那里把设计对齐。

## 报告问题

1. 使用 [Issue 表单](https://github.com/TypeDreamMoon/DreamFX/issues/new/choose)。
2. 在游戏工程根目录运行下面这条命令，把输出贴进表单：

   ```powershell
   pwsh -File Plugins/DreamFX/.skill/dfx.ps1 info
   ```

   它不启动引擎，也不打印任何路径。
3. 附上能独立编译、能复现问题的最小 `.dfs` / `.dfe` / `.dfm`。
4. 写上诊断码(`DFXnnnn`)和报错附近的几行日志。

如果源码是 AI 工具写的，请改用 **AI 写错了** 表单。

## 提问与想法

去 [Discussions](https://github.com/TypeDreamMoon/DreamFX/discussions):用法提问、设计想法、作品展示。
Issue 只用于 bug 和具体提案。

## 开发环境

需要 Windows、Unreal Engine 5.8(源码版或安装版)、PowerShell 7(`pwsh`)、git，
以及跑其中一项测试用的 Python 3。

在单独的测试宿主里构建和测试，不要在你日常工作的工程里做：

```powershell
pwsh -NoProfile -File Tools/TestHost/New-DreamFXTestHost.ps1 -Root <HostDir> -RepoPath <RepoDir> -Detach
& "<EngineDir>/Engine/Build/BatchFiles/Build.bat" DreamFXTestHostEditor Win64 Development "-Project=<HostDir>/DreamFXTestHost.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
```

`<HostDir>/Plugins/DreamFX` 是你本地仓库的一个 git worktree。细节见
[Tools/TestHost/README.md](Tools/TestHost/README.md) 和[贡献总览](Docs/contributing/index.md)。

## 开 PR 之前

关掉编辑器后跑一遍闸门，结尾必须是 `ci: OK`:

```powershell
pwsh -NoProfile -File <HostDir>/Plugins/DreamFX/.skill/ci.ps1 -Project <HostDir>/DreamFXTestHost.uproject
```

下面这些如果涉及，请在同一个 PR 里一起更新：

- `Docs/`:你改了哪个行为，就改对应的英文页。
- `CHANGELOG.md`:用户能感知的改动，在 `## Unreleased` 下加一行。
- 诊断页：新增或修改了 `DFXnnnn` 后，运行
  `pwsh -NoProfile -File .skill/gen-diagnostics.ps1`，并写上 Cause 和 Fix。
- 生成器版本：生成的资产结构变了，就升 `FProvenance::GetGeneratorVersion`。
- 命令列表：`dfx.ps1` 新增命令，同步写进 `.skill/dream-fx-verify/SKILL.md` 和 README 的工具表。

## 提交与 PR

- 提交要小，一个提交一个主题。说明照 `git log --oneline` 的风格：
  `fix(area): ...`、`feat(area): ...`、`docs: ...`、`ci: ...`。
- 分支名用同样的类别：`fix/<主题>`、`feat/<主题>`、`docs/<主题>`、`ci/<主题>`。
- 入库文件里不要出现本机绝对路径、用户名或邮箱。
- 按模板填写 PR 描述，并关联 issue(`Fixes #123`)。

## AI 辅助贡献

欢迎 AI 辅助的 PR，要求和其他 PR 一样：

- 在 PR 里写明用了什么工具和模型。
- 贴上你自己跑闸门的结果行(`ci: OK`)。
- 每一行都由你负责。提交前自己读过、测过。
- 没有测试证据的批量 AI PR 会被关闭。
- 开始之前先让 agent 读 [AGENTS.md](AGENTS.md)。

## 文档与翻译

- `Docs/` 里的英文是原文。在那里改；[文档站](https://fx.toolchain.64hz.cn)发布它。
- `Docs/zh-CN/` 由 [dreamdocs](https://github.com/TypeDreamMoon/dreamdocs) 按段落翻译。
  英文改动合进 `main` 后，CI 翻译改动的段落并开一个 PR。
- 修正中文翻译：直接改中文页。下一次运行会保留你的修改。
- `README.zh-CN.md` 和 `CHANGELOG.md`(用中文写)是手工维护的。
- CI 用 `dreamdocs check --plugin .` 检查文档。见 [Docs/README.md](Docs/README.md)。

## 许可证

DreamFX 使用 [MIT 许可证](LICENSE)。你提交贡献，即表示同意它按同样的条款授权。不签 CLA。

## 行为准则与安全

- [行为准则](https://github.com/TypeDreamMoon/.github/blob/main/CODE_OF_CONDUCT.md)
- [安全策略](https://github.com/TypeDreamMoon/.github/blob/main/SECURITY.md)
