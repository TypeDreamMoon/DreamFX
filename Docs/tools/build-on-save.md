# Build on save and up-to-date checks

This page describes the published DreamFX implementation at `9b0d990`. Saving source in the
editor triggers a normal rebuild. The published command interface does **not** provide an
`-Incremental` switch or an `Incremental Build On Save` setting.

The existing website address `plugin/tools/incremental-builds` is retained for compatibility;
it now points to this description of the behavior available in the published plugin.

## Saving source in the editor

The file watcher queues changed `.dfs`, `.dfe` and `.dfm` sources. Its debounced batch builds
modules before emitters and systems, so dependencies are available when their consumers build.
It also follows system `Parent` and emitter `from` references, including references outside
the discovered DFX roots.

A save sets the normal force-rebuild option. The generator rebuilds the affected source through
the existing generation pipeline; it does not apply a stored-plan difference to just the edited
property. **Rebuild from Source** uses the same watcher queue and drains it immediately.

If more than **Bulk Rebuild Threshold** files change together (default **8**), the editor offers
the batch for confirmation instead of starting all Niagara compiles at once. The files remain
queued. Ordinary saves at or below the threshold, and explicit **Rebuild DFX** requests, proceed
without that batch prompt. See [editor integration](editor-integration.md#the-bulk-batch).

## Unchanged sources can be skipped

An ordinary headless build can report `up to date` when the generated asset's provenance matches
its source. This skips a build; it is different from incrementally modifying an existing asset.

```powershell
# Build the source if its generated asset is out of date.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs

# Request a rebuild even when provenance matches.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs -Force

# Check source/asset provenance without repairing the asset.
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 verify DFX/Effects/NS_Hello.dfs
```

Close the editor before running package-writing headless commands. Source location matters to
provenance: moving a source within its DFX root causes an ordinary rebuild to update the stamp;
checking out the same root-relative tree at a different absolute path does not by itself make
the source stale. See [provenance and verification](../generation/provenance.md).

## Protecting editor-authored values

The build safety gate can refuse to save a rebuild that would lose undeclared asset state
(`DFX8018`). The explicit CLI `-Force` can permit that loss with warnings. Its loss override is
separate from the ordinary force-rebuild flag used by watcher, bridge and Adopt paths: those
editor paths do not bypass the safety gate merely because they request rebuilding.

When values were tuned in the editor, use [pull](pull.md) to update eligible source-declared
literals before rebuilding. Pull's `-Apply`, `-Structure` and baseline behavior are available
in the published implementation; they do not imply incremental asset generation.

## Verification

Use the published [command-line interface](headless-compilation.md) and [CI sequence](ci.md):
lint, build, verify and corpus. This documentation change does not claim new timings, performance
improvements or runtime validation; those require implementation and measurement on the relevant
plugin and engine version.
