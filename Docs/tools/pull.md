# `pull`: write current asset values back to source

> Recorded on 2026-10-09 on branch `feat/pull`. Tool: `dfx pull`, implemented in
> `Source/DreamFXEditor/Private/WriteBack/DreamFXPull.cpp`.
>
> [Write-back coverage](write-back-coverage.md) records measured gaps. The separate research note
> `write-back-from-dreamgui.md` describes DreamGUI/DUI techniques worth reusing. This document
> covers the semantics, boundaries and measurements of `pull` itself; it does not replace those notes.

## 0. Summary

**`pull` copies a value from an asset's rapid-iteration store into a literal already declared in the source.**

The workflow is “author structure in text, tune values in the editor.” The final step needs care:
the tuned value lives in the asset, while the text either omits that input entirely (so rebuilding
restores the module default), or contains what the **read path sees**, rather than what the **store
contains**. For `NS_Effects1_Mesh` and `Sprite_Atlas_Size.MeshYaw`, the store contains `-90`, yet
every pin read returns `0.0`. `pull` makes the stored, tuned value part of the source.

## 1. Usage

```powershell
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# Dry run: report proposed line edits without writing any source bytes (the default).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Engine $eng

# Apply: back up the source before writing values.
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Apply -Engine $eng

# Read another asset: exported source targets a mirror, but the tuned values are in the original.
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Asset /Game/FX/NS_Spark -Apply -Engine $eng

# Structural edits: add/remove lines only when correspondence is unambiguous (section 4d).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Structure -Apply -Engine $eng

# Ignore the baseline and compare all declared values (section 4c; also used when no baseline exists).
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -NoBaseline -Apply -Engine $eng
```

| Argument | Meaning |
|---|---|
| `pull <file.dfs>` | Source to update; exactly one `.dfs` system document |
| `-Apply` | Write the source. **Without it, the command is a dry run** |
| `-Asset=<package path>` | Read **this** asset instead of the one named by the source's `Name=` |
| `-Structure` | Allow **structural edits**, adding or removing lines; otherwise report them only |
| `-NoBaseline` | Ignore the previous `-Apply` baseline and compare everything (section 4c) |

`-Asset` is more than a convenience: **an export cannot structurally target the asset it was
exported from**. Plan-v4 V1 puts all exports in the `Decompiled/` namespace so rebuilding cannot
touch originals. Therefore reading tuned values from an original into its export requires this
override. The source's `Name=` still determines what `build` writes; `-Asset` only selects what
`pull` reads.

### Output locations

| Content | Location |
|---|---|
| Source before writing, one backup per file | `Saved/DreamFX/Pull/<source path relative to project root>`, e.g. `Saved/DreamFX/Pull/DFX/Decompiled/Game/FX/NS_Spark.dfs` |
| Report for this run, including dry runs | The same path with extension `.report.txt` |

Backups and reports stay in `Saved/`, **outside `DFX/`**. Inputs stored in the asset but undeclared
in the text are reported **one by one, without truncation**. Real assets can have dozens:
`破空灰尘` has 94. These names appear only in this report, and each represents an input that a
rebuild must recover from its module default. Truncation would hide that risk.

The editor exposes the same operation through **Write Back to Source** in the asset context menu
and the Niagara system editor toolbar's DreamFX menu. It is equivalent to `-Apply`, with `-Structure`
controlled by `bPullStructuralEdits`. Optional write-back on asset save uses `bPullOnAssetSave`
(default off). After writing a file, the editor notifies the source watcher so it does not rebuild
the asset it just read. See [editor integration](editor-integration.md#write-back-to-source).

## 2. What `-Apply` means

- **Without `-Apply`**, read the asset, parse the source and compute each replacement literal.
  Print `file(line,column): info DFX7116: pull: <module>.<input>: <old literal> -> <new value>`
  and stop. No source bytes change; the report file is still written.
- **With `-Apply`**, back up the source and replace only **each literal's own byte range**.
  The result **must parse successfully** before it is saved. A parse failure reports `DFX7110`
  and leaves the source unchanged.
- **Idempotent**: a second `-Apply` writes no bytes (section 5, case 1). Comparison is **by value**,
  not spelling: lower the literal with the same `FValueLowering::Lower` as `build`, then compare
  its bytes with the store. `0` and `0.0` are equal and are not reformatted.
- **`pull` never writes assets.** It uses `LoadObject` and read-only APIs, with no `Save` path.

## 3. Relationship to `decompile` and the build safety gate

These operations cover different parts of bidirectional write-back:

| Operation | Direction | Reads | Does |
|---|---|---|---|
| `decompile` | Asset → text | Live input **pins** | Writes structure; `-NoDefaults` includes **every input name**, with pin-derived values |
| **`pull`** | Asset → text | Rapid-iteration **store** | Updates literal values **already declared** in the text |
| `build` | Text → asset | Source text | Writes assets; **safety gate DFX8018** prevents saving a rebuild that would lose facts the text cannot express |

1. **`decompile` supplies keys; `pull` supplies values.** `pull` does not invent undeclared inputs:
   that changes structure. First make the keys explicit, most easily with `decompile -NoDefaults`.
   [Write-back coverage, section 6.4](write-back-coverage.md#64-read-path-export-every-input-explicitly),
   calls this the read mode for write-back; `pull` relies on it.
2. **`pull` complements the safety gate.** The gate catches a changed fact whose structural address
   exists on both sides but whose input is absent from the source. `pull` makes the input explicit.
   For `NS_Effects1_Mesh`, targeting the original trips the gate on two
   `MeshYaw: 0000B4C2 -> 00000000` facts (coverage section 6.8, row 6). Write `-90.0` into the source
   with `pull`, then `build` succeeds: the source declares the changed value, so rule 3 allows it.
3. **Stored but undeclared values are reported, not added** (`DFX7106`). Whether they would
   actually disappear is the gate's (`DFX8018`) decision. Values equal to module defaults are
   restored on rebuild without losing facts: of 37 undeclared entries in the section 6.8
   measurement, only 8 were really lost. `pull` does not recompute defaults, which would introduce
   another probe and default table that could drift away from the gate.

## 4. What can and cannot change

### Initial scope: literal arguments of existing inputs in existing module calls

All four correspondence checks verify identity rather than guess:

1. **Node order**: a source statement corresponds to the same ordinal node in the same emitter
   and stack in the asset.
2. **Module identity**: the `UNiagaraScript` resolved by `FModuleLibrary::FindModule` must have
   exactly the same asset path as the script used by that node.
3. **Node name**: when the statement contains `as <name>`, the asset node must have that name.
4. **Collapsed assignments**: consecutive assignments, collapsed by L2 into one Set Parameters
   module, count as **one** node; its counterpart must be `bIsSetParameters`.

Any mismatch rejects **the entire stack** (`DFX7109`, listing nodes on both sides). Editing a
literal through an off-by-one correspondence is more dangerous than declining the edit.

For values:

- Lower the argument through `FValueLowering::Lower` using **the input's actual type**. Failure,
  or a nonliteral form (link, dynamic input, `hlsl {}`, `curve {}`, DI, object reference), reports
  `DFX7111` and leaves it untouched. **Changing value mode is an edit, not write-back.**
- A store value that is a data interface or object reference is also left untouched.
- The DSL normalizes spaces, hyphens and case. `Scale RGB` (Vector3) and its inline condition
  `ScaleRGB` (bool) therefore become **the same identifier**. The **value's type** selects the
  intended input, exactly as in generation. If it actually means another input, this store entry
  is outside its responsibility and is silently skipped. Multiple accepting candidates produce
  `DFX7111`; the command does not guess.

### Initial exclusions

The following table describes v1; section 4b records later coverage extensions.

| Exclusion | Result |
|---|---|
| New arguments, new/deleted modules, any structural change | `DFX7106` for a stored input absent from the text |
| Event stacks `OnEvent` and simulation stages `Stage` | `DFX7111`; v1 does not address their separate focus-slice read path |
| Renderer properties and system/emitter `Settings` | Initially outside call arguments; covered by safety gate `DFX8018` |
| Assignment values (Set Parameters entries and `Defaults = {}`) | Not addressed in v1 |
| A stack inherited via `from "..."` but absent from this file | Only stacks written in this file are read; `Emitter.FromPath` emits a Verbose message |
| Non-module-input fact families (`di` / `compiled` / `user`) | Neither read nor reported |

### Whitespace, line endings and comments

A replacement covers only `FValue::StartOffset/EndOffset`, the byte span recorded by the lexer
(section 6). Trailing comments, indentation and line-ending style remain unchanged. The result
must still parse.

## 4b. Coverage: the source's declarable values (second pass, 2026-10-09)

The second pass extends v1's module-call literals to the source's existing declaration surfaces.
The rules remain: write only declared addresses, refuse ambiguity, replace only literal bytes.

| Kind | Example address | Value source | Source spelling |
|---|---|---|---|
| Module argument | `Module.Input` | Rapid-iteration store, `Constants.<emitter>.<node>.<input>` | `LiteralToSource` / `EnumEntryToSourceToken` |
| Renderer property | `MeshRenderer.FacingMode` | `GetRendererProperties`, the JSON consumed by `SetRendererProperties` | `RenderJsonPropertyAsSource` |
| `Bind X -> Y` target | `Bind CustomSorting -> ...` | `GetRendererBindings`, reading the live struct through the adapter | Write the name directly |
| System/emitter `Settings` | `Settings.WarmupTime` | `GetSystemProperties` / `GetEmitterProperties` | `RenderSettingSource` |
| Collapsed assignment, a Set Parameters entry | `<node>.<parameter name>` | That node's constant, matched **by parameter name** | As above |
| `Defaults = { }` | `Defaults.Particles.Foo` | `GetParameterDefaults` | As above, including link forms |

Three implementation decisions matter:

1. **Settings read-back uses the generator's own table.** `RenderSettingSource` reverses
   `PlanSettings` and lives alongside `SystemSettings` / `EmitterSettings`. A second table would
   introduce another source of drift, precisely the decompiler-field-table failure recorded in
   coverage section 3.8. Aliases must run in reverse: writing maps `CPU -> CPUSim`; reading must
   map `CPUSim -> CPU`, or `pull` emits a spelling the table does not accept.
2. **Renderer values have one spelling implementation**, `WriteBack/DreamFXSourceValue.h`,
   shared by the decompiler and `pull`. Different spellings would rewrite correct lines every
   time. The extraction was intended as a pure move; 14 RoundTrip fixtures caught a mistake:
   leftover `break` statements after replacing the old `switch` caused only the first renderer
   property to be emitted, failing five fixtures.
3. **Compare values, not strings.** `LowerDeclaredPropertyToJson` uses generator code to lower
   source values before comparison with asset JSON. String comparison would replace shorthand
   `Material = "Plugin.MoonToon:..."` with an absolute path, and would repeatedly rewrite `0.1f`
   because the property converter widens `float` to `double`. Integers compare exactly; other
   numbers compare at the `float` precision the language can express.

Renderer properties, `Settings`, assignments and `Defaults` are now covered, with assertions.

### 4c. Dirty values: write only what actually changed in the asset (`DFX7115`)

Comparing asset and source directly is correct on the first run, but a later difference may mean
**someone edited the source**. In that case the asset is older, and writing it back discards the
newer decision. Like DreamGUI, `pull` writes changed values only, with a reproducible baseline.

The last `-Apply` records `Saved/DreamFX/Pull/**/<file>.baseline.txt`: sorted
`scope<TAB>key<TAB>value` triples, using the same scope/key division as the safety gate.
The alternatives cannot answer the required per-value question:

- `UPackage::IsDirty` is one bit for the whole asset, can be set by loading, and says nothing
  useful to a commandlet opening the asset in a fresh process.
- Editor modification events also lack the required granularity and do not survive processes.

| Condition | Outcome |
|---|---|
| Values differ; the baseline contains this entry and equals the asset's current value | The source is newer: preserve it, report `DFX7115`, do not write |
| Values differ; there is no baseline entry (new value or first run) | Write |
| Values differ; the baseline entry differs from the current asset value | The asset changed again: write |
| Recording a baseline | Only `-Apply` records it. A dry run has not reconciled differences and must not mark them handled |
| No baseline on disk | Compare everything and report `DFX7114`; `-NoBaseline` explicitly requests the same behavior |

Boundaries:

- Only **source-declared addresses** enter the dirty set or receive baseline protection.
- The baseline records **observed**, not necessarily written, values. A value rejected with
  `DFX7111` is still recorded as the asset's current state and counts as unchanged next time.
  This intentionally avoids asking about the same refusal on every run.
- Renderer addresses are positional (`emitter X renderer N`); changing order makes the
  baseline mismatch, so values are treated as dirty.
- Deleting the baseline always restores full comparison.

### 4d. Structural editing: `-Structure` (report only by default)

V1 rejects a whole stack whose structure differs (`DFX7109`). An explicit switch now permits
unambiguous edits. Compute correspondence first: guessing would move modules, not just text.

1. Align nodes by their module asset. Consecutive assignments count as **one** sentinel-key node;
   two Set Parameters nodes are distinguished by **entries**, not positions.
2. Use a **longest common subsequence only if it is unique**. A candidate pair `(i,j)` lies on an
   optimal alignment when `LCS(prefix) + 1 + LCS(suffix) == LCS(total)`. The eligible pairs must
   number exactly the LCS length and strictly increase on both sides, making them forced pairs.
   Repeated modules plus another deletion can yield multiple LCS alignments; those are refused.
3. Verify `as <name>` for every pair after alignment, since node names are not part of the key.

Supported edits:

- An asset module absent from the source **inserts one line**, before the next source statement
  in execution order, or before the stack's `}` if it is last. The form is
  `<module>@<major.minor>[ as <node name>](<inputs>);`. The version pin preserves the asset's
  module version. Emit `as` only when the node name differs from the module asset name; otherwise
  the engine's default naming suffices. Emit only inputs actually in the store, in declaration order.
- A source statement absent from the asset **removes its line** only if it owns that line:
  whitespace before and after, no trailing comment. A comment is not an asset-owned structure
  and must not be deleted; refusal explains why.
- **Reuse the file's style.** Take indentation from the nearest statement in this stack, then
  another stack in this document, then four spaces. Take line endings from the insertion line.
- Refuse with a reason: modules with DI/object-reference inputs that cannot be rebuilt from that
  call; undeclared Set Parameters nodes whose collapsed assignments lack supporting fixtures;
  statements without byte spans; statements sharing a line; stacks without a recorded closing span.

Without `-Structure`, addressable differences produce `DFX7113`, describing both sides and the
switch that can apply them. **No bytes in that stack change**, since positional value addressing
cannot proceed while its structure is unresolved. `DFX7109` remains “cannot establish
correspondence at all.” `DFX7112` reports each **applied** structural edit.

## 4e. Editor entry point: the bridge `pull` action

Command-line `pull` starts a headless editor and reads assets **from disk**. The VS Code extension
has a different situation: the editor is already running, values tuned in Niagara may be unsaved,
and the text buffer may be newer than the file on disk. The bridge's `pull` action
(`Saved/DreamFX/Bridge/`; see [editor integration](editor-integration.md)) handles this case through
the same implementation, `FPuller::PullBuffer`.

The request uses the same `protocol` as other actions:

| Field | Meaning |
|---|---|
| `action` | `"pull"` |
| `sourceFile` | The absolute path of this `.dfs`. Only system documents (`.dfs`) are accepted; `.dfe`, `.dfm` and all other extensions return `ok:false` |
| `text` | Optional. The client's **complete buffer**, which may be unsaved |
| `assetPath` | Optional. Equivalent to `-Asset`: read values from this asset |
| `structure` | Optional boolean. Equivalent to `-Structure` |
| `apply` | Optional boolean. Its meaning is described below |

**Whether `text` is present determines the behavior:**

| | With `text` | Without `text` |
|---|---|---|
| Input processed | `text` (the buffer) | `sourceFile` on disk |
| Source file | **Never written.** The client owns the buffer and decides when to save | As on the command line: only `apply:true` writes, after backing up to `Saved/DreamFX/Pull/` |
| Result text | Returned in `pull.newText`, including previews, regardless of `apply` | No `newText` |
| Effect of `apply:true` | **Record the baseline only**: the client promises to put `newText` into its buffer | Write the file and record the baseline |
| Missing `sourceFile` | Allowed: the buffer may never have been saved | `ok:false`, `No such source file` |

The asset is the object **in the editor process**: `LoadObject` returns the in-memory object for an
already open asset. Values changed in the Niagara editor but not yet saved are therefore the values
read here. Command-line `pull` can only see the asset on disk.

**The baseline is shared with the command line.** Its path depends only on `sourceFile`
(`Saved/DreamFX/Pull/<relative path>.baseline.txt`). Applying on the command line and then previewing
the same file in the editor uses the same record. Values whose text is newer than the asset are
still withheld by `DFX7115`; a buffer request cannot silently change them back. A preview without
`apply`, like a command-line dry run, **does not record** a baseline.

The response keeps the bridge envelope (`protocol`, `requestId`, `ok`, `durationMs`, `message`,
`diagnostics`). `message` is a one-line summary, for example
`1 value(s) differ; 1 withheld (text is newer) (preview)`. A nested `pull` object adds the results:

```json
{
  "ok": true,
  "message": "1 value(s) differ (preview)",
  "diagnostics": [ ... ],
  "pull": {
    "changed": true,
    "newText": "...",
    "edits": 1,
    "compared": 3,
    "changedValues": 1,
    "undeclared": 5,
    "unwritable": 0,
    "unaddressable": 0,
    "withheld": 0,
    "added": 0,
    "removed": 0,
    "structureRefused": 0,
    "baselineUsed": false,
    "baselineWritten": false,
    "wroteFile": false,
    "reportPath": "…/Saved/DreamFX/Pull/….report.txt",
    "report": ["…"]
  }
}
```

- With `text`, `changed` means `newText` differs from the request's `text`; without `text`, it means
  the file was written. `changedValues` is `FPullResult::Changed`: the number of values that differ
  from the asset.
- `newText` is present only when the request includes `text`. On `ok:false`, such as a parse
  failure, it equals the request's `text` unchanged, `changed` is `false`, and `diagnostics` explains why.
- A preview actually produces the rewritten text. If that text fails to parse (`DFX7110`), the
  preview returns `ok:false`. A command-line dry run does not generate rewritten text and does not
  have this failure mode.
- The report file (`reportPath`) is written even during a preview, as on the command line.

## 5. Measurements (2026-10-09; raw logs in `Saved/pull-probe/runs/`)

All four cases used real assets and source copies under `Saved/pull-probe/`; the project's `DFX/`
tree was untouched. The originals `NS_Effects1_Mesh.uasset` and `破空灰尘.uasset` were unchanged:
their SHA256 hashes matched `Saved/DreamFX/gate-probe/asset-backup/` byte for byte, with mtimes
10-08 17:38 and 03-04 22:47, both earlier than this test.

### Case 1: `NS_Effects1_Mesh`, `Sprite_Atlas_Size.MeshYaw` (store `-90`, pin `0.0`)

```powershell
# Export every key. A normal export has no MeshYaw line (the shape tested in case 2).
pwsh -File $dfx decompile /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
# Read the original: this source's Name= targets a mirror whose store contains 0.
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -Apply
pwsh -File $dfx build Saved/pull-probe/NS_Effects1_Mesh.dfs
pwsh -File $dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
```

### Case 1 results

```text
$ dfx decompile <original asset> -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
  106: MeshYaw = 0.0                     <- pin value; the store contains -90
  Export SHA256 = 3E3A1DC4...

$ dfx pull <source> -Asset <original asset>          # dry run
  NS_Effects1_Mesh.dfs(106,27): info DFX7116: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  pull: 1 literal(s) would be written and nothing was: this is a dry run.
  === DreamFX pull: 14 value(s) compared, 1 to write, 5 not declared, 1 not writable, 0 stack(s) not addressed | dry run ===
  SHA256 unchanged

$ dfx pull <source> -Asset <original asset> -Apply
  pull: wrote 1 literal(s) into '...NS_Effects1_Mesh.dfs'. The file it was is at
        'C:/CrossingVoid/Saved/DreamFX/Pull/Saved/pull-probe/NS_Effects1_Mesh.dfs'.
  NS_Effects1_Mesh.dfs(106,27): info DFX7116: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  === DreamFX pull: 14 value(s) compared, 1 written, 5 not declared, 1 not writable, 0 stack(s) not addressed | applied ===

  Whole-file Compare-Object (backup vs result): exactly one changed line
      => MeshYaw = -90.0
      <= MeshYaw = 0.0

$ dfx build <source>                       # source targets the mirror, so rebuild the mirror
  === DreamFX done: 1 built, 0 up to date, 0 failed | 0 error(s), 0 warning(s) ===
  No build safety messages: the source declares this input, so rule 3 allows it.

$ dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
  DIFF ... original 186 fact(s), mirror 186; 1 only-original, 1 only-mirror
  Only system FixedBounds differs (original +/-100, mirror +/-300); MeshYaw is 0000B4C2 on both sides.

$ dfx pull <source> -Asset <original asset> -Apply    # second run
  info DFX7107: ... already holds every value ... (14 compared, 5 not declared, 1 not writable). No bytes written.
  === DreamFX pull: 14 value(s) compared, 0 to write, ... | applied ===
  SHA256 and mtime unchanged (idempotent)
```

Interpretation:

- **Only one line changed.** The `-NoDefaults` export stayed at 123 lines; indentation, other
  literals, comments and line endings were preserved.
- **The build really rebuilt** (`1 built`, not `up to date`) and passed cleanly. This demonstrates
  `pull` cooperating with the gate: changing `0` to `-90` is allowed because the source declares it.
- **`asset-diff` no longer differs on `MeshYaw`**; both values are `0000B4C2`. The remaining
  `system FixedBounds` difference predates this test: the prior gate probe's
  `Saved/DreamFX/BuildSafety/NS_Effects1_Mesh.{before,after}.facts` already contained mirror bounds
  of ±300. Both sides had `bFixedBounds = False`, so the bounds were disabled, and the source never
  declared `FixedBounds`. Rebuilding did not change it, so the gate saw no change. This was neither
  introduced here nor `pull`'s responsibility; resolving it requires deciding whether source should
  declare the system's `FixedBounds`.
- **Idempotence**: the second `-Apply` reports `DFX7107` and writes no bytes.

The same file also produced a real refusal:

```text
SpawnBurst_Instantaneous.Age is declared but was not written back: the text writes it as a linked parameter, not as a literal, and changing the value MODE is an edit rather than a write-back
```

The source has `Age = Emitter.Age`, a link, while the store has a constant. `pull` reports it without
changing it (`DFX7111`).

### Case 2: `破空灰尘` — stored but undeclared values

The ordinary export, **without** `-NoDefaults`, omitted eight module-input facts present only in
the original (coverage sections 3.4 and 6.8, row 7): four `ScaleColor.Scale RGBA` facts (two emitters
times spawn/update scripts), plus four `EmitterState.Min/MaxDistance` facts.

Excerpt from `Saved/pull-probe/runs/c2-pull-apply.log`; the full report has one line per undeclared input:

```text
info DFX7106: pull: ScaleColor.ScaleRGBA is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MaxDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MinDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
...
info DFX7107: ... (19 compared, 94 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 19 value(s) compared, 0 to write, 94 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

- **Six report lines represent those eight facts.** The same name occurs in
  `ParticleSpawnScriptInterpolated` and `ParticleUpdateScript` with equal values, so `pull`
  deduplicates by name. Unequal duplicates are refused (section 6). These scripts serve
  `ParticleSpawn` and `ParticleUpdate`; each report line has its own `(line,column)`.
- `ScaleColor.ScaleRGBA` is the source identifier for `Scale RGBA`, produced by `ToInputIdentifier`.
- **Source SHA256 was unchanged** after both dry run and `-Apply`, demonstrating that no structure
  was added. Their summary differs only in `| dry run` versus `| applied`.
- Most of the 94 undeclared values are constants the engine materialized from defaults.
  Section 6.8 notes that only 8 of 37 examined entries were actually lost. The gate, not `pull`,
  determines loss.

### Case 3: already matching source (clean mirror of `/AtlasFX/Templates/NS_Atlas2D_Mesh`)

The source is the mirror's own export, with `Name=` targeting
`/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh`:

```text
info DFX7107: pull: '...NS_Atlas2D_Mesh.dfs' already holds every value
  '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' stores for the input(s) it declares
  (2 compared, 18 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 2 value(s) compared, 0 to write, 18 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

SHA256 was unchanged. All 18 undeclared entries were materialized defaults (`MeshSize`, `MinSize`,
`UniformScale`, `MeshYaw = 0`, etc.). This template's `MeshYaw` really is zero (coverage section 2.1),
and `pull` leaves those values alone.

### Case 4: gates

```text
$ dfx lint -All
  === DreamFX done: 0 built, 0 up to date, 0 failed | 0 error(s), 89 warning(s) ===
  dfx: OK (exit 0)                      # 89 pre-existing content lint warnings
$ dfx corpus
  dfx: corpus OK (55 passed)            # same baseline as the safety-gate pass
$ Build.bat CrossingVoidEditor Win64 Development -MaxParallelActions=2 -WaitMutex
  Result: Succeeded                     # only the existing DreamFXCorpusTest.cpp:572 C4996 remains
```

Raw logs: `Saved/pull-probe/runs/c4-lint.log` and `c4-corpus.log`.

## 6. Known limitations

1. **`pull` reads the store, not pins.** For inputs such as `MeshYaw`, these disagree (coverage
   section 3.5). `pull` follows the store, consistently with `asset-diff` and the gate. If no store
   entry exists, for example for an input that has always been linked, it does not invent a value.
2. **Duplicate names with different values are refused** (`DFX7111`). `pull` cannot determine
   which one the engine actually reads; guessing would author a value unsupported by the asset.
3. **Correspondence is mandatory.** Manually adding or removing a module rejects that stack in
   ordinary value mode. Align source and asset with `build` before pulling, or use the unambiguous
   structural path in section 4d.
4. **Node names come from the asset and need not be explicit in the source.** `as <name>` matters
   when the text must name an identity, such as `Output.<node>.<value>`. Engine names like `Foo_1`
   therefore need no extra declaration. Correspondence relies on the same stack, ordinal and
   module asset, exactly like `build`: statement order = insertion order = node order. A value
   pulled at ordinal `i` is applied at that same ordinal by the rebuild.
5. **`-NoDefaults` supplies keys, not authoritative values.** Pin-derived `MeshYaw` starts as
   `0.0`; `pull` replaces it with the store value. This measured export rebuilt with `1 built,
   0 error, 0 warning`, but that is a test result, not a promise for every asset. If such an export
   includes an input the writer cannot accept, that is a `decompile -NoDefaults`/writer issue.
   A normal export plus a manually added `<input> = <current value>` is also valid and smaller.
6. **After `-Apply`, the source's `Name=` still selects the build destination.** `-Asset` can read
   the original while `build` writes the mirror; it never means “write back to the original asset.”
7. **`GetEmitterInfo` appends to its output parameter.** Reusing an `FEmitterInfo` across emitters
   leaks the first emitter's stacks into the second. `pull` creates a fresh instance per emitter.
   Hardening that shared adapter was deliberately outside this implementation pass.
8. **Only the six fixed stacks are addressed.** Event/simulation-stage stacks require the separate
   focus-slice mechanism. Renderer properties, settings, assignments and `Defaults` are covered
   from the second pass (section 4b).
9. **Structural editing covers module calls only.** Set Parameters additions/removals are explicitly
   refused (section 4d). Splitting collapsed assignments is a different edit; incorrect boundaries
   can change which entries see each other, and there is no fixture supporting it yet.
10. **Renderer-count/type mismatches reject the emitter's renderer edits.** Different numbers of
    source `SpriteRenderer` blocks and asset renderers, or changed types at a position, produce
    `DFX7109`. `-Structure` does not insert renderer blocks.
11. **The positional baseline does not survive renderer reordering** (section 4c). Properties may
    be treated as dirty and written again after such an edit.
12. **The post-structure test guarantees no lost fact on rebuild, not identical recompiled
    renderers.** Corpus assertions distinguish: address still present but fact missing (failure);
    whole address removed (a source decision, also ignored by gate rule 1); and address gone but
    the same value survives at the same address after removing the script label (the rebuild
    merged duplicate constants). The third is reported but not failed, since no value was lost.
    The safety gate does not distinguish it: this is the only known difference in their criteria.

## 5b. Second-pass measurements (2026-10-09, `/Game/DreamFXProbe/NS_PullProbe`)

The probe used `DFX/DreamFXProbe/NS_PullProbe_T1.dfs` (three modules) and `_T2.dfs` (four, adding
`ScaleColor();`) with the same `Name=`. Build **T2** to simulate an editor-added module, then pull
into **T1**. Both probe sources and the probe asset were removed after verification.

| # | Command | Observed result |
|---|---|---|
| 1 | `pull T1`, dry run without `-Structure` | `warning DFX7113: pull: Motes.ParticleUpdate: the asset has ScaleColor that the text does not ... so -Structure writes that difference ... Nothing in this stack was addressed.`; `info DFX7107: ... (4 compared, 6 not declared, 0 not writable, 0 withheld). No bytes written.`; **T1 SHA256 unchanged** |
| 2 | `pull T1 -Structure -Apply` | `info DFX7112: pull: Motes.ParticleUpdate: +ScaleColor:             ScaleColor@1.1(ScaleRGB = (1.0, 1.0, 1.0), ScaleAlpha = 1.0);`; `=== DreamFX pull: 5 compared, 0 written, 12 not declared, 0 not writable, 0 withheld, 0 stack(s) not addressed, 1 line(s) added, 0 removed, 0 structure(s) refused \| applied \| structure \| baseline recorded ===`; 34 → 35 lines, **only that line in Compare-Object** |
| 3 | `build T1 -Force`, after structural editing | `=== DreamFX done: 1 built, 0 up to date, 0 failed \| 0 error(s), 1 warning(s) ===`, exit 0; **no build safety messages or DFX8018**, and no `Saved/DreamFX/BuildSafety/` directory (only created on gate findings). The sole warning is existing `DFX7102`, unbounded SpawnRate |
| 4 | `pull T1 -Apply`, already aligned | `info DFX7107: ... (7 compared, 12 not declared, 0 not writable, 0 withheld). No bytes written.`; **same SHA256 and mtime** (`32072A08…` / `16:39:41`), baseline recorded |
| 5 | Edit source `WarmupTime = 0.5` to `0.9`, then `pull T1 -Apply` | `info DFX7115: pull: Settings.WarmupTime differs from this text but the asset has not moved since the last apply, so the text's value stands.`; `(7 compared, 12 not declared, 0 not writable, 1 withheld)`; **unchanged SHA256**, still `WarmupTime = 0.9;` |
| 6 | `lint -All` | `0 error(s), 91 warning(s)`, exit 0; 89 existing tree warnings plus two from the probe files |
| 7 | `corpus` | `dfx: corpus OK (64 passed)`, up from 55 with nine new `DreamFX.Corpus.WriteBack` cases |

The nine cases in `Tests/Corpus/WriteBack/` cover rows 1–5, plus **byte-identical no-op write-back**
(`NoOp`, one value of each of four kinds; compare actual bytes, not just an edit count of zero);
**exactly one changed line** for `ModuleInput`, `RendererProperty`, `SystemSetting` and `Assignment`,
followed by a second pull with zero edits; and no lost facts after rebuilding in place in the three
structural cases.

## 7. Reproduction

```powershell
# 0) Ensure no UnrealEditor is running; each command starts one editor process.
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# All four cases; logs go to Saved/pull-probe/runs/.
pwsh -File C:\CrossingVoid\Saved\pull-probe\run-pull-cases.ps1

# Gates
pwsh -File $dfx lint -All -Engine $eng
pwsh -File $dfx corpus -Engine $eng
& 'D:\UnrealEngine-5.8.2\Engine\Build\BatchFiles\Build.bat' CrossingVoidEditor Win64 Development `
    -Project='C:\CrossingVoid\CrossingVoid.uproject' -MaxParallelActions=2 -WaitMutex
```
