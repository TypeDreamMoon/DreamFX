---
title: Layout and naming
description: Where DreamFX sources live in the DreamSource layout, migrating from the deprecated DFX/ directory, how Root and Name resolve into an asset path, module search paths, and what belongs in version control.
---

## Source roots

A source root is a directory DreamFX reads `.dfs` / `.dfe` / `.dfm` files from. The project has one,
and every plugin may have its own. Since the [DreamSource layout](#the-dreamsource-layout) they sit
under `DreamSource/FX/`:

```text
DevTest/
  Config/
    DefaultDreamSource.ini          the project's source directory (Project Settings ▸ Dream Source ▸ FX)
  DreamSource/
    FX/                             the project's source root
      Effects/
        NS_Hello.dfs
      Emitters/
        E_MoonFlashCard.dfe
      Modules/
        ToonSpin.dfm
      Decompiled/
      DreamFX.code-workspace
      .dreamfx/
        index.db                    the module index; not in version control
  Intermediate/
    DreamSource/
      fx.db                         the bridge's last diagnostics
      FX/
        Bridge/                     Requests/, Responses/, status.json
  Plugins/
    MoonToon/
      DreamSource/
        FX/                         a plugin's source root
```

Every root is discovered, watched by the file watcher, and swept up by `build -All`. Enabling one
more plugin adds one more root; nothing has to be registered.

A project or plugin that still keeps its sources in the old `DFX/` directory keeps working: see
[Which directory is read](#which-directory-is-read).

## The DreamSource layout

Every Dream plugin with text sources keeps them under one `DreamSource/` directory, one subdirectory
per language, and its generated data under `Intermediate/DreamSource/`. For DreamFX that is
`DreamSource/FX/`, which replaces the project-root `DFX/` directory.

### The setting

*Project Settings ▸ Dream Source ▸ FX ▸ Source Directory*, stored in `Config/DefaultDreamSource.ini`:

```ini
[/Script/DreamFXEditor.DreamFXSourceSettings]
SourceDirectory=(Path="DreamSource/FX")
```

| | |
| :-- | :-- |
| Default | `DreamSource/FX` |
| Relative to | the project directory. An empty or absolute value is ignored and the default used, so every checkout and the [VSCode extension](../tools/vscode.md) resolve the same directory |
| Takes effect | after an editor restart: the source watcher is set up on the directory at startup |
| Plugins | not configurable: a plugin's sources are always in `<Plugin>/DreamSource/FX` |

The *Decompiled Output Directory* (*Project Settings ▸ Plugins ▸ DreamFX*, `Config/DefaultEditor.ini`)
follows the source directory: empty, its default, means `<source directory>/Decompiled`. The old
default `DFX/Decompiled` is read the same way as empty. Any other value is used as written, relative
to the project directory.

### Which directory is read

For the project, `C` is the configured directory and `L` is `<Project>/DFX`:

| `C` exists | `L` exists | Read | Message |
| :-- | :-- | :-- | :-- |
| yes | no | `C` | — |
| yes | yes | `C`; `L` is **ignored** | Output Log warning, and an editor notice: merge what `L` still holds into `C`, then delete it |
| no | yes | `L`, deprecated | Output Log warning, and an editor notice with a **Migrate** button ([below](#the-migration-notice)) |
| no | no | `C` | — ; whatever writes the first file there (Adopt, *Open DreamFX Workspace*, `dfx index`) creates it |

The legacy `DFX/` is never removed from this rule: it keeps working for good, and the warning only
says it is deprecated. A `C` configured as `DFX` is not legacy: it is what was asked for.

The answer is fixed for the editor session. A directory that appears, disappears or is moved while
the editor runs — a migration included — is picked up at the next start. Each commandlet run is a
process of its own and resolves it afresh.

A plugin reads `<Plugin>/DreamSource/FX` when it exists, else `<Plugin>/DFX`. The plugin's `DFX/` is
deprecated too, but it gets only the Output Log warning: there is no notice and no Migrate button for
a plugin.

The warnings are written once per process as `LogDreamFX: Warning`, in the editor and in every
commandlet run (`dfx.ps1` included).

A `from "DFX/Emitters/E_Flash"` written before the move still resolves after it: a leading `DFX/`, or
the configured directory (`DreamSource/FX/`), is dropped before the path is tried against each source
root. Moving the sources never edits them, and `Name=` / `Root=` are relative to the root, so every
generated asset path stays the same.

### Old and new paths

| Before | Now |
| :-- | :-- |
| `<Project>/DFX/` | `<Project>/DreamSource/FX/` (the *Source Directory* setting) |
| `<Plugin>/DFX/` | `<Plugin>/DreamSource/FX/` |
| `DFX/Decompiled/` | `<source directory>/Decompiled/` (*Decompiled Output Directory* empty) |
| `DFX/DreamFX.code-workspace`, `DFX/.vscode/` | `<source directory>/DreamFX.code-workspace`, `<source directory>/.vscode/` |
| `DFX/.dfx-index.json`, `.dfx-index.json.inflight`, `.dfx-index.json.quarantine` | `<source directory>/.dreamfx/index.db` |
| `Saved/DreamFX/Bridge/` (`Requests/`, `Responses/`, `status.json`) | `Intermediate/DreamSource/FX/Bridge/`; the protocol is unchanged |
| `Saved/DreamFX/Bridge/diagnostics.json` | `Intermediate/DreamSource/fx.db`, table `diagnostics` |
| `Saved/DreamFX/{Pull,BuildSafety,WriteBack,DynamicMesh}/`, `Saved/DreamFX/*.facts`, `Saved/DreamFX/l3-report.md` | unchanged |

`<source directory>` is whichever directory [the rule above](#which-directory-is-read) picked, so a
project still on `DFX/` keeps its index in `DFX/.dreamfx/index.db`. The two databases are described in
[VSCode and the module index](../tools/vscode.md#the-module-index) and
[Editor integration](../tools/editor-integration.md#the-bridge).

### The migration notice

When the project's sources are still in `DFX/`, the editor shows a notice at startup with three
buttons:

| Button | Effect |
| :-- | :-- |
| **Migrate** | Moves the folder and offers a restart, below |
| **Don't show again** | Stops the notice on this machine: `bHideFXMigrationNotice=True` under `[DreamSource]` in the per-user `EditorPerProjectUserSettings.ini` under `Saved/Config/`. The Output Log still says it |
| **Later** | Closes the notice until the next start |

**Migrate** refuses when the configured directory already exists — it never merges. Otherwise it:

1. Moves `DFX/` to the configured directory, whole: a `.git` inside it (a project that keeps its
   effects in their own repository) goes along. When the rename is refused it copies, then deletes
   the old folder; a folder that cannot be deleted is left in place with a warning, and is then
   ignored.
2. Imports the old `.dfx-index.json`, `.inflight` and `.quarantine` files into `.dreamfx/index.db`,
   then deletes them. If that fails, nothing is lost: the files stay, and the next `dfx index` imports
   them.
3. Rewrites the relative folder paths in `DreamFX.code-workspace` so they point where they did from
   one level deeper.
4. Deletes `Saved/DreamFX/Bridge/`.
5. Offers **Restart Now** / **Later**. The running editor keeps reading the old place until it restarts.

Close VSCode windows that have files in `DFX/` open before you press it. The notice appears only in
an interactive editor: never in a commandlet, an automation run, an unattended session or under
`-NoDreamFXEditor`, which only log the warning. When `DreamSource/FX/` and `DFX/` both exist the
notice has no Migrate button; it asks you to merge by hand.

The [VSCode extension](../tools/vscode.md#the-dreamsource-layout) offers the same move as the command
*DreamFX: Migrate to DreamSource*.

### Migrating by hand

1. Close the editor.
2. Move the folder: `DFX/` → `DreamSource/FX/` (or wherever *Source Directory* points). Keep it whole,
   `.git` included. With git: `git mv DFX DreamSource/FX`.
3. Add `.dreamfx/` to `.gitignore`.
4. Delete `Saved/DreamFX/Bridge/`.
5. Start the editor. Old `.dfx-index.json*` files that moved with the folder are imported into
   `.dreamfx/index.db` and deleted by the next `dfx.ps1 index` (or the bridge's `refreshIndex`). A
   `DreamFX.code-workspace` is rewritten by the next *Open DreamFX Workspace*.

A plugin's sources move the same way: `<Plugin>/DFX/` → `<Plugin>/DreamSource/FX/`.

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
> (`DreamSource/FX/Effects/NS_Spark.dfs` declaring `Name="Effects/NS_Spark"`), but the language does
> not require it — the one place it does is the `Decompiled/` namespace, below.

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

Relative to the **referencing file** first, then against every source root in turn. A leading
`DFX/`, or the configured source directory (`DreamSource/FX/`), is dropped first, so a path written
before the [DreamSource layout](#the-dreamsource-layout) still resolves. The extension is optional.

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

*Export .dfs* / *Export .dfe* write into the **Decompiled Output Directory**: by default `Decompiled/`
inside the project's source directory (`DreamSource/FX/Decompiled`), changed in *Project Settings ▸
Plugins ▸ DreamFX*.

Files there are **first-class source**: watched, built, linted, CI'd. One rule applies to them and to
nothing else — they must name into the `Decompiled/` namespace:

```
/Game/FX/NS_X  ──export──>  DreamSource/FX/Decompiled/Game/FX/NS_X.dfs
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
| `DreamSource/FX/DreamFX.code-workspace` | *Open DreamFX Workspace* | A VSCode workspace with one folder per source root. **Fully rewritten every time, never merged** — keep per-user settings in `DreamSource/FX/.vscode/` |
| `DreamSource/FX/.dreamfx/index.db` | `dfx.ps1 index` | Every module and dynamic input on the search paths, with its stacks, category, description and **input signature**; the editor extension reads it for completion |

Both sit in the project's source directory, whichever [the rule](#which-directory-is-read) picked.

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

The one directory that should stay out is `.dreamfx/` in the source directory: `index.db` records
the engine path and the enabled plugin list, so it is wrong on the next machine. Add `.dreamfx/` to
`.gitignore`. `Intermediate/` and `Saved/` are outside version control already.
