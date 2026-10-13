---
title: ci.ps1 and gates
description: What each of the four CI steps catches, which switches to use where, the docs drift check, and a minimal pipeline.
---

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

One command, four steps, ordered so the fastest failure comes first:


### `lint -All`
Source only, no asset access, so it fails fastest. Catches: a GPU emitter with no bounds, an uncapped
spawn rate, randomness with no seed, a renderer with no material.

### `build -All -Force`
Every `.dfs` and `.dfm` generates, and its Niagara compile is clean. `-Force` is the point — a gate
must not skip anything because "the hash did not move".

### `verify -All`
Every generated asset's stamp matches its source. **This is the step that catches the case nobody
notices**: someone edited a `.dfs`, did not rebuild, and committed both. Build alone would pass,
because build fixes it.

### `corpus`
The `Tests/Corpus` suites: diagnostics by code and position, golden topologies, decompile
idempotence. **It catches a behaviour changing rather than breaking.**


Exit code 0 means all four passed; anything else is the first failing step's code.

> **Note:**
>
> Why step 4 is worth its cost: almost everything DreamFX knows about Niagara was established by
> **experiment**, not guaranteed by a type. The corpus is what makes a quiet change in any of it fail.

## Switches

| Switch | When |
| --- | --- |
| `-SkipBuild` | a check-only gate that must not write to the working tree. `verify` then reports any source that has not been built |
| `-SkipCorpus` | a quick local check. **Never** for the gate — the corpus boots the editor and costs more than the other three steps put together, so skipping it means skipping behavioural regression |
| `-CleanNew` | delete assets the build newly created, for a gate that leaves no trace. `verify` is skipped in that case: the assets it would check were just deleted on purpose |
| `-Project` / `-Engine` | name the project and engine root explicitly |

> **Warning:**
>
> **Run it with the editor closed.** Steps 2 and 4 write `.uasset` files, and an editor open on the
> same project writes them too — whichever saves second wins, and neither says so.

## The docs must not drift

```bash
pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
```

Half of the diagnostics documentation is **machine-written**: the sources are scanned for
`Diagnostics.Error/Warning/Info(TEXT("DFXnnnn"), ...)` and one page per thousand-range is written,
with a section per code holding its severity, message template and the place that raises it. The
human half — cause and fix — is carried across untouched.

`-Check` turns it into a gate: exit 1 if the pages are out of date. That is how CI notices a new code
nobody documented.

> **Note:**
>
> The reason for splitting it this way is direct: **the half that rots (which codes exist, what they
> say) is regenerated, and the half that does not (why it happens) is written once.**

## A minimal pipeline

```yaml
# Sketch: any CI is these four steps plus the docs gate
steps:
  - run: pwsh -File Plugins/DreamFX/.skill/gen-diagnostics.ps1 -Check
  - run: pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

What it needs first: a Windows machine, a resolvable engine (`UE_ENGINE_ROOT` or a registered
`EngineAssociation`), the same content plugins the authoring project enables, and **no editor open on
the project**.

## Deeper checks

Beyond the four steps, a few things are worth running on a release round rather than every commit:

```bash
dfx.ps1 verify -All -StrictVersions   # module version drift becomes an error
dfx.ps1 mirror-diff                    # L1 text + L2 compile
dfx.ps1 asset-diff                     # asset facts, routing around the exporter
dfx.ps1 coverage                       # coverage, bucketed by feature
```

See [round trip and equivalence](../generation/roundtrip.md).
