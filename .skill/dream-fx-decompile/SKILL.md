---
name: dream-fx-decompile
description: Export an existing Unreal Niagara system back into DreamFXLang source headlessly, so a hand-built effect can be migrated to a text source file. Use when asked to decompile, export, reverse, convert, or migrate a Niagara system / NS asset to .dfs, or to report how much of a project's VFX DreamFX can represent.
---

# dream-fx-decompile `<asset path>`

Turns a `UNiagaraSystem` back into a `.dfs`. The migration path for effects that already exist, and
the round-trip check that keeps the generator honest.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/FX/NS_Spark -Out DFX/NS_Spark.dfs
```

Without `-Out` the source goes to the log.

| Argument | Effect |
| :-- | :-- |
| *(positional)* | the asset — `/Game/FX/NS_Spark`, or `Plugin.MoonToon:FX/NS_Spark` |
| `-Out` | write here instead of printing |
| `-Root` | the `Root="..."` to stamp on the output, default `Game`. Asset paths under that root are shortened |

## The `Decompiled/` namespace

An export's `Name=` is **not** the asset it was read from. `/Game/FX/NS_Spark` exports as
`Name="Decompiled/FX/NS_Spark"`, so building the file writes `/Game/Decompiled/FX/NS_Spark` — a
mirror — and the original cannot be reached however the file is edited. That is what makes the whole
`DFX/Decompiled/` tree ordinary source: saving an export rebuilds its mirror, and `build -All`, lint
and CI cover it like anything else.

Rehoming is idempotent, so re-exporting a mirror reproduces the mirror's own source rather than
nesting a second copy.

To make the text authoritative for the *original* asset instead, that is **Adopt** — a different
command, which refuses when anything about the asset cannot be expressed.

### Batch export and mirror comparison

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path '/Game/FX+/Game/Explosions'
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build -All -Force
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff -Path '/Game/FX+/Game/Explosions'
```

`decompile-all` exports every system under the given paths in one editor boot (`+` or `,` separates
paths). `mirror-diff` then decompiles the original and its mirror and compares the two texts line by
line — a stronger check than the round-trip corpus, which only reads back what DreamFX itself wrote.
Only the `// Decompiled from` line is excused. `-NoCompile` skips the per-mirror compile check.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff -Path '/Game/FX' -DumpFacts
```

`asset-diff` is the one that does **not** go through the exporter. It walks both assets by reflection
and compares facts as multisets, so a loss the exporter makes on both sides — invisible to
`mirror-diff`, whose two inputs are that same exporter's output — lands here as a fact one side has
and the other does not. Exit code is the number of systems that differ.

It compiles both sides first, and has to: `PostLoad` throws away a cached script VM whose stored id
does not match its graph, so an asset last compiled by an older engine reads as having no simulation
stages, no data interfaces and no written attributes. `-NoCompile` skips that — diagnostic only, and
what it then reports about the `compiled` fact family is each side's compile history rather than its
content. `-DumpFacts` writes both sides' full fact lists to `Saved/DreamFX/*.facts`; the console
report truncates each fact to 400 characters, which is exactly wrong for the long ones.

## What it does and does not recover

**Recovered**: system and emitter settings that differ from a new asset's, user parameters, every
stack with its modules in order, non-default module inputs, dynamic input chains, curve literals with
their tangents, `hlsl { }` expressions, Set Parameters modules as assignment blocks, renderer
properties and `Bind` statements.

**Normalised, not reproduced**:

- **Inline arithmetic comes back as `hlsl { }`.** `User.Speed * 0.6` exports as
  `hlsl { (User.Speed * 0.6) }`. L6 is explicit that the reverse direction is equivalence, not
  reproduction.
- **`from "..."` emitter references are inlined.** Copy semantics mean "which parts were overrides"
  is not recoverable, so an export is always self-contained.
- **User parameter defaults are dropped.** The read API reports a user variable's name, type and
  description and no value, so the export declares without a default. The runtime default stays on
  the asset; rebuilding does not clear it.

**Consequence for round trips**: text is idempotent from the *second* pass on, not the first. Export,
rebuild, export again — the two exports match byte for byte. That is one of the two properties the
corpus tests. The other is that the *asset* survives: a fixture's asset and the asset rebuilt from
its export must hold the same reflected facts. Text idempotence alone cannot see a loss the exporter
makes on both sides — every curve tangent and every stage binding missing before 2026-08-12 was
symmetric in exactly that way, and therefore silent.

## Why an export is always rebuildable

Two rules, both learned the hard way:

- **Module names are printed at whatever length disambiguates them.** `InitializeParticle` exists
  twice in the engine, so it exports as `Spawn/Initialization/InitializeParticle`.
- **Inputs the writer would refuse are never exported.** An input hidden by a static switch, or with
  a false EditCondition, cannot be written back — so printing it would produce a file that does not
  rebuild. Those are dropped and counted as gaps instead.

Default suppression is what makes the output readable at all. The read API reports resolved values
with no "explicitly set" flag, so every input would otherwise be dumped. DreamFX adds each module to
a throwaway probe system, reads its pristine values, and prints only what differs. The cost is that
"explicitly set to the default value" is indistinguishable from "not set", and is lost.

## Coverage report

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage -Path /Game
```

Decompiles every Niagara system under a path and reports how many came back whole, plus a
frequency-ordered list of what could not be represented. This is how v2's feature order should be
decided — by what the project actually contains. `-Path` takes several paths separated by `+`, which
matters because booting the editor is most of what a scan costs. Assets under `Decompiled/` are left
out: they are this pipeline's own output, and counting them would report every gap twice.

```text
=== DreamFX coverage over 2 Niagara system(s) under /DreamFX ===
  ok      /DreamFX/Samples/NS_ToonHitSpark  (1 gap(s))
  ok      /DreamFX/Samples/NS_DreamFXPhase1
=== 2 exported, 0 failed ===
Gaps, most common first:
     1 x  dynamic input static switch
```

Exit code is the number of systems that failed to export.

## After decompiling

The output compiles as-is, but it is machine-written. Worth doing by hand:

1. **Leave `Name=` alone** unless you mean to change what the file builds. It already points at the
   mirror, so saving the file is safe; pointing it back at the original is what *Adopt* is for, and a
   file in the decompiled tree that names an asset outside `Decompiled/` is refused (DFX8013).
2. **Put back the inline arithmetic** the exporter turned into `hlsl { }` — it reads better and lint
   can see into it.
3. **Restore user parameter defaults**, which the export could not read.
4. **Rebuild and diff** to confirm nothing moved:
   `pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build <file> -Force`

## See also

- [`dream-fx-verify`](../dream-fx-verify/SKILL.md) — the build harness
- [`dream-fx-diagnose`](../dream-fx-diagnose/SKILL.md) — when a rebuilt export does not compile
