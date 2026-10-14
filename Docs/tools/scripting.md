---
title: Python and Blueprint
description: UDreamFXEditorLibrary exposes every editor command to scripts — and why that is not an afterthought.
---

`UDreamFXEditorLibrary` exposes every editor command to Python and Blueprint, forwarding to the same
bodies the menus call.

```python
import unreal

lib = unreal.DreamFXEditorLibrary

lib.rebuild_all_sources()                   # = Tools ▸ DreamFX ▸ Rebuild DFX
lib.verify_all_sources()                    # = Verify DFX
print(lib.write_workspace_file())           # writes the workspace, does not launch it

lib.export_system(unreal.load_asset("/Game/FX/NS_Spark"))
lib.adopt_system(unreal.load_asset("/Game/FX/NS_Spark"), True)   # True = skip the confirmation
```

> **Note:**
>
> **A command only a human click can reach is a command that never gets regression tested.** A Slate
> entry cannot be invoked from a script, so every command has a callable entry point — not a
> convenience bolted on afterwards, but the precondition for the menu layer being verifiable at all.

## `bSkipConfirmation` suppresses the dialog only

It removes the modal confirmation and nothing else. Both refusals still apply —
[DFX8010](../diagnostics/DFX8xxx.md#dfx8010) (something is unrepresentable) and
[DFX8011](../diagnostics/DFX8xxx.md#dfx8011) (another source already declares this asset) — reported
as a toast and a log line instead of a dialog.

The reason is direct: a modal with no one to click it would block the calling thread.

## What it is good for

**Bulk migration.** Sweep a set of assets, adopt the ones that decompile cleanly, and list the rest
for a human:

```python
import unreal

lib = unreal.DreamFXEditorLibrary
registry = unreal.AssetRegistryHelpers.get_asset_registry()

for data in registry.get_assets_by_path("/Game/VFX", recursive=True):
    asset = data.get_asset()
    if isinstance(asset, unreal.NiagaraSystem):
        lib.export_system(asset)          # export everything; the originals are never touched
```

**A gate inside another toolchain.** Feed the result of `verify_all_sources()` into your own release
check.

**A scheduled rebuild in-editor.** Run `rebuild_all_sources()` once after a branch switch rather than
letting the watcher meet a hundred changes at once (see the bulk gate in
[the daily loop](../getting-started/workflow.md)).

## The command line is the headless path

The Python surface runs **inside the editor process**. For headless work use
[`dfx.ps1`](headless-compilation.md) — it runs the commandlet, with the same pipeline and the same diagnostic
codes, and needs no UI at all.

| Goal | Use |
| --- | --- |
| Automating a sequence inside the editor | Python / Blueprint |
| CI, batch jobs, agents | `dfx.ps1` / `ci.ps1` |
