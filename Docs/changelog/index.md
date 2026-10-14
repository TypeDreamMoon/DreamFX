---
title: Changelog
description: What each DreamFX release covers, plus the 1.0.0 verification snapshot and known issues.
---

Versions follow the plugin (`VersionName` in `DreamFX.uplugin`). This page mirrors the repository's
[CHANGELOG.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/CHANGELOG.md).

## Unreleased

### Fixed

- **5.6 / 5.7: inputs revealed by a static switch are visible again**
  ([#1](https://github.com/TypeDreamMoon/DreamFX/issues/1)). Engines without the external edit API go
  through the `Compat/` layer, and it stopped enumerating module inputs at the first
  `UNiagaraStackFunctionInput`. Module inputs are a **hierarchy**: an input a switch reveals is that
  switch's **child**, not its sibling — `InitializeParticle`'s `Lifetime` hangs under `LifetimeMode`,
  `UniformSpriteSize` under `SpriteSizeMode`. So the probe saw 13 top-level inputs, writing a switch
  looked like it had no effect, and every argument it revealed was reported as a typo
  ([DFX3003](../diagnostics/DFX3xxx.md#dfx3003)). Now it follows the engine's own two rules: descend
  into an input's children, stop at a Dynamic input. The write path uses the same resolver, so
  conditional inputs are addressable too. 5.8 and MoonEngine use the engine's own API and never took
  this path.
- **5.6 / 5.7: input visibility and editability now use the engine's three gates.** `GetIsHidden` +
  VisibleCondition + EditCondition, matching `GetStackInputTopology` word for word. The previous
  `GetShouldShowInStack` / `GetIsEnabled` answer, on those engines, the "show modified only" filter
  and the host node's enabled state respectively, and both return true for everything — harmless
  while only top-level inputs were collected, and not harmless once conditional children are, because
  an unselected branch is a child that exists and is hidden. The decompiler skips inputs on
  `!bVisible || !bEditable`, and is the reader that gets fooled.
- **A failed save no longer takes the editor down.** Four `UPackage::SavePackage` sites (the
  adapter's write, module generation, the module library, the Phase 0 commandlet) used
  `FSavePackageArgs`' default `Error = GError` — and any message on GError is fatal, so "the file is
  in use by another program" (two editors' watchers rebuilding one `.dfs`, a read-only file, an
  antivirus holding it) killed the editor outright, and the
  [DFX5030](../diagnostics/DFX5xxx.md#dfx5030) path was never reached. `SaveArgs.Error = GWarn` puts
  the failure back into the build errors.
- [DFX7102](../diagnostics/DFX7xxx.md#dfx7102) no longer treats `SpawnParticlesInGrid` as rate-based
  spawning: like `SpawnBurst` it is a per-loop burst (X·Y·Z), bounded by the loop. The list keeps
  `SpawnRate` / `SpawnPerUnit` / `SpawnPerFrame`.

### Documentation

- Calling a `DI<X>` input's functions from a `.dfm` body
  (`Wind.SampleWind(Particles.Position, V, G)`) needs the `out` arguments to be **uninitialised
  locals**: `float G = 1.0;` fails the CPU VM compile with `out/inout parameters must be lvalues`
  ([DFX6006](../diagnostics/DFX6xxx.md#dfx6006)) — hlslcc constant-folds the initialised local into
  the argument list before it checks the out parameter. GPU accepts both; write the uninitialised
  form so one body compiles on both targets. See [`.dfm`](../language/dfm.md). The first external
  plugin data interface (DreamWind's `UNiagaraDataInterfaceDreamWind`) works end to end — reflection
  resolution through `DI<DreamWind>`, module input wiring, and `User.Wind` passed from a `.dfs` — with
  no DreamFX changes.

## 1.0.0 — 2026-08-13

The first release. Text sources (`.dfs` / `.dfe` / `.dfm`) → standard Niagara assets
(`UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript`), full decompilation in the other direction,
and a verification system built on the discipline that every claim carries a measurement.

### Language and generation

- **Full `.dfs` system definition**: user parameters (including **data interface parameters with
  their JSON configuration**), the six system/emitter stacks, **OnEvent handlers**, **named
  simulation stages** (iteration source, bindings, `ExecuteBehavior`, `NumIterations`/`Enabled` as
  value *or* parameter), renderer property assignment and `Bind`, Settings (including
  `bFixedTickDelta` and system-level `FixedBounds`).
- **Every value mode**: literal / linked / enum / nested dynamic input / `hlsl { }` / `curve { }`
  (with `Interp` and **`Tangent=Auto|User|Break|None`**; a curve interface the readable form cannot
  express falls back to verbatim JSON — a convenience does not get to lose data).
- Static switches go through the override pin, the same path the engine's own stack UI uses, on both
  engines; **the linked form resolves against the module's own DefaultBinding** — equal is a no-op,
  unequal is refused, and there is no linking mechanism at the call site.
- `.dfe` reusable emitters copied in; `.dfm` module and dynamic input generation, **working on the
  stock engine through the reflection backend**.
- `@version` really selects; R7 provenance stamps (source hash, generator version, module version
  GUIDs).

### Decompilation and mirrors

- Any `UNiagaraSystem` → `.dfs`, byte-for-byte idempotent; anything unexpressible is written into
  the file header as an explicit gap, never silently.
- The `Decompiled/<original path>` namespace: an export is first-class source that rebuilds on save
  and structurally cannot touch the original.
- Editor integration: menus, context menus, toolbars, the VSCode workspace, and Adopt (which refuses
  on any loss).

### The four verification layers

- L1 `mirror-diff` line by line; L2 the mirror's compile; L3 runtime equivalence (fixed-step SimCache
  per-frame particle counts, with an A-vs-A self-comparison deciding decidability); and **asset-level
  fact comparison `asset-diff`** (a reflection walk that never touches the exporter, including the
  `compiled` fact family, with both sides force-compiled first). The 55-case corpus additionally
  compares "the asset built from a fixture" against "the asset rebuilt from its export" — symmetric
  loss has been visible since 2026-08-12.
- Four-step CI: lint → build → verify → corpus; `gen-diagnostics.ps1 -Check` guards against docs
  drift; **143 diagnostic codes, each with file, line and column**.

### Two engines

- MoonEngine (source 5.8.1) and stock installed 5.8.1 from **one source tree with zero `#if` forks**.
- The 1.0.0 round fixed four long-process defects on stock: the UObject count GC gate, ordering
  safety between collection and the live edit surface, a fallback for user parameters the engine drops
  when they have no default, and a fatal modification-during-iteration. Plus the host-project content
  plugin alignment requirement (see [limits](../diagnostics/limitations.md)).

### Verification snapshot (2026-08-13, commit `4e23c55`)

| Gate | Moon (source 5.8.1) | stock (installed 5.8.1) |
| --- | --- | --- |
| ci (lint→build→verify→corpus) | OK, 55 verified / corpus 55 | corpus 55/55 (run alone) |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0 (twice in a row) |
| mirror-diff | L1 45/45, L2 45/45 | L1 24/24, L2 24/24 |
| asset-diff | 6 same / 39 different (all declared families) | — |
| L3 runtime equivalence | 45 pairs: 7 exact / 0 phased / **0 differ** / 38 undecidable | — |
| Diagnostics docs `-Check` | green | — |

### Known issues

See [limits](../diagnostics/limitations.md).
