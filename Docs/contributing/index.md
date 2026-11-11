---
title: Contributing
description: Repository layout, the two modules, building in a test host, the ci.ps1 gate, and the conventions for generated files, docs and releases.
---

This page is for people who change the plugin itself. How to report a bug, what a pull request needs,
and the terms for AI-assisted work are in
[CONTRIBUTING.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/CONTRIBUTING.md). A coding agent
should start from [AGENTS.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/AGENTS.md).

| | |
| :-- | :-- |
| Repository | <https://github.com/TypeDreamMoon/DreamFX> |
| Released engine | Unreal Engine 5.8, source build and installed build |
| License | MIT |
| Questions | [Discussions](https://github.com/TypeDreamMoon/DreamFX/discussions) |

## Repository layout

| Path | Contents |
| :-- | :-- |
| `Source/DreamFX/` | Runtime module: parser, language types, diagnostics |
| `Source/DreamFXEditor/` | Editor module: generation, Niagara adapter, decompiler, lint, schema probe, commandlet, editor UI, write-back, automation tests |
| `.skill/` | `dfx.ps1` (the headless driver), `ci.ps1` (the gate), `gen-diagnostics.ps1`, `sync-skills.ps1`, `close-round.ps1`, and the four agent skills |
| `Tests/Corpus/` | Fixtures for the automation suites: `Parse`, `Generate`, `RoundTrip`, `WriteBack`, `Support`. `*.bad.*` files are invalid on purpose; they are test cases, not examples |
| `Tests/Tools/` | Driver and L3 regressions that run without the engine |
| `Tools/TestHost/` | `New-DreamFXTestHost.ps1` and the template of a minimal test project |
| `DFX/` | The plugin's own samples, emitters and modules |
| `Docs/` | This manual in English; `Docs/zh-CN/` is the Chinese translation |
| `Content/` | Empty in the repository; builds write generated assets here, and they are gitignored |

## Modules

| Module | Type | Loading phase |
| :-- | :-- | :-- |
| `DreamFX` | Runtime | Default |
| `DreamFXEditor` | Editor | Default |

The descriptor enables one plugin dependency, `Niagara`.
One source tree builds on a source engine and on the installed engine; the difference is detected at
run time, not with `#if` — see [Engines and backends](../generation/engines.md).

## Build in a test host

Build and test in a minimal project that holds this repository as a git worktree, not in a project you
work in. An editor open on your project locks the plugin's DLLs and writes the same packages as the
gate. Create the host once:

```powershell
pwsh -NoProfile -File Tools/TestHost/New-DreamFXTestHost.ps1 -Root <HostDir> -RepoPath <RepoDir> -Detach
```

Build the editor target:

```powershell
& "<EngineDir>/Engine/Build/BatchFiles/Build.bat" DreamFXTestHostEditor Win64 Development "-Project=<HostDir>/DreamFXTestHost.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
```

Never build the game target `DreamFXTestHost`. The parameters, exit codes and how to try a pull request
are in [Tools/TestHost/README.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/Tools/TestHost/README.md).

## The gate

Run it before every pull request, with the editor closed:

```powershell
pwsh -NoProfile -File <HostDir>/Plugins/DreamFX/.skill/ci.ps1 -Project <HostDir>/DreamFXTestHost.uproject
```

It stops at the first failure and ends with `ci: OK` when everything passed. The steps, in order:

1. `diagnostics`: the generated diagnostic pages are current (`gen-diagnostics.ps1 -Check`).
2. `driver regressions`: `Tests/Tools/Test-Driver.ps1`.
3. `lint -All`, then `build -All -Force`, then `verify -All`.
4. `DreamFX automation`: every `DreamFX` automation suite, the corpus included.

What each step catches, and the switches, are in [ci.ps1 and gates](../tools/ci.md).
Two checks need no engine at all:

```powershell
pwsh -NoProfile -File Tests/Tools/Test-Driver.ps1
python -B Tests/Tools/test_l3_equivalence.py
```

## Generated files and versions

- **Diagnostic pages.** `Docs/diagnostics/DFX1xxx.md` to `DFX8xxx.md` and `Docs/diagnostics/README.md`
  are half generated. After adding or changing a `DFXnnnn`, run
  `pwsh -NoProfile -File .skill/gen-diagnostics.ps1` and write the Cause and Fix by hand. Never edit
  between the `generated:begin` and `generated:end` markers.
- **Generator version.** When the generated asset changes shape, bump `FProvenance::GetGeneratorVersion`
  in `Source/DreamFXEditor/Private/Generation/DreamFXProvenance.cpp`. Every asset then rebuilds once.
- **Skills.** Edit them in `.skill/`. `pwsh -File .skill/sync-skills.ps1` publishes them into a
  project's `.claude/skills/`; `-Check` reports drift. A new `dfx.ps1` command also goes into the
  command table of `.skill/dream-fx-verify/SKILL.md`.

## Docs and translations

English under `Docs/` is the source of this site. Chinese pages under `Docs/zh-CN/` are translated
paragraph by paragraph with [dreamdocs](https://github.com/TypeDreamMoon/dreamdocs): after English
changes reach `main`, CI translates the changed paragraphs and opens a pull request. Correct a
translation by editing the Chinese page; the next run keeps the edit.

CI checks the manuals with `dreamdocs check --plugin .`, using the dreamdocs version named in
`Docs/dreamdocs.json`. With a dreamdocs checkout at that version, run it from the repository root:

```bash
PYTHONPATH=<dreamdocs checkout> python3 -B -m dreamdocs check --plugin .
```

A new page also needs an entry in the documentation site's page map before it appears on the site.

## Changelog and releases

- `CHANGELOG.md` is written in Chinese. A user-visible change adds one line under `## Unreleased`.
- The version is `VersionName` in `DreamFX.uplugin`. A release turns `## Unreleased` into
  `## <VersionName> — <date>` and is tagged `v<VersionName>`, as `v1.0.0` was.
- A release round runs the deeper checks as well: `verify -All -StrictVersions`, `mirror-diff`,
  `asset-diff` and `coverage` (see [ci.ps1 and gates](../tools/ci.md#deeper-checks)), or
  `.skill/close-round.ps1`, which runs the rebuild, `mirror-diff` and the corpus against one binary.

## Reporting problems

Bug reports go through the issue forms. They ask for the output of:

```powershell
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 info
```

It prints the plugin version and commit, the engine version and kind, the OS, the UI language and the
PowerShell version. It starts nothing and prints no paths, so it can be pasted as it is.
