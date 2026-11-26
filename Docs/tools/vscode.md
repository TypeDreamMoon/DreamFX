---
title: VSCode and the module index
description: What the workspace file contains, how VSCode is discovered, the rules for jump links, what the extension reads in the DreamSource layout, and why the module index probes input signatures.
---

## Open DreamFX Workspace

*Tools ▸ DreamFX ▸ Open DreamFX Workspace (VSCode)* rewrites `DreamFX.code-workspace` in the project's
source directory (`DreamSource/FX/`, see [the DreamSource layout](../getting-started/project-layout.md#the-dreamsource-layout)),
then launches it through VSCode → the OS default editor → Notepad.

```json
{
  "folders": [
    { "name": "DreamFX Source", "path": "." },
    { "name": "Plugin: DreamFX", "path": "../../Plugins/DreamFX/DreamSource/FX" }
  ],
  "settings": {
    "files.associations": { "*.dfs": "dreamfxlang", "*.dfe": "dreamfxlang", "*.dfm": "dreamfxlang" }
  },
  "extensions": {
    "recommendations": ["typedreammoon.dreamfxlang-language-support"]
  }
}
```

> **Warning:**
>
> **The file is fully rewritten every time, never merged.** Hand-added `launch` or `tasks` blocks are
> lost. Per-user configuration belongs in `DreamSource/FX/.vscode/`.

The project root is always first and always `"."` — the workspace file lives inside it, and a stable
folder identity keeps VSCode's per-folder state attached across a rewrite. A plugin root on another
drive has no relative form on Windows and gets an absolute path.

`dreamfxlang` is the language id the
[DreamFXLang extension](https://github.com/TypeDreamMoon/dreamfx-language-support) registers. The
association is written whether or not the extension is installed — without it VSCode falls back to
plain text, which is harmless — and the recommendation is a prompt VSCode shows once and never again
if dismissed.

| Toast | Condition |
| --- | --- |
| `DreamFX failed to create workspace: {Error}` | the file could not be written |
| `Opened DreamFX workspace in VSCode: {Path}` | VSCode launched |
| `Opened DreamFX workspace: {Path}` | the OS default editor launched |
| `Opened DreamFX workspace in Notepad: {Path}` | Notepad launched |
| `DreamFX could not open workspace: {Path}` | every launcher failed |

## How VSCode is discovered

Windows only, most specific first. The first candidate that exists on disk wins.

| # | Location |
| :-- | :-- |
| 1–2 | `%LOCALAPPDATA%\Programs\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 3–4 | `%LOCALAPPDATA%\Programs\Microsoft VS Code Insiders\{Code - Insiders.exe, bin\code-insiders.cmd}` |
| 5–6 | `%ProgramFiles%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 7–8 | `%ProgramFiles(x86)%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 9 | every `PATH` entry, checked for `code.cmd`, `code.exe`, `Code.exe`, `code-insiders.cmd`, `Code - Insiders.exe` |

Opening a **workspace** honours *Open Workspace In New Window*. Opening a **file** always passes
`--reuse-window -g <path>:<line>:<col>` — a new window per diagnostic jump would be unusable.

## The DreamSource layout

The DreamFXLang extension follows the same [layout](../getting-started/project-layout.md#the-dreamsource-layout)
as the plugin from version 0.8.0, which needs VS Code 1.147 or later: it reads the databases with the
`node:sqlite` that release ships.

| What | Where the extension looks |
| :-- | :-- |
| The project's sources | `SourceDirectory` in `Config/DefaultDreamSource.ini`, default `DreamSource/FX`; the legacy `DFX/` while that is the only one there |
| A plugin's sources | `<Plugin>/DreamSource/FX`, else `<Plugin>/DFX` |
| The module index | `<source directory>/.dreamfx/index.db`; the old `.dfx-index.json` while the database is missing or has no modules |
| The editor's last diagnostics | `Intermediate/DreamSource/fx.db`; the old `Saved/DreamFX/Bridge/diagnostics.json` from an older plugin |
| The [bridge](editor-integration.md#the-bridge) | `Intermediate/DreamSource/FX/Bridge/` when an editor has written `status.json` there, else `Saved/DreamFX/Bridge/` |

The command **DreamFX: Migrate to DreamSource** (`dreamfx.migrateToDreamSource`) moves `DFX/` the
way the editor's [Migrate button](../getting-started/project-layout.md#the-migration-notice) does. It
refuses while the editor has the project open, and leaves the old index files for the plugin to
import on its next `dfx index`.

## The module index

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 index
```

Writes `.dreamfx/index.db` in the project's source directory — `DreamSource/FX/.dreamfx/index.db`, or
`DFX/.dreamfx/index.db` while the project is still on `DFX/`: every module and dynamic input the
search paths expose, with its declared stacks, category, description and **input signature**. The
editor extension reads it for completion and hover — it cannot ask the engine, because every `dfx`
call boots it and nothing that happens while someone is typing may cost tens of seconds.

The two halves have very different costs:

| Half | Cost | Source |
| --- | --- | --- |
| Stacks and metadata | free | the asset's usage bitmask — the same list the engine's own stack UI filters by |
| Input signature | expensive | **probed**, because the asset-level schema misses inline edit conditions and static switches entirely |

Measured here: 571 modules, 2.9s without the probe, 8.1s with it. A completion list with no
enum-shaped inputs in it would be worse than none, so the cost is worth paying.

| Flag | |
| --- | --- |
| `-Out <path>` | write a JSON file there instead of the database, with its `.inflight` / `.quarantine` files beside it |
| `-NoInputs` | skip the probe. Fast, and enough to answer "what exists" |
| `-Retry` | clear the quarantine and try every module again — for after an engine upgrade |

The editor's [bridge](editor-integration.md#the-bridge) action `refreshIndex` writes the same database
from a running editor, without probing inputs: probing can crash the editor, so that half stays a
`dfx index` job.

> **Warning:**
>
> **Some module graphs cannot be walked.** `/Niagara/Modules/Masks/ConeMask` makes the engine's own
> traversal recurse without bound, and the stack overflow ends the process. So the walk is resumable:
> the module about to be probed is recorded first, and whatever is still recorded when the next run
> starts is quarantined. `dfx.ps1` re-runs until the walk completes. A quarantined module keeps its
> name, path and stacks; only its inputs are missing — **and it says so.**

The index records the engine path and the enabled plugin list, because those are what invalidate it:
a different engine is a different module set, and enabling a content plugin adds a whole family.
Nothing compares them yet — that needs a live editor to compare against — so rebuilding is a command
rather than something guessed at.

### What is in `index.db`

A SQLite database, written with `synchronous=FULL` so the record of the module being probed is on
disk before the probe that may end the process.

| Table | Holds |
| :-- | :-- |
| `meta(key, value)` | `schema` (`1`), `producer` (`DreamFX@<version>`), `revision`, `modules_revision`, `modules_updated_utc`, and the header the JSON file had: `version`, `generated_utc`, `project`, `inputs_probed`, `engine`, `plugins`, `search_paths` |
| `modules(ord, path, name, kind, json)` | one module (`kind = 'module'`) or dynamic input (`'dynamicInput'`) per row, in the JSON array's order; `json` is that array entry |
| `probe_journal` | at most one row: the module being probed right now |
| `quarantine(package, quarantined_utc)` | the modules whose probe ended a run |

A finished run replaces `modules` and the header in one transaction; the quarantine stays.

**The database is never dropped.** Each quarantine row cost a crashed engine. When its `schema` is
not the one this build writes, `dfx index` refuses to open it and asks you to update the DreamFX
plugin; the file is left as it is.

**The old files are imported.** A source directory that still holds `.dfx-index.json`,
`.dfx-index.json.inflight` or `.dfx-index.json.quarantine` — from before the DreamSource layout — has
them read into `index.db` by the next `dfx index` (or `refreshIndex`, or the editor's
[Migrate](../getting-started/project-layout.md#the-migration-notice)), then deleted. The quarantine and
whatever was in flight are always merged in; the modules only when the database has none yet.

## Committing it

Do not commit `.dreamfx/`: `index.db` carries this machine's engine path. Add `.dreamfx/` to
`.gitignore`. See [layout and naming](../getting-started/project-layout.md#what-belongs-in-version-control).
