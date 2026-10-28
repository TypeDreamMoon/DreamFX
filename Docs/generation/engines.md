---
title: Engines and backends
description: How one plugin source tree runs on a source build and on the installed engine — zero #if forks, three outcomes for .dfm generation, and the drift policy for an experimental API.
---

## One source tree, two homes

DreamFX builds and is verified on a **source-built engine** (MoonEngine, 5.8.1) and on the
**installed engine** (stock 5.8.1) — from **one plugin source tree with zero `#if` forks**.

That is a constraint, not a slogan, and it drives a concrete design choice: capability differences
are not forked in the preprocessor, they **select a backend at runtime**, and the two backends are
diffed against each other.

## Three outcomes for `.dfm`

Writing HLSL onto a Niagara custom node needs `UNiagaraNodeCustomHlsl::SetCustomHlsl`, and building
the graph around it needs four more declarations. MoonEngine puts an export macro on all five; a
stock engine exports none of them.

But *exported* and *reachable* are not the same thing — public data members need no export macro,
public virtuals dispatch through the vtable, and a private field that is a `UPROPERTY` can be written
by name. Hence:

| Backend | When | Behaviour |
| --- | --- | --- |
| **direct** | the engine exports the five declarations | calls them; this is MoonEngine |
| **reflection** | it does not, but the shapes it depends on all check out | rebuilds each operation from the public surface |
| **degraded** | a shape it depends on has moved | **refuses to generate**, and says which one |

The reflection backend is **verified against the direct one** rather than assumed equivalent: the
same `.dfm` built both ways reads back with a byte-identical schema, and that holds across engines.

```bash
# force the reflection backend on an engine that does not need it —
# this is how the two paths are diffed on one machine
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Modules/ToonSpin.dfm -ForceReflectionBackend
```

> **Note:**
>
> **The product was never the limited part.** A generated module is an ordinary `UNiagaraScript`: any
> engine loads it, references it, cooks it and runs it. *Generate anywhere it works, commit the
> asset, everyone consumes it.*

When neither backend can run:

| Situation | Outcome |
| --- | --- |
| asset committed and matching the source | build skips it, CI stays green |
| source edited without regenerating | [DFX5107](../diagnostics/DFX5xxx.md#dfx5107) |
| no asset at all | [DFX5100](../diagnostics/DFX5xxx.md#dfx5100), naming the check that failed |

## An experimental API: drift must surface

DreamFX depends on the engine's Niagara external edit API (`UNiagaraExternalEditUtilities`), which
the engine marks EXPERIMENTAL. **Startup self-checks surface drift; it is never absorbed silently.**

The cost of that choice is that an engine upgrade may require touching DreamFX. The benefit is that
it will not, unnoticed, generate an asset through a degraded path that looks right and is not.

## Host projects must align their content plugins

The module probe needs a baseline. If the authoring project enables a content plugin such as
`NiagaraFluids`, consumers must enable it too; otherwise sources referencing its modules surface as
gaps or compile errors.

**A whole family of unresolvable names sharing one prefix is a mount point that is missing, not a
source that is wrong.** This is an environment requirement, not a defect.

## Older engines (5.6 / 5.7)

The external edit API arrived in 5.8. Older engines go through a `Compat/` layer that supplies the
same API by another route, so the adapter needs no changes at all. That work is **unreleased** — the
1.0.0 release round was verified on 5.8 — and the two fixes so far are in
[the changelog](../changelog/index.md).

One of them is worth knowing about. The compat layer enumerated module inputs and stopped at the
first one, but module inputs are a **hierarchy**: an input revealed by a switch is that switch's
**child**, not its sibling. So a static switch looked like it had no effect, and every argument it
revealed was reported as a typo (DFX3003). 5.8 and MoonEngine use the engine's own API and never take
that path.

## Release verification snapshot

The 1.0.0 gates (2026-08-13, commit `4e23c55`) on both engines:

| Gate | Moon (source 5.8.1) | stock (installed 5.8.1) |
| --- | --- | --- |
| ci (lint→build→verify→corpus) | OK, 55 verified / corpus 55 | corpus 55/55 (run alone) |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0 (twice in a row) |
| mirror-diff | L1 45/45, L2 45/45 | L1 24/24, L2 24/24 |
| asset-diff | 6 same / 39 different (all declared families) | — |
| L3 runtime equivalence | 45 pairs: 7 exact / 0 differ / 38 undecidable | — |
