---
title: The build pipeline
description: What a build actually does — discovery, ordering, schema probing, writing, the Niagara compile, saving, stamping, and why an unchanged source is skipped.
---

A build turns source into assets through six stages — and the first digit of a diagnostic code is the
stage that raised it:

| Stage | What it does | Codes |
| --- | --- | --- |
| 1 | Driver and file I/O | `DFX1xxx` |
| 2 | Lexer and syntax | `DFX2xxx` |
| 3 | Declarations and document structure | `DFX3xxx` |
| 4 | Values, types and expressions | `DFX4xxx` |
| 5 | Generation and asset writing | `DFX5xxx` |
| 6 | Niagara compilation | `DFX6xxx` |

Two more families sit outside that line: `DFX7xxx` is
[provenance, drift and lint](provenance.md), and `DFX8xxx` is
[the decompiler](roundtrip.md).

## Order: modules → emitters → systems

`build -All` (and *Rebuild DFX* in the editor) builds in that order for a concrete reason: when a
`.dfm` and the `.dfs` that calls it are queued together, the module has to exist first or the system
cannot resolve it.

The editor-side watcher uses the same order — it stamps changed files into a pending queue, and the
debounce ticker drains it.

## One file's journey


### Parse

Lexer plus parser produce a document. Position information (file, line, column) is attached here and
carried to the last diagnostic, which is why a failed build can toast an **Open in VSCode** link that
lands on the offending column.

### Resolve module names and schemas

Short module names resolve on `ModulePaths` (L4). Once a name resolves, **the input signature is
probed, not assumed**: the module is added to a transient system and the inputs it really exposes in
that stack are read back.

That detour is necessary — the asset-level schema misses inline edit conditions and inputs revealed
by a static switch entirely. `dfx schema <Module> -Stack <Stack>` reproduces this exact probe.

### Write

The system, emitters, stacks, modules and inputs are written through the Niagara external edit API.
Write order carries meaning: a static switch has to be written before the inputs it reveals exist
(see [`.dfs`](../language/dfs.md)).

### Compile

The system compiles once it is written. A compile that is not clean is a `DFX6xxx` — including the
engine's own stack issues (unmet dependencies, unbound parameters), which DreamFX restates as
positioned diagnostics.

### Save and stamp

`SavePackage` writes the package and stamps provenance on the asset: the source hash, the generator
version, and module version GUIDs. A failed save is
[DFX5030](../diagnostics/DFX5xxx.md#dfx5030), a build error — **not** a fatal, so "the file is in use
by another program" no longer takes the editor down with it.


## Unchanged sources are skipped

Building an untouched file a second time reports `0 built, 1 up to date`: the hash stamped on the
asset matches the source's, so there is nothing to do.

| To | Use |
| --- | --- |
| Rebuild regardless of the hash | `-Force` |
| Build without writing packages | `-NoSave` |
| Run static checks only, touching no asset | `lint` |
| Check assets against sources, writing nothing | `verify` |

## Building in the editor vs on the command line

One pipeline, two entry points. There is exactly one difference, and it is hard:

> **Warning:**
>
> **Do not run package-writing commands with the editor open.** Two processes saving the same
> packages race silently, and whichever saves second wins. `build`, `corpus`, `mirror-diff` and
> `decompile-all` all write packages.

One more that is equally hard: **despawn a system's instances before rebuilding it** — see
[the daily loop](../getting-started/workflow.md).

## Performance: why a full-tree build is not linear

In a whole-tree build, most of the time is neither parsing nor writing — it is the **Niagara
compile**. Measured on a single asset, 84% of the time went there, with roughly four system compiles
triggered per asset.

Several knobs exist, and the defaults are already the fast side. They are there mainly for A/B and
for escape:

| Switch | Effect |
| --- | --- |
| `-Window=<n>` | pipeline depth: how many systems may sit between compile request and finalize. `1` restores the fully serial build |
| `-NoWriteScope` | rebuild the edit context for every write (the old behaviour, much slower) |
| `-RebuildOnStructural` | drop the edit context after every structural write (the old behaviour) |
| `-RebuildOnSwitch` | drop the edit context after every static-switch write (the old behaviour) |
| `-RebuildPerAdd` | pay the engine's per-add stack refresh instead of one batch refresh per stack |

> **Note:**
>
> These are not day-to-day switches. Their value is that **the fast path and the slow path can both
> be run on one machine from one binary** — so "did that optimisation actually help" is measured
> rather than read out of the code.

## Related

  - **[Provenance and verify](provenance.md)** — What the stamp holds, the three drifts `verify` reports, and the four lint rules.
  - **[Round trip and equivalence](roundtrip.md)** — Decompilation, gaps, and the four verification layers.
  - **[The command line](../tools/headless-compilation.md)** — Every `dfx.ps1` subcommand and switch.
