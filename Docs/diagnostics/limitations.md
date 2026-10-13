---
title: Limits
description: What is deliberate, what is not done yet, what the round trip cannot carry, and the 1.0.0 known issues — each with the way it surfaces.
---

This page states what DreamFX does not do. There is one principle behind all of it: **every
degradation is diagnosed, never silent.** So each entry below comes with the way it surfaces.

## Deliberate

| | Why |
| --- | --- |
| **Scratch Pad** | A scratch pad script is a graph embedded in the asset, with no stable name to address it by |
| **Lowering a module body into a node graph** | A `.dfm` body becomes one custom HLSL node. Lowering it is "tier two": the ~13k-line problem DreamShader already has, reproduced |
| **A general expression compiler** | Inline expressions are arithmetic plus a short whitelist ([DFX4031](DFX4xxx.md#dfx4031)). Anything more belongs in `hlsl { }` or a `.dfm` |
| **Implicit native inheritance through `from`** | `from` copies and merges `.dfe` source on rebuild. Native asset inheritance uses [`inherits`](../language/native-emitter-inheritance.md) explicitly |
| **`MaterialParam`** | Reserved syntax, not implemented ([DFX5093](DFX5xxx.md#dfx5093)) |
| **`#Region` reaching the asset** | The external edit API has no stack-note function (L5) |

`Group` and `SortPriority` populate the asset's user parameter hierarchy. The former
[DFX5099 limitation](history/DFX5099.md#dfx5099) remains documented for old links, but is no
longer a restriction in the published implementation. See the [system reference](../language/dfs.md).

## Not done yet

| | What happens now |
| --- | --- |
| GPU/CPU branch conditions | not expressible |
| Scalability condition logic | not expressible (an emitter's `QualityLevelMask` **is** supported) |
| Compile-time folding for a `.dfm` static switch | accepted, validated, then lowered as an ordinary input ([DFX5102](DFX5xxx.md#dfx5102)) |
| Full readable syntax for curve data interfaces | anything configured outside the keys falls back to verbatim JSON |

Generic stages also support dispatch sizing and particle-iteration filters. Their supported
arguments are listed in the [system reference](../language/dfs.md); custom C++ stage classes
remain a gap.

## What the round trip cannot carry

**[DFX8014](DFX8xxx.md#dfx8014):** **Limits of native emitter inheritance.** System exports preserve
`inherits` and pin the parent version, with documented limits for complete stack/renderer
overrides, event/stage snapshots and unavailable parent baselines. Standalone `.dfe` exports
still flatten the parent relationship. See [native emitter inheritance](../language/native-emitter-inheritance.md).

**[DFX8015](DFX8xxx.md#dfx8015):** **A second event handler on one emitter.** One is representable; more are not. A standalone `.dfe`
export cannot carry a handler's `Source` either, because there is no sibling emitter to name.

**[DFX8016](DFX8xxx.md#dfx8016):** **Custom C++ stage classes.** Ordinary stages export as `Stage name(...)`; a stage whose class is not
the engine's generic one does not.

Two more that raise no code but are worth knowing:

- **The other fields of a renderer list element.** `Meshes` is written as an array of paths, but each
  element is really a struct — a mesh's per-element pivot, scale and LOD range have no syntax. An
  export that would drop one says so in the file header.
- **Inline expressions do not come back as themselves.** They return as an equivalent `hlsl { }`
  block. **The round trip is semantic, not textual.**

## 1.0.0 known issues

1. A **linked** declaration switch on a nested dynamic input node at **depth > 1** is unguarded — an
   engine defect that is reachable but has zero hits in the corpus; the guard is planned to reach that
   depth in 1.0.x.
2. **Emitter-level `FixedBounds` is not carried** (system-level is).
3. `bGpuAlwaysRunParticleUpdateScript` is not carried.
4. Data interface configuration rides as verbatim JSON (readable syntax pending); with a bare
   `DI<T>` plus a link, the count of leftover default instances is unstable — that is judgment noise,
   not data loss.
5. A UE 5.8.1 refPath import regression — an engine bug, worked around with
   `NormalizeObjectReferences`, to be removed after a 5.8.2 retest.
6. **The L4 picture comparison is a manual, optional step** (the OBS recipe is verified, not
   scripted).
7. The long tail listed as not-done: custom C++ stage classes, parameter-driven iteration counts and
   so on — the gap header names them, and the corpus has no instances.

## These are not defects — they are requirements

> **Warning:**
>
> **Host projects must enable the same content plugins as the authoring project** (e.g.
> `NiagaraFluids`). With the plugin unmounted the module probe has no baseline, and sources referencing
> its modules surface as gaps or compile errors. **A whole family of unresolvable names sharing one
> prefix is a mount point, not a typo.**

> **Warning:**
>
> **Package-writing commands need the editor closed** (`build`, `corpus`, `mirror-diff`,
> `decompile-all`). Two processes saving the same packages race silently.

> **Warning:**
>
> **Despawn a system's instances before rebuilding it.** During a hot swap a live `NiagaraActor` can
> see a half-built renderer layout and assert on a render worker. It is an engine race, not something
> DreamFX can guard.

## Measure your own coverage

Somebody else's number does not answer your question:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
```

It reports how much of a content tree is expressible, bucketed by feature. Unpacked in
[round trip and equivalence](../generation/roundtrip.md).
