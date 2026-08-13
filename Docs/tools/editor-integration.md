# Editor integration

> [DreamFX](../../README.md) » **Editor integration**

Every place DreamFX attaches itself to the Unreal editor UI: the Tools menu, the Level Editor
toolbar, the Content Browser asset context menus, and the Niagara system editor toolbar.

| | |
| :-- | :-- |
| Registered by | `DreamFXEditor` at module startup, through `UToolMenus::RegisterStartupCallback` |
| ToolMenu owner | `DreamFXEditor` |
| Suppressed by | `-NoDreamFXEditor`, and by every commandlet run |
| Settings | *Project Settings ▸ Plugins ▸ DreamFX* |

Registration is idempotent — a second pass adds nothing — and is skipped while the editor is shutting
down. **Every entry calls the same code the commandlet calls.** A menu is an entry point, never a
second implementation: if *Rebuild DFX* works, `dfx build -All` works, and vice versa.

## Tools menu

*Tools ▸ DreamFX*, extending `LevelEditor.MainMenu.Tools`, section `DreamFX`.

| Entry name | Label | Icon | Effect |
| :-- | :-- | :-- | :-- |
| `DreamFX.RebuildAll` | **Rebuild DFX** | `Icons.Refresh` | Queues every `.dfs` / `.dfe` / `.dfm` into the watcher's build queue, exactly as if each had been saved. One summary toast when the queue drains |
| `DreamFX.VerifyAll` | **Verify DFX** | `Icons.Adjust` | Verifies every source against its generated asset. Writes nothing. One toast: clean, or a drift count pointing at the Output Log |
| `DreamFX.OpenWorkspace` | **Open DreamFX Workspace (VSCode)** | `Icons.OpenInExternalEditor` | Rewrites `DFX/DreamFX.code-workspace` from the current source roots, then launches it |

## Level Editor toolbar

Extending `LevelEditor.LevelEditorToolBar.AssetsToolBar`, section `DreamFX`.

| Entry name | Label | Icon | Effect |
| :-- | :-- | :-- | :-- |
| `DreamFX.RebuildAllToolbar` | **DFX** | `Icons.Refresh` | Identical to *Rebuild DFX* |
| `DreamFX.OpenWorkspaceToolbar` | **Open DreamFX Workspace (VSCode)** | `Icons.OpenInExternalEditor` | Identical to *Open DreamFX Workspace* |

## Content Browser context menus

Added into the stock `GetAssetActions` section of the per-class asset context menu.

> [!WARNING]
> **Every DreamFX context-menu entry requires exactly one selected asset.** With two or more selected
> the section is empty and nothing says why. Right-click a single asset.

| Asset class | Dynamic entry | What appears |
| :-- | :-- | :-- |
| `UNiagaraSystem` | `DreamFX.SystemAssetActions` | submenu **DreamFX** — [System submenu](#system-submenu) |
| `UNiagaraEmitter` | `DreamFX.EmitterAssetActions` | submenu **DreamFX** — [Emitter submenu](#emitter-submenu) |

### System submenu

`DreamFX.SystemActions` — label **DreamFX**, icon `Icons.Settings`. Its contents depend on whether
the asset carries a DreamFX [provenance stamp](../../README.md), because a generated asset and an
imported one need opposite things.

**Without provenance** (an asset DreamFX did not make), section `DreamFX.DecompileActions`,
titled *Decompiler*:

| Entry | Label | Icon | Effect |
| :-- | :-- | :-- | :-- |
| `DreamFX.ExportDfs` | **Export .dfs** | `Icons.Save` | Decompiles to `<DecompiledOutputDirectory>/<package path>.dfs` and opens it. The file rebuilds a mirror under `Decompiled/`; the asset is never modified |
| `DreamFX.Adopt` | **Adopt (take over as a DFX source)** | `Icons.Import` | [below](#adopt) |

**With provenance**, section `DreamFX.SourceActions`, titled *Source*:

| Entry | Label | Icon | Effect |
| :-- | :-- | :-- | :-- |
| `DreamFX.OpenSource` | **Open Source** | `Icons.OpenInExternalEditor` | Opens the `.dfs` named in the stamp |
| `DreamFX.RebuildFromSource` | **Rebuild from Source** | `Icons.Refresh` | Queues that one file |
| `DreamFX.VerifyAsset` | **Verify** | `Icons.Adjust` | Checks this asset against its source |

### Emitter submenu

`DreamFX.EmitterActions` — one entry, because a standalone emitter has no round trip: a `.dfe`
generates no asset of its own, so there is nothing to stamp, rebuild or verify.

| Entry | Label | Icon | Effect |
| :-- | :-- | :-- | :-- |
| `DreamFX.ExportDfe` | **Export .dfe** | `Icons.Save` | Copies the emitter into a throwaway `/Temp` host system, reads it there, and writes `<DecompiledOutputDirectory>/<package path>.dfe`, named into the `Decompiled/` namespace like *Export .dfs* |

The host system is what makes this honest rather than a guess: every reader in the Niagara external
edit API addresses through an owning system, so what comes back is exactly what the emitter
contributes when a system uses it.

## Niagara system editor toolbar

`DreamFX.SystemEditorToolbarActions`, a dynamic entry in section `DreamFX` of
`AssetEditor.Niagara.ToolBar`. Adds a combo button labelled **DreamFX** whose content is the same
two-state [System submenu](#system-submenu).

> [!NOTE]
> `AssetEditor.Niagara.ToolBar` is shared. `FNiagaraSystemToolkit`, `FNiagaraSimCacheToolkit`,
> `FNiagaraParameterDefinitionsToolkit`, `FNiagaraParameterCollectionToolkit` and
> `FNiagaraStatelessEmitterTemplateToolkit` all return the toolkit name `"Niagara"`. So the entry
> first looks for a `UNiagaraSystem` among the objects being edited and adds nothing when there is
> none — opening a Sim Cache shows no DreamFX button.

## Command semantics

### Rebuild DFX

Invalidates the source-root cache (so a newly enabled plugin is picked up), then stamps every source
file into the watcher's pending queue. The debounce ticker builds them modules-first, then emitters,
then systems — the same order the commandlet uses, and load-bearing when a `.dfm` and the `.dfs` that
calls it are queued together.

Decompiled exports are included, like any other source. See [Export vs Adopt](#export-vs-adopt).

| Toast | Condition |
| :-- | :-- |
| `DreamFX: {Built} built, {Skipped} up to date.` | nothing failed |
| `DreamFX build failed: {FirstError}` + an **Open in VSCode** link | one or more failed |
| `DreamFX found no .dfs/.dfe/.dfm sources to rebuild.` | no sources at all |

The **Open in VSCode** link jumps to the first error's file, line and column. The diagnostic has
carried a position all along; before this it only reached the log.

### The bulk batch

Re-exporting a content pack, switching branch, or running a scripted rewrite changes many sources at
once, and rebuilding each one queues a Niagara compile per system. On this project that killed the
editor: 24 systems rebuilding together put 237 jobs into the compile queue.

So a batch holding more than **Bulk Rebuild Threshold** files (Project Settings → Plugins → DreamFX,
default 8) is offered rather than built:

> DreamFX: *N* source files changed at once. Rebuilding them all now would queue hundreds of Niagara
> compiles. — **Rebuild them now**

The files are kept, not dropped: dropping them would leave the assets stale with nothing to notice
it, which is the same failure the gate exists to prevent, only quieter. If the toast expires, *Tools
→ DreamFX → Rebuild DFX* does the same thing.

An ordinary save is never gated — one file, or any batch at or under the threshold, rebuilds
immediately, because that path is the iteration loop. An explicit *Rebuild DFX* is never gated
either: it was asked for.

> [!NOTE]
> This gate was first written against a different theory — that the danger was the watcher replaying
> everything changed while the editor was shut down — and measurement contradicted it.
> `RegisterDirectoryChangedCallback_Handle` starts watching at registration and reports no backlog:
> twelve files changed with the editor closed produced *nothing at all* on reopen. The batch that
> actually hurts arrives while the editor is open. Measured with twelve files before the fix: 54
> Niagara system compiles and eight compile-pool saturations. After: the toast, and nothing built
> until asked.

### Verify DFX

Runs the generator in verify mode over every source. Nothing is written — not the asset, not the
provenance stamp. `.dfe` files are skipped: they generate no asset, so there is nothing to compare.

| Toast | Condition |
| :-- | :-- |
| `DreamFX: {N} source(s) verified, all assets in step.` | clean |
| `DreamFX: {Drifted} of {N} source(s) out of step ({Failed} failed). See the Output Log.` | drift or failure |

### Export vs Adopt

Both decompile. They differ in what happens next, and that difference is the whole point.

| | **Export .dfs** | **Adopt** |
| :-- | :-- | :-- |
| Writes to | `DecompiledOutputDirectory` (default `DFX/Decompiled`) | the real source root — `<Project>/DFX/…` or `<Plugin>/DFX/…` |
| `Name=` names | `Decompiled/<original directory>/<asset>` — a **mirror** | the original asset |
| Picked up by the build | yes — saving one rebuilds its mirror | yes |
| Modifies the asset | **never** | yes: rebuilds it from the new source and stamps provenance |
| Unrepresentable features | warns, and lists them in the file | **refuses** |
| Meaning | "let me read and play with this as text" | "from now on this text is the only truth" |

What keeps the two apart is structural, not procedural: an export cannot name the asset it came from.
`/Game/FX/NS_X` exports to `DFX/Decompiled/Game/FX/NS_X.dfs`, whose `Name=` is `Decompiled/FX/NS_X`,
so a build writes `/Game/Decompiled/FX/NS_X` and the original is untouched however the file is edited.
The whole decompiled tree is therefore ordinary source — watched, built on save, linted and CI'd.

Exports written before this arrangement still name the original. They are refused with
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) rather than obeyed; re-exporting replaces them.

Exporting an asset that is *already* a mirror is refused too — it would leave two sources claiming one
asset — and the toast links to the source the mirror was built from.

### Adopt

1. Decompile. **Refuse** if anything is unrepresentable, listing what.
2. Work out the source path from the asset's mount point: `/Game/FX/NS_X` → `<Project>/DFX/FX/NS_X.dfs`,
   `/MoonToon/FX/NS_X` → `<MoonToon>/DFX/FX/NS_X.dfs`.
3. **Refuse** if another `.dfs` already declares the same target asset.
4. Confirm, listing the file to write and the asset to rebuild.
5. Write the file, rebuild the asset from it, stamp provenance.
6. Re-decompile the rebuilt asset and compare byte for byte.

| Diagnostic | Cause |
| :-- | :-- |
| `DFX8010` | the asset has features DreamFXLang cannot express |
| `DFX8011` | another source already generates this asset |
| `DFX8012` | adopted, but the rebuilt asset does not re-export to the same text |

Step 1 is what makes step 6 meaningful: with no known gaps, a mismatch is a real defect rather than
an expected loss. `DFX8012` leaves the source file in place — the asset was rebuilt from it, so the
text is authoritative either way — and logs the first differing line.

### Open DreamFX Workspace (VSCode)

Rewrites `DFX/DreamFX.code-workspace`, then launches it through VSCode → the OS default editor →
Notepad. The file is **fully rewritten every time, never merged**: hand-added `launch` or `tasks`
blocks are lost. Per-user configuration belongs in `DFX/.vscode/`.

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

The project root is always first and always `"."` — the workspace file lives inside it, and a stable
folder identity keeps VSCode's per-folder state attached across a rewrite. A plugin root on another
drive has no relative form on Windows and gets an absolute path.

`dreamfxlang` is the language id the [DreamFXLang
extension](https://github.com/TypeDreamMoon/dreamfx-language-support) registers. The association is
written whether or not the extension is installed — without it VSCode falls back to plain text, which
is harmless — and the recommendation is a prompt VSCode shows once and never again if dismissed.

| Toast | Condition |
| :-- | :-- |
| `DreamFX failed to create workspace: {Error}` | the file could not be written |
| `Opened DreamFX workspace in VSCode: {Path}` | VSCode launched |
| `Opened DreamFX workspace: {Path}` | the OS default editor launched |
| `Opened DreamFX workspace in Notepad: {Path}` | Notepad launched |
| `DreamFX could not open workspace: {Path}` | every launcher failed |

## VSCode discovery

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

## Settings

*Project Settings ▸ Plugins ▸ DreamFX*, stored in `Config/DefaultEditor.ini`.

| Setting | Default | Effect |
| :-- | :-- | :-- |
| **Open Workspace In New Window** | `true` | Only the workspace launcher reads it |
| **Decompiled Output Directory** | `DFX/Decompiled` | Where *Export .dfs* / *Export .dfe* write, relative to the project directory. Ordinary source: discovered, watched and built. Files here must name an asset in the `Decompiled/` namespace (DFX8013) |

## Scripting the same commands

`UDreamFXEditorLibrary` exposes every command to Python and Blueprint, forwarding to the same bodies
the menus call. A Slate entry cannot be invoked from a script, and a command that only a human click
can reach is a command that never gets regression tested.

```python
import unreal
lib = unreal.DreamFXEditorLibrary
lib.rebuild_all_sources()
lib.verify_all_sources()
print(lib.write_workspace_file())          # writes, does not launch
lib.export_system(unreal.load_asset("/Game/FX/NS_Spark"))
lib.adopt_system(unreal.load_asset("/Game/FX/NS_Spark"), True)   # True = skip the confirmation
```

`bSkipConfirmation` suppresses the modal dialog only. Both refusals still apply, reported as a toast
and a log line instead of a dialog — a modal with no one to click it would block the calling thread.

## Disabling the integration

```powershell
UnrealEditor.exe "<Project>.uproject" -NoDreamFXEditor
```

Parsed as a bare command-line parameter, so `-NoDreamFXEditor` is the only accepted spelling.

| Disabled | Still active |
| :-- | :-- |
| Every menu, toolbar and context-menu entry on this page | The `-run=DreamFX` commandlet, which never uses any of it |
| The source watcher and save-to-rebuild | The parser, generator, decompiler and settings object |
| The generated-asset guard | Assets already generated and saved on disk |

A commandlet run reaches the same state by a different route: `IsRunningCommandlet()` returns true
and the module registers nothing.

## See also

- [Getting started](../getting-started.md) — the editor-side workflow in order
- [Language reference](../language/README.md) — what the exported text means
- [Diagnostics](../diagnostics/README.md) — every `DFXnnnn`, including `DFX8010`–`DFX8016`, the
  gap codes that say what a decompile could not carry
- `dfx.ps1 coverage` — the same question asked of a whole content tree, bucketed by feature
