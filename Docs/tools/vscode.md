---
title: VSCode and the module index
description: What the workspace file contains, how VSCode is discovered, the rules for jump links, and why .dfx-index.json probes input signatures.
---

## Open DreamFX Workspace

*Tools ▸ DreamFX ▸ Open DreamFX Workspace (VSCode)* rewrites `DFX/DreamFX.code-workspace`, then
launches it through VSCode → the OS default editor → Notepad.

```json
{
  "folders": [
    { "name": "DreamFX Source", "path": "." },
    { "name": "Plugin: DreamFX", "path": "../Plugins/DreamFX/DFX" }
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
> lost. Per-user configuration belongs in `DFX/.vscode/`.

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

## The module index

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 index
```

Writes `DFX/.dfx-index.json`: every module and dynamic input the search paths expose, with its
declared stacks, category, description and **input signature**. The editor extension reads that file
for completion and hover — it cannot ask the engine, because every `dfx` call boots it and nothing
that happens while someone is typing may cost tens of seconds.

The two halves have very different costs:

| Half | Cost | Source |
| --- | --- | --- |
| Stacks and metadata | free | the asset's usage bitmask — the same list the engine's own stack UI filters by |
| Input signature | expensive | **probed**, because the asset-level schema misses inline edit conditions and static switches entirely |

Measured here: 571 modules, 2.9s without the probe, 8.1s with it. A completion list with no
enum-shaped inputs in it would be worse than none, so the cost is worth paying.

| Flag | |
| --- | --- |
| `-Out <path>` | write somewhere else |
| `-NoInputs` | skip the probe. Fast, and enough to answer "what exists" |
| `-Retry` | clear the quarantine and try every module again — for after an engine upgrade |

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

## Committing it

Do not commit `DFX/.dfx-index.json`: it carries this machine's engine path. See
[layout and naming](../getting-started/project-layout.md).
