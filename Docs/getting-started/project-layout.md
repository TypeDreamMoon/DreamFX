---
title: Layout and naming
description: DFX source roots, how Root and Name resolve into an asset path, module search paths, and what belongs in version control.
---

## Source roots

A `DFX/` directory is a source root. The project has one, and every plugin may have its own:

```text
DevTest/
  DFX/
    Effects/
      NS_Hello.dfs
    Emitters/
      E_MoonFlashCard.dfe
    Modules/
      ToonSpin.dfm
    Decompiled/
    DreamFX.code-workspace
    .dfx-index.json
  Plugins/
    MoonToon/
      DFX/
```

Every root is discovered, watched by the file watcher, and swept up by `build -All`. Enabling one
more plugin adds one more root; nothing has to be registered.

## `Root` and `Name`

Two header arguments decide where the asset lands:

```cpp
System(Name="Effects/NS_Spark", Root="Game")     // -> /Game/Effects/NS_Spark
Emitter(Name="Emitters/E_Card", Root="Plugin.MoonToon")  // -> /MoonToon/Emitters/E_Card
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
```

| `Root` | Mount point |
| --- | --- |
| `"Game"` or `""` | `/Game` |
| `"Plugin.<PluginName>"` | that plugin's content root, e.g. `/MoonToon` |

A `Root` that names no mounted content root, or a `Name` with nothing after its last slash, is
[DFX3000](../diagnostics/DFX3xxx.md#dfx3000).

> **Note:**
>
> **Where the file sits is not where the asset goes.** The directory only decides which source root
> owns the file; the asset path comes from `Root` + `Name`. Keeping them parallel is the convention
> (`DFX/Effects/NS_Spark.dfs` declaring `Name="Effects/NS_Spark"`), but the language does not require
> it — the one place it does is the `Decompiled/` namespace, below.

## The extension has to match the declaration

| File | May declare |
| --- | --- |
| `.dfs` | `System` |
| `.dfe` | `Emitter` |
| `.dfm` | `Module` or `DynamicInput` |

A mismatch is [DFX2021](../diagnostics/DFX2xxx.md#dfx2021), and the message names the filename to
rename to. One file holds exactly one top-level object; anything after it is
[DFX2022](../diagnostics/DFX2xxx.md#dfx2022).

## How a `from` path resolves

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard" { ... }
```

Relative to the **referencing file** first, then against every DFX root in turn. The extension is
optional.

## Module search paths

Short module names (`GravityForce`) resolve on the search paths. The engine's three defaults are
always on the list:

```
/Niagara/Modules      /Niagara/DynamicInputs      /Niagara/Functions
```

Your own module folders are **added** (not substituted) through `ModulePaths` in the system's
`Settings`:

```cpp
Settings = {
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

When a short name is ambiguous, write a longer path — a partial one such as
`Spawn/Initialization/V2/InitializeParticle` is enough, the full asset path is rarely needed. Nothing
found is [DFX3001](../diagnostics/DFX3xxx.md#dfx3001); found but with no such input is
[DFX3003](../diagnostics/DFX3xxx.md#dfx3003) — the two typos are reported apart, because otherwise
they read identically.

## The decompiled output directory

*Export .dfs* / *Export .dfe* write into the **Decompiled Output Directory** (default
`DFX/Decompiled`, changed in *Project Settings ▸ Plugins ▸ DreamFX*).

Files there are **first-class source**: watched, built, linted, CI'd. One rule applies to them and to
nothing else — they must name into the `Decompiled/` namespace:

```
/Game/FX/NS_X  ──export──>  DFX/Decompiled/Game/FX/NS_X.dfs
                            Name="Decompiled/FX/NS_X"
                            ──build──>  /Game/Decompiled/FX/NS_X
```

So an export **structurally cannot touch the original**, however it is edited. Older exports that
predate the arrangement are refused with
[DFX8013](../diagnostics/DFX8xxx.md#dfx8013) rather than obeyed; re-exporting replaces them. The full
story is in [Export vs Adopt](../generation/adopt.md).

## Two generated files

| File | Written by | Purpose |
| --- | --- | --- |
| `DFX/DreamFX.code-workspace` | *Open DreamFX Workspace* | A VSCode workspace with one folder per source root. **Fully rewritten every time, never merged** — keep per-user settings in `DFX/.vscode/` |
| `DFX/.dfx-index.json` | `dfx.ps1 index` | Every module and dynamic input on the search paths, with its stacks, category, description and **input signature**; the editor extension reads it for completion |

The index is a command rather than something automatic: it boots the engine, and measured here it
costs 8.1s for 571 modules with input probing (2.9s without). See
[VSCode and the module index](../tools/vscode.md).

## What belongs in version control

**Every `.dfs`, `.dfe` and `.dfm`.** They are the source.

The generated `.uasset` files are a team decision, and both answers work:

- **Do not commit them** — whoever needs them runs `build -All`. The cleanest tree, but everyone has
  to be able to generate.
- **Commit them** — usable on checkout, and `verify` inside `ci.ps1` watches for a source that was
  edited without a rebuild. A team on an engine that cannot generate `.dfm` graphs **must** commit
  the module assets, or consumers have no script at all
  ([DFX5100](../diagnostics/DFX5xxx.md#dfx5100) / [DFX5107](../diagnostics/DFX5xxx.md#dfx5107)).

The one file that should stay out is `DFX/.dfx-index.json`: it records the engine path and the
enabled plugin list, so it is wrong on the next machine.
