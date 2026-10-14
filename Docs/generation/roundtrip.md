---
title: Round trip and equivalence
description: The decompilation contract, how a gap surfaces, what each of the four verification layers answers, and three lessons in "all green is not the same as correct".
---

Decompilation is not a convenience export. It is a contract:

1. any `UNiagaraSystem` exports to a `.dfs`;
2. the export is **byte-for-byte idempotent** — build the export into an asset, export again, get the
   same text;
3. anything the language cannot express is **written into the file header as an explicit gap** and
   reported as a `DFX8xxx`, never dropped in silence;
4. `Adopt` re-exports and compares before taking an asset over, and refuses on any mismatch.

Point 3 carries the weight. An exporter that degrades silently leaves you staring at a wrong-looking
effect three months later with nothing to pull on.

## What a gap looks like

**[DFX8014](../diagnostics/DFX8xxx.md#dfx8014):** System exports preserve native `inherits` and the
selected parent version, but complete overrides, event/stage snapshots and unavailable parent
baselines still have limits. Standalone `.dfe` exports flatten the relationship. See
[native emitter inheritance](../language/native-emitter-inheritance.md) for the exact contract.

**[DFX8015](../diagnostics/DFX8xxx.md#dfx8015):** More than one event handler on an emitter cannot be carried, and the export names what the rebuilt
emitter will no longer receive.

**[DFX8016](../diagnostics/DFX8xxx.md#dfx8016):** A stage of a custom C++ stage class — not the engine's generic stage — has no DreamFXLang form.

Every one of them appears in two places: the exported file's header, and the log.

## The four layers

| Layer | Command | Question it answers |
| --- | --- | --- |
| **L1** | `mirror-diff` | does the mirror's export match the original's, **line by line**? |
| **L2** | `mirror-diff` | does the mirror **compile clean**? |
| **L3** | the runtime-equivalence fixture | does the rebuilt system **simulate** the same? Fixed-step SimCache, per-frame particle counts |
| **asset-diff** | `asset-diff` | do the two agree **as assets**? Reflection-walked facts, independent of the exporter |

`mirror-diff -NoCompile` skips L2 — much faster, and the right thing when the mirrors were built in
the same session that is now diffing them.

### Why the fourth layer has to exist

Of the first three, L1 compares two outputs of the **same exporter**. Something the exporter itself
drops is dropped symmetrically on both sides: L1 is byte-identical, L2 still compiles clean, and L3 —
which counts particles — sees nothing. The picture, meanwhile, is wrong.

That is not hypothetical. In one real case L1 was byte-identical, L3 reported exact, and the effect
looked wrong; the cause was a derived bit (`bBindingExistsOnSource`) that appears in no export at all.
So `asset-diff` takes another route entirely: it **reflection-walks the facts of both assets**,
including the compiler's own view (the `compiled` fact family), and never touches the exporter.

### The corpus compares a fifth thing

On top of that, the 55-case corpus compares "the asset built from a fixture" against "the asset
rebuilt from its export". That catches **symmetric loss**: something dropped identically on both
sides of a text comparison, and therefore invisible to it.

## Three measurement lessons

> **Warning:**
>
> **Measure one system per run.** The L3 fixture can only measure one system per exec; more than that
> and the same asset gets reported as different things.

> **Warning:**
>
> **A random system with no determinism is not equal to itself.** So L3 uses an A-vs-A self-comparison
> to decide decidability first: a system that disagrees with itself is judged undecidable, not
> different.

> **Warning:**
>
> **Behavioural divergence with everything statically green ⇒ measure the derivative.** In the case
> where birth speed was off by exactly half, the cause was a lost `bFixedTickDelta` — one missing entry
> in the Settings table. A line diff reports only the first difference, so fixing one means re-running
> the whole thing.

## Measuring your own content

```bash
# how much of my content can be represented? bucketed by feature
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage

# export a whole content tree to source, in one editor boot
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX

# L1 + L2
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff -Path /Game/VFX

# asset facts, independent of the exporter; -DumpFacts writes both sides to Saved/DreamFX/*.facts
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff -Path /Game/VFX -DumpFacts
```

> **Note:**
>
> `asset-diff` **force-compiles both sides** before comparing. Without that, the `compiled` fact
> family reports each side's compile history rather than its content, which is not a comparison of
> anything. `-NoCompile` is for diagnosis only.

## The mirror namespace

Exports land in a `Decompiled/<original path>` namespace: the whole tree is first-class source that
rebuilds into **mirrors** and structurally cannot touch the originals. Unpacked in
[Export vs Adopt](adopt.md).
