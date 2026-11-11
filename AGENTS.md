# DreamFX — agent guide

DreamFX is an Unreal Engine plugin that generates Niagara assets from text.
The text language is DreamFXLang: `.dfs` (system), `.dfe` (reusable emitter), `.dfm` (module or dynamic input).
A build turns each source into a standard `UNiagaraSystem`, `UNiagaraEmitter` or `UNiagaraScript` asset.

Two kinds of work happen here. Pick the section that matches yours.

- **Authoring `.dfs` / `.dfe` / `.dfm` in a game project** that has this plugin installed → [Authoring](#authoring).
- **Changing the plugin itself** (C++, tools, docs) → [Changing the plugin](#changing-the-plugin).

## Authoring

DreamFXLang is not in your training data. Read the language reference and start from its examples.
Do not guess syntax from HLSL, C++ or other Niagara tools. Do not copy from `Tests/Corpus/`: those are
parser and generator test cases, and some are wrong on purpose.
Commands below run from the game project root; the plugin sits at `Plugins/DreamFX/`.

1. Read: `Plugins/DreamFX/Docs/getting-started/quickstart.md` and `Plugins/DreamFX/Docs/language/README.md`.
2. Look up what exists. **Do this before writing any module call.** Module input names come from real
   assets (spaces, switch-revealed inputs, per-stack inputs). Nobody can guess them.
   - List modules: `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list`
   - List dynamic inputs: `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list -DynamicInputs`
   - Read one module's real inputs, once per module you call: `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce`
3. Write the source under the project's `DFX/` folder, for example `DFX/Effects/NS_Spark.dfs`.
   `Name=` and `Root=` in the header decide the asset path (see `Plugins/DreamFX/Docs/getting-started/project-layout.md`).
4. Build it (editor closed): `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Spark.dfs -Force`
   For a quick check with no asset access first: `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 lint DFX/Effects/NS_Spark.dfs`
5. Read the diagnostics. Each has a code `DFXnnnn`, a line and a column. Look the code up in
   `Plugins/DreamFX/Docs/diagnostics/README.md`. Fix, and repeat until the run ends with `dfx: OK (exit 0)`.

### Skills

Four skills ship in `Plugins/DreamFX/.skill/`. Each is a plain `SKILL.md`; any agent can read it.

| Skill | Use it when |
| --- | --- |
| `dream-fx-create` | writing a new effect from a description, then building it |
| `dream-fx-verify` | building or checking one file or the whole `DFX/` tree headlessly |
| `dream-fx-diagnose` | a build failed with a `DFXnnnn` code and you need the cause and fix |
| `dream-fx-decompile` | exporting an existing Niagara system to `.dfs` |

Claude Code loads skills from `.claude/skills/`. Copy them there with
`pwsh -File Plugins/DreamFX/.skill/sync-skills.ps1` (re-run after updating the plugin; `-Check` reports drift).
Other agents: open the `SKILL.md` files directly.

### Rules

- Run `schema` for every module you call. A guessed input name fails with `DFX3003`.
- Close the editor before `build`, `corpus`, `mirror-diff` and `decompile-all`. Two processes saving one package race silently.
- Pass `-Force` when you need proof. Without it an unchanged source reports `up to date` and proves nothing.
- The exit code is the error count. `0` is green; anything else is not done.
- Each run boots the engine. Edit several files, then build once (`build -All -Force`), instead of looping per file.
- The `.dfs` is the source. Never edit a generated asset and expect it to survive the next build.
- To rename an emitter, run `dfx.ps1 rename <asset>:<old>:<new>` first, then change the source (see `dream-fx-verify`).
- The project must enable the same content plugins as the effects use (for example `NiagaraFluids`), or their modules are invisible.

## Changing the plugin

### Layout

| Path | Kind | What it holds |
| --- | --- | --- |
| `Source/DreamFX/` | Runtime module | parser, language types, diagnostics |
| `Source/DreamFXEditor/` | Editor module | generation, Niagara adapter, decompiler, lint, schema probe, commandlet, editor UI, write-back, automation tests |
| `Source/DreamFXEditor/Private/Compat/` | Editor | the Niagara edit API for engines before 5.8 (not released yet) |
| `.skill/` | tools | `dfx.ps1` driver, `ci.ps1` gate, `gen-diagnostics.ps1`, `sync-skills.ps1`, the four skills |
| `Tests/Corpus/` | tests | fixtures for the automation suites; `*.bad.*` files are invalid on purpose, not examples |
| `Tests/Tools/` | tests | driver and L3 regressions that need no engine |
| `Tools/TestHost/` | tools | creates a minimal test project with this repository as a worktree |
| `DFX/` | sources | the plugin's own samples, emitters and modules |
| `Docs/` | docs | English manual; `Docs/zh-CN/` is the translation |

### Build

Build in a test host, not in a project you work in. Create it once (see `Tools/TestHost/README.md`):

```powershell
pwsh -NoProfile -File Tools/TestHost/New-DreamFXTestHost.ps1 -Root <HostDir> -RepoPath <RepoDir> -Detach
& "<EngineDir>/Engine/Build/BatchFiles/Build.bat" DreamFXTestHostEditor Win64 Development "-Project=<HostDir>/DreamFXTestHost.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
```

### Test

Before a pull request, run the full gate with the editor closed. It boots the editor several times.

```powershell
pwsh -NoProfile -File <HostDir>/Plugins/DreamFX/.skill/ci.ps1 -Project <HostDir>/DreamFXTestHost.uproject
```

It must end with `ci: OK`. It runs: diagnostics drift, driver regressions, `lint -All`, `build -All -Force`,
`verify -All`, and every `DreamFX` automation suite. Details: `Docs/tools/ci.md`.
Checks that need no engine: `pwsh -NoProfile -File Tests/Tools/Test-Driver.ps1` and `python -B Tests/Tools/test_l3_equivalence.py`.

### Rules

- Diagnostic pages are half generated. After adding or changing a `DFXnnnn`, run
  `pwsh -NoProfile -File .skill/gen-diagnostics.ps1` and commit the result. Write only the Cause and Fix
  text, never between the `generated:begin` / `generated:end` markers.
- If the emitted asset shape changes, bump the generator version in `FProvenance::GetGeneratorVersion`
  (`Source/DreamFXEditor/Private/Generation/DreamFXProvenance.cpp`) and say so in `CHANGELOG.md`.
- Released engine: Unreal Engine 5.8, source build and installed build, from one source tree. Do not split
  those two with `#if`; detect at run time (`Docs/generation/engines.md`). Pre-5.8 paths use `UE_VERSION_OLDER_THAN` and `Compat/`.
- Edit skills in `.skill/`, never the published copy under `.claude/skills/`.
- No absolute local paths in committed files. Write `Plugins/DreamFX/...` or `<HostDir>`.
- One topic per commit. Subjects follow `git log --oneline`: `fix(area): ...`, `feat(area): ...`, `docs: ...`.
- User-visible changes get a line under `## Unreleased` in `CHANGELOG.md` (written in Chinese, like the rest of it).
- Behaviour changed → update the English page in `Docs/`. Do not hand-translate `Docs/zh-CN/`; CI does it.

### Where things are documented

- Contributing overview: `Docs/contributing/index.md` and `CONTRIBUTING.md`
- CI gate: `Docs/tools/ci.md`; test host: `Tools/TestHost/README.md`
- Headless command line: `Docs/tools/headless-compilation.md`; skills: `Docs/tools/skills.md`
- Language reference: `Docs/language/README.md`; diagnostics: `Docs/diagnostics/overview.md`
- Engines and backends: `Docs/generation/engines.md`
- Documentation site: <https://fx.toolchain.64hz.cn>

## Don't

- Don't run `build -All` or `ci.ps1` in a user's project while its editor is open.
- Don't hand-edit generated diagnostic sections or generated `.uasset` files.
- Don't commit build products: `Content/Samples/`, `Content/Modules/`, `Content/Corpus/`, `DFX/.dfx-index.json`.
- Don't use `-Incremental`, `-NoShaderCompile` or `-PrecompiledShadersOnly` in a gate.
- Don't open a pull request without the `ci: OK` line from the gate.
