---
title: Install and sanity check
description: Enable the DreamFX plugin, confirm the driver can reach the engine, and the three hard requirements of the environment.
---

DreamFX is an Unreal plugin and installs like any other: drop `DreamFX/` into the project's
`Plugins/` directory (or the engine's `Engine/Plugins/`) and restart the editor. It depends on
`Niagara`, and the `.uplugin` declares that dependency.

| | |
| --- | --- |
| Modules | `DreamFX` (Runtime), `DreamFXEditor` (Editor, `LoadingPhase = Default`) |
| Enabled by default | yes (`EnabledByDefault: true`) |
| Plugin dependency | `Niagara` |
| Engine | Unreal Engine `5.8` |
| Platform | Win64 (the driver is PowerShell; `dfx.ps1` needs `pwsh`) |

Both modules are **editor-side**: generation, decompilation and diagnostics all happen inside an
editor or a commandlet process. A packaged game contains no DreamFX at all — only the Niagara assets
it generated, which are ordinary assets that anything can load.

## Sanity check: can the driver reach the engine?

Run this first. It is much faster than opening the editor, and it says more:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list
```

It prints every Niagara module on the default search paths. **Anything printed means three things at
once**: the engine resolved, the commandlet booted, and the module index is readable.

If it errors before printing anything, the engine did not resolve. `dfx.ps1` looks for it in this
order:

1. the `-Engine <root>` argument;
2. the `UE_ENGINE_ROOT` environment variable;
3. the `.uproject`'s `EngineAssociation` — a source build is registered under
   `HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds`, an installed build under
   `HKLM:\SOFTWARE\EpicGames\Unreal Engine\<version>`.

The project itself is found by walking up from the target file (then from the working directory), so
calling the driver from anywhere under `DFX/` still finds the right one. Pass `-Project` when it
cannot.

## Confirming it editor-side

Once the editor is up, *Tools ▸ DreamFX* holds three entries: **Rebuild DFX**, **Verify DFX** and
**Open DreamFX Workspace (VSCode)**. The Level Editor toolbar carries a **DFX** button as well.

If none of them are there, either the module did not load or the process was launched with
`-NoDreamFXEditor`. That switch is deliberate: it turns off the whole interactive surface (menus,
file watcher, generated-asset guard) but **not** the commandlet or the parser. Use it when DreamFX
itself is the thing being debugged.

```powershell
UnrealEditor.exe "<Project>.uproject" -NoDreamFXEditor
```

> **Warning:**
>
> `-NoDreamFXEditor` is the only accepted spelling — it is parsed as a bare command-line parameter.

## Environment requirements

Three of them, none optional.

### The engine API it builds on is experimental

DreamFX is built on the engine's Niagara external edit API (`UNiagaraExternalEditUtilities`), which
the engine marks EXPERIMENTAL. What happens when it drifts? **Startup self-checks surface the drift
rather than absorbing it** — that is deliberate. A generator that quietly degrades is far more
dangerous than one that stops and says so.

### Host projects must enable the same content plugins

If the authoring project enables a **content** plugin such as `NiagaraFluids`, every project
consuming those sources has to enable it too. With the plugin unmounted the module probe has no
baseline, and sources referencing its modules surface as gap headers or compile errors
([DFX3001](../diagnostics/DFX3xxx.md#dfx3001)).

A whole family of unresolvable names sharing one prefix — `/NiagaraFluids/*` — is a mount point that
is missing, not a source that is wrong.

### Package-writing commands need the editor closed

`build`, `corpus`, `mirror-diff` and `decompile-all` all write `.uasset` files. An editor open on the
same project writes them too; whichever saves second wins, and neither says so. `dfx.ps1` warns when
it sees an editor process, but it cannot tell which project that editor has open, so the warning is
**advice, not a gate**.

## Where source lives

Sources live under `DFX/` — the project's own, or any plugin's:

```
DevTest/DFX/                        Root=""  or  Root="Game"     -> /Game
DevTest/Plugins/MoonToon/DFX/       Root="Plugin.MoonToon"       -> /MoonToon
```

Every `DFX/` directory is a source root: discovered, watched, and swept up by `build -All`. The full
story is in [layout and naming](project-layout.md).

## Next

  - **[Your first effect](first-effect.md)** — Nothing to a running `NS_Hello` in about fifteen minutes.
  - **[The daily loop](workflow.md)** — Save-to-rebuild, the bulk gate, and the four-step CI before you commit.
