---
title: Export vs Adopt
description: Both decompile; they differ in what happens next — the mirror namespace, the adoption sequence, and the three reasons it refuses.
---

**Export** and **Adopt** both decompile. They differ in what happens next, and that difference is the
whole point.

| | **Export .dfs** | **Adopt** |
| :-- | :-- | :-- |
| Writes to | `DecompiledOutputDirectory` (default `DFX/Decompiled`) | the real source root — `<Project>/DFX/…` or `<Plugin>/DFX/…` |
| `Name=` names | `Decompiled/<original directory>/<asset>` — a **mirror** | the original asset |
| Picked up by the build | yes — saving one rebuilds its mirror | yes |
| Modifies the asset | **never** | yes: rebuilds it from the new source and stamps provenance |
| Unrepresentable features | warns, and lists them in the file | **refuses** |
| Meaning | "let me read and play with this as text" | "from now on this text is the only truth" |

## What keeps them apart is structural, not procedural

An export **cannot name** the asset it came from:

```
/Game/FX/NS_X
   └─ export ─> DFX/Decompiled/Game/FX/NS_X.dfs
                Name="Decompiled/FX/NS_X"
                   └─ build ─> /Game/Decompiled/FX/NS_X      (the original is untouched)
```

So the whole decompiled tree is **ordinary source** — watched, built on save, linted and CI'd — and
the original stays untouched however the file is edited.

Exports written before this arrangement still name the original. They are **refused** with
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) rather than obeyed; re-exporting replaces them.

Exporting an asset that is *already* a mirror is refused too — it would leave two sources claiming
one asset — and the toast links to the source the mirror was built from.

## Adopt, in six steps


### Decompile
**Refuse** if anything is unrepresentable, listing what
([DFX8010](../diagnostics/DFX8xxx.md#dfx8010)).

### Work out the source path from the mount point
`/Game/FX/NS_X` → `<Project>/DFX/FX/NS_X.dfs`; `/MoonToon/FX/NS_X` → `<MoonToon>/DFX/FX/NS_X.dfs`.

### Check nothing else already declares this asset
If something does → **refuse** ([DFX8011](../diagnostics/DFX8xxx.md#dfx8011)). Two sources would
overwrite each other on alternate builds.

### Confirm
Listing the file to write and the asset to rebuild. (From Python, `bSkipConfirmation` suppresses the
modal.)

### Write the file, rebuild the asset, stamp provenance

### Re-decompile and compare byte for byte
A mismatch is [DFX8012](../diagnostics/DFX8xxx.md#dfx8012), and the first differing line is logged.


Step 1 is what makes step 6 meaningful: with no known gaps, a mismatch is a **real defect** rather
than an expected loss.

> **Note:**
>
> `DFX8012` leaves the source file in place — the asset was rebuilt from it, so the text is
> authoritative either way — and logs the first differing line for you to judge.

## Which one to use

| Goal | Use |
| --- | --- |
| Understand an effect someone else built | **Export** |
| Use an existing effect as a template for a new one | **Export**, then change `Name=` into your own namespace |
| **Migrate** an effect onto the text workflow | **Adopt** |
| Sweep a whole tree to see how much is covered | `dfx decompile-all` + `mirror-diff` |

> **Warning:**
>
> After an adopt, that asset is build output: hand edits are wiped on the next rebuild. That is
> exactly what you asked for by adopting — just make sure the team knows.

## The same thing from the command line

```bash
# print it
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion

# write it to a file
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion -Out DFX/Decompiled/NS_Explosion.dfs

# a whole tree
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
```

`-NoDefaults` prints every input, including ones equal to a pristine module's. That is **diagnostic
only** — the result is not meant to be maintained; it answers "what did the baseline hide?".
