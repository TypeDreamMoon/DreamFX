# Build on save and up-to-date checks

This page describes the published DreamFX implementation at `9b0d990`. Saving source in the
editor triggers a normal rebuild. The published command interface does **not** provide an
`-Incremental` switch or an `Incremental Build On Save` setting.

The existing website address `plugin/tools/incremental-builds` is retained for compatibility;
it now points to this description of the behavior available in the published plugin.

## Published build scope [#incremental-builds]

The section links on this page also preserve older addresses. They describe the published full
build and up-to-date checks, without claiming a partial-update implementation or its performance.

## Build sequence [#how-it-works]

The generator reads the source, resolves its modules and dependencies, writes the system in
memory, completes Niagara compilation, stamps provenance, and saves if validation permits it.
See the [build pipeline](../generation/pipeline.md) for the full sequence.

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

## Available controls [#switching-it-on-and-off]

There is no incremental-generation mode to enable or disable in the published plugin. The editor
offers **Bulk Rebuild Threshold** for large watcher batches. An ordinary CLI build can skip an
up-to-date source; explicit `-Force` requests rebuilding and can permit safety-gate loss with
warnings, as explained below.

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

## Source changes and build cost [#what-each-kind-of-edit-costs]

A value edit and a structural edit both use the published generation pipeline. Changes to a
referenced `.dfm`, parent system source or reusable emitter can require rebuilding consumers.
This page supplies no per-edit timing table: costs depend on the system, dependencies, engine
and compilation caches, and must be measured on the version actually in use.

### Niagara compilation [#compile]

A system build requests Niagara compilation and waits for current executable data before
stamping or saving. A derived-data cache hit may supply that data without shader-worker log
lines. Neither a stale successful status nor a missing compile log alone establishes success;
see [headless compilation](headless-compilation.md).

### Module schema probes [#module-probes-are-made-once]

Use `dfx.ps1 schema <Module>` to inspect the inputs the installed module actually exposes.
The published documentation does not promise a shared once-per-process module-probe cache or
timings based on such a cache. See [VSCode and the module index](vscode.md) for available indexing.

## Full rebuild behavior [#when-it-falls-back-to-a-full-rebuild]

The published path already performs the normal rebuild for changed source. There is no
incremental-plan fallback decision or stored plan digest to interpret. A build that reports
`up to date` was skipped through provenance; explicit rebuild requests use normal generation.

## Interpreting build output [#reading-the-log]

`built`, `up to date` and `failed` distinguish completed builds, provenance skips and failures.
The first diagnostic explains a failure; a `DFX8018` refusal concerns state loss before saving.
These results do not describe incremental edits or establish runtime visual equivalence.

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

### What the checks establish [#the-equivalence-guarantee]

Compilation, provenance verification and round-trip comparison answer different questions.
L1 compares exported text, L2 checks compilation, and `asset-diff` compares reflected facts;
runtime or visual equivalence requires its own measurement. No incremental/full-build equivalence
claim is made for a mode absent from the published interface. See
[round trip and equivalence](../generation/roundtrip.md).

## Diagnosing a rebuild [#troubleshooting]

- A large pending batch: use the editor's offered rebuild action or **Tools → DreamFX → Rebuild DFX**.
- Unexpected `up to date`: inspect source provenance and the source/root-relative location.
- A failed compile: start with its first positioned diagnostic and the module's actual schema.
- `DFX8018`: inspect the reported lost or changed facts and make the intended source values explicit;
  use [pull](pull.md) where its declaration and addressing rules apply.

## Related references [#see-also]

- [Editor integration](editor-integration.md).
- [Headless compilation](headless-compilation.md).
- [Provenance and verification](../generation/provenance.md).
- [Diagnostic catalog](../diagnostics/README.md).
