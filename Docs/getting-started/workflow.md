---
title: The daily loop
description: Save-to-rebuild, live preview, the bulk-rebuild gate, the two hard rules, and the four-step CI before you commit.
---

## Save to rebuild

The editor watches every `DFX/` root. Save a source, it rebuilds, and the generated asset refreshes.
Leave that asset open in the Niagara editor and it is your live preview, at no setup cost.

A failed build toasts with an **Open in VSCode** link that jumps to the offending line and column.
The "this asset is generated" warning on a generated asset carries one to its source file too.

> **Warning:**
>
> ### Despawn instances before rebuilding
>
> This one is not optional: **despawn instances of a system before rebuilding it.** During a hot swap
> a live `NiagaraActor` can see a half-built renderer layout and assert on a render worker, which takes
> the editor with it.
>
> It is an engine race, not something DreamFX can guard.

## The bulk batch is gated

Re-exporting a content pack, switching branch, or running a scripted rewrite changes many sources at
once, and each rebuild queues a Niagara compile per system. Measured on this project: 24 systems
rebuilding together put 237 jobs into the compile queue and killed the editor.

So a batch holding more than **Bulk Rebuild Threshold** files (*Project Settings ▸ Plugins ▸
DreamFX*, default 8) is **offered** rather than built:

> DreamFX: *N* source files changed at once. Rebuilding them all now would queue hundreds of Niagara
> compiles. — **Rebuild them now**

The files are kept, not dropped: dropping them would leave the assets stale with nothing to notice
it, which is the same failure the gate exists to prevent, only quieter. If the toast expires, *Tools
▸ DreamFX ▸ Rebuild DFX* does the same thing.

An ordinary save is never gated — one file, or any batch at or under the threshold, rebuilds
immediately, because that path is the iteration loop. An explicit *Rebuild DFX* is never gated
either: it was asked for.

> **Note:**
>
> The gate was first written against a different theory — that the danger was the watcher replaying
> everything changed while the editor was shut down — and measurement contradicted it.
> `RegisterDirectoryChangedCallback_Handle` starts watching at registration and reports no backlog:
> twelve files changed with the editor closed produced *nothing at all* on reopen. The batch that
> actually hurts arrives while the editor is open. Measured with twelve files before the fix: 54
> Niagara system compiles and eight compile-pool saturations. After: the toast, and nothing built
> until asked.

## Before you commit

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

Four steps, each catching something the others cannot:

| Step | Catches |
| --- | --- |
| `lint` | a GPU emitter with no bounds, an uncapped spawn rate, randomness with no seed |
| `build` | anything that stops the text becoming an asset |
| `verify` | **a source edited and committed without a rebuild** — build alone passes, because build fixes it |
| `corpus` | a *behaviour* changing rather than breaking: diagnostics moving, topologies shifting, decompile losing idempotence |

`verify` is the one worth understanding. It compares each asset's provenance stamp against its
source; a mismatch means the committed asset and the committed text disagree, which is invisible any
other way.

> **Warning:**
>
> **Close the editor first.** `build` and `corpus` write `.uasset` files, and an editor open on the
> same project writes them too — whichever saves second wins, and nothing says so. The run warns when
> it sees an editor process, but it cannot tell which project that editor has open, so the warning is
> advice and not a gate.

## The shape of a day


### Write
Edit the `.dfs` in VSCode (*Tools ▸ DreamFX ▸ Open DreamFX Workspace* sets the workspace up for you).
Completion and hover come from `DFX/.dfx-index.json`; re-run `dfx.ps1 index` after an engine upgrade
or when a new content plugin is enabled.

### Save
Saving rebuilds. The Niagara preview refreshes. On a failure, the toast's **Open in VSCode** link
lands on the line.

### Check
When an input name is uncertain, run `dfx.ps1 schema <Module> -Stack <Stack>` — it runs
the same probe the build runs, so inputs revealed by a static switch show up too.

### Commit
Close the editor, run `ci.ps1`, commit when it is green.


## Asking the coverage question of your own content

This is not a question to answer with somebody else's number. Measure it:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff       # L1 text + L2 compile
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff        # facts, independent of the exporter
```

See [round trip and equivalence](../generation/roundtrip.md).
