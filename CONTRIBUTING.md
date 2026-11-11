# Contributing to DreamFX

[English](CONTRIBUTING.md) · [简体中文](CONTRIBUTING.zh-CN.md)

Thank you for helping. This page says what to send, how to test it, and what a pull request needs.
Using a coding agent? Have it read [AGENTS.md](AGENTS.md) first.

## Ways to contribute

- **Bug reports**: wrong output, a crash, or behaviour that differs from the docs.
- **Feature or syntax proposals**: something DreamFXLang cannot express yet.
- **Docs and translation fixes**: wrong, unclear or missing text, in English or Chinese.
- **Small fixes**: a typo, a clear bug with a small patch. Open the pull request directly.
- **Feature pull requests**: open a proposal issue first and agree on the design there.

## Reporting a bug

1. Use the [issue forms](https://github.com/TypeDreamMoon/DreamFX/issues/new/choose).
2. Run this from the game project root and paste the output into the form:

   ```powershell
   pwsh -File Plugins/DreamFX/.skill/dfx.ps1 info
   ```

   It starts nothing and prints no paths.
3. Give the smallest `.dfs` / `.dfe` / `.dfm` that shows the problem and builds on its own.
4. Add the diagnostic code (`DFXnnnn`) and the few log lines around it.

If an AI tool wrote the source, use the **AI wrote it wrong** form instead.

## Questions and ideas

Ask in [Discussions](https://github.com/TypeDreamMoon/DreamFX/discussions): usage questions, design
ideas, and effects you made. Issues are for bugs and concrete proposals.

## Development setup

You need Windows, Unreal Engine 5.8 (source or installed build), PowerShell 7 (`pwsh`), git, and
Python 3 for one test.

Build and test in a separate test host, not in a project you work in:

```powershell
pwsh -NoProfile -File Tools/TestHost/New-DreamFXTestHost.ps1 -Root <HostDir> -RepoPath <RepoDir> -Detach
& "<EngineDir>/Engine/Build/BatchFiles/Build.bat" DreamFXTestHostEditor Win64 Development "-Project=<HostDir>/DreamFXTestHost.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
```

`<HostDir>/Plugins/DreamFX` is a git worktree of your clone. Details: [Tools/TestHost/README.md](Tools/TestHost/README.md)
and the [contributing overview](Docs/contributing/index.md).

## Before you open a PR

Run the gate with the editor closed. It must end with `ci: OK`:

```powershell
pwsh -NoProfile -File <HostDir>/Plugins/DreamFX/.skill/ci.ps1 -Project <HostDir>/DreamFXTestHost.uproject
```

Update these in the same pull request when they apply:

- `Docs/`: the English page for any behaviour you changed.
- `CHANGELOG.md`: one line under `## Unreleased` for a user-visible change.
- Diagnostic pages: after adding or changing a `DFXnnnn`, run
  `pwsh -NoProfile -File .skill/gen-diagnostics.ps1` and write its Cause and Fix.
- Generator version: if the generated asset changes shape, bump `FProvenance::GetGeneratorVersion`.
- Command lists: a new `dfx.ps1` command also goes into `.skill/dream-fx-verify/SKILL.md` and the README tooling table.

## Commits and pull requests

- Small commits, one topic each. Match the subjects in `git log --oneline`:
  `fix(area): ...`, `feat(area): ...`, `docs: ...`, `ci: ...`.
- Branch names follow the same kinds: `fix/<topic>`, `feat/<topic>`, `docs/<topic>`, `ci/<topic>`.
- No absolute local paths, user names or e-mail addresses in committed files.
- Fill in the pull request template. Link the issue (`Fixes #123`).

## AI-assisted contributions

AI-assisted pull requests are welcome, on the same terms as any other:

- Say in the pull request which tool and model you used.
- Paste the gate's result line (`ci: OK`) from your own run.
- You are responsible for every line. Read and test it before you submit.
- Bulk AI pull requests without test evidence will be closed.
- Let the agent read [AGENTS.md](AGENTS.md) before it starts.

## Docs and translations

- English in `Docs/` is the source. Edit it there; the [documentation site](https://fx.toolchain.64hz.cn) publishes it.
- `Docs/zh-CN/` is translated paragraph by paragraph with [dreamdocs](https://github.com/TypeDreamMoon/dreamdocs).
  After English changes reach `main`, CI translates the changed paragraphs and opens a pull request.
- Fixing a Chinese translation: edit the Chinese page directly. The next run keeps your edit.
- `README.zh-CN.md` and `CHANGELOG.md` (written in Chinese) are maintained by hand.
- CI checks the docs with `dreamdocs check --plugin .`. See [Docs/README.md](Docs/README.md).

## License

DreamFX is under the [MIT License](LICENSE). By contributing you agree that your contribution is
licensed under the same terms. There is no CLA.

## Code of conduct and security

- [Code of conduct](https://github.com/TypeDreamMoon/.github/blob/main/CODE_OF_CONDUCT.md)
- [Security policy](https://github.com/TypeDreamMoon/.github/blob/main/SECURITY.md)
