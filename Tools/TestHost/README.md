# DreamFX test host

A minimal Unreal project that exists only to build DreamFX and run its gate, away from any project you
actually work in.

## Why

The gate (`.skill/ci.ps1`: lint, build, verify, corpus) used to run inside the working project (DevTest).
That had three costs:

- **The working project's editor had to be closed.** Building links the plugin's DLLs, which a running
  editor holds open; the build and corpus steps write `.uasset` files, and an editor open on the same
  project rebuilds and saves the same packages from its source watcher -- whichever saves second wins, and
  two processes saving one package at once has crashed the editor before.
- **Other sessions share the working tree.** Anything else committing to or editing the same checkout
  changes what a run is testing, half-way through.
- **"The project builds" is not "the plugin builds".** A project that enables thirty other plugins can
  hide a dependency DreamFX forgot to declare.

The host fixes all three. Its `Plugins/DreamFX` is a separate **git worktree** of the DreamFX repository,
with its own `Binaries/`, `Intermediate/` and generated `Content/`; it enables nothing but DreamFX (and
Niagara, which DreamFX declares); and it has no code or content of its own. The only DreamFX sources it
sees are the plugin's own `DFX/` tree and `Tests/Corpus`.

It is also the place to try a pull request: check the PR's head out in the host's worktree, build, run the
gate and open the host editor -- the working project never sees it.

## Layout

```
DreamFXTestHost/                       (default I:\UnrealProject_Moon\DEV_58\DreamFXTestHost)
  DreamFXTestHost.uproject             from Template/
  Config/DefaultEngine.ini             from Template/
  Config/DefaultGame.ini               from Template/
  Source/DreamFXTestHost*.Target.cs    from Template/
  Source/DreamFXTestHost/              from Template/ -- an empty primary game module
  Plugins/DreamFX/                     a git worktree of the DreamFX repository
  .dreamfx-testhost.json               what the host was made from (template version, repo, branch)
  Binaries/ Intermediate/ Saved/       created by the first build and run
```

The template is not under the plugin's `Source/` or `Tests/` folder, the only places UnrealBuildTool looks
for module and target rules inside a plugin, so no project that has DreamFX in its `Plugins/` picks the
template's `.Build.cs` or `.Target.cs` files up.

## Creating it

```powershell
pwsh -NoProfile -File Tools\TestHost\New-DreamFXTestHost.ps1 -Detach -WhatIf   # see what it would do
pwsh -NoProfile -File Tools\TestHost\New-DreamFXTestHost.ps1 -Detach
```

`-Detach` is the usual case: `main` is checked out in `DevTest\Plugins\DreamFX`, and a branch can be checked
out in one worktree at a time, so the host follows `main` with a detached HEAD. A clean detached worktree is
moved to the branch's tip on every run of the script; a dirty one is left where it is.

| Parameter | Default | |
| --- | --- | --- |
| `-Root` | `I:\UnrealProject_Moon\DEV_58\DreamFXTestHost` | The host directory. Refused on drive C unless `-AllowSystemDrive`. |
| `-RepoPath` | `I:\UnrealProject_Moon\DEV_58\DevTest\Plugins\DreamFX` | Any working tree of the repository. |
| `-Branch` | `main` | What the worktree must have (or is created with). |
| `-EngineRoot` | looked up from the `.uproject`'s engine association | |
| `-Force` | off | Replace files that differ from the template; each old one is kept as `<name>.bak-<timestamp>`. |
| `-KeepCurrentHead` | off | Accept an existing worktree on another branch or a detached commit (a PR head, a revert experiment). |
| `-Detach` | off | Follow `-Branch` with a detached HEAD. |
| `-AllowSystemDrive` | off | Accept `-Root` on drive C (or set `DREAMFX_ALLOW_DRIVE_C=1`). |
| `-WhatIf` | off | Report only. |

It is safe to run again at any time. Exit codes: `0` the host matches the template; `1` a check failed;
`2` the host is usable but does not match the template (files kept without `-Force`, or `-WhatIf` found work
to do). The script is the DreamGUI host's script with the names changed; see DreamGUI's
`Tools/TestHost/README.md` for the details of each check.

## Building and running

```powershell
$Engine = 'F:\UnrealEngine\UE_Moon'
$Proj   = 'I:\UnrealProject_Moon\DEV_58\DreamFXTestHost\DreamFXTestHost.uproject'

& "$Engine\Engine\Build\BatchFiles\Build.bat" DreamFXTestHostEditor Win64 Development "-Project=$Proj" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges
pwsh -NoProfile -File I:\UnrealProject_Moon\DEV_58\DreamFXTestHost\Plugins\DreamFX\.skill\ci.ps1 -Project $Proj
```

Every `.skill/dfx.ps1` command takes `-Project` the same way (and finds the host by itself when it is run
from inside the host's directory). `ci.ps1` checks diagnostic-document drift and the driver regressions,
then runs lint, build, verify and `dfx.ps1 corpus DreamFX`: every DreamFX automation suite, including
language, workspace and engine probes. Use `dfx.ps1 corpus` explicitly for only the corpus subset.

The driver and L3 capture regressions also run without launching Unreal:

```powershell
pwsh -NoProfile -File Tests/Tools/Test-Driver.ps1
python -B Tests/Tools/test_l3_equivalence.py
```

`-CleanNew` preserves all assets that existed before the command, including ignored or untracked files.
Each commandlet run writes a unique `Saved/Logs/DreamFX-<id>.log`; an earlier project's log cannot supply
its exit verdict. `ci.ps1 -SkipBuild -SkipCorpus` performs the checks that do not write packages.

The editor target uses the **shared** build environment: engine modules already built in
`Engine/Binaries/Win64` are linked against as they are, and only the host module and the plugin are compiled
-- about 40 seconds on this machine. `-NoEngineChanges` makes the build stop, instead of rebuilding engine
modules, if the engine is out of date; build the engine first in that case. Never build the game target
(`DreamFXTestHost`): against a source engine it is monolithic and compiles the whole engine into the host.

## Trying a pull request

```powershell
$Repo = 'I:\UnrealProject_Moon\DEV_58\DevTest\Plugins\DreamFX'
$Wt   = 'I:\UnrealProject_Moon\DEV_58\DreamFXTestHost\Plugins\DreamFX'

git -C $Repo fetch origin pull/<N>/head:refs/remotes/pr/<N>
git -C $Wt switch --detach pr/<N>
# build, run the gate, open the host editor
git -C $Wt switch --detach main            # back; or rerun New-DreamFXTestHost.ps1 -Detach
```

While the worktree is on anything but the tip of `main`, `New-DreamFXTestHost.ps1 -Detach` moves it back
when it is clean -- pass `-KeepCurrentHead` instead to keep it where it is.

## The derived data cache

The host has no derived-data-cache configuration, on purpose, and shares the machine-wide local Zen store
the working project already warmed. A cache hit needs the same cache key, and shader keys include the
project's renderer settings and targeted shader formats, which is why `Config/DefaultEngine.ini` copies
those sections from DevTest verbatim. Change one of them and the first launch compiles every shader again.

## How the host differs from DevTest

| | DevTest | Host |
| --- | --- | --- |
| Plugins | DreamFX plus about thirty others | DreamFX (and Niagara through it) |
| DFX roots | the project's own `DFX/` (its own repository) and every plugin's | the plugin's `DFX/` only |
| Content | the project's own | none; DreamFX writes its samples, modules and corpus assets into the worktree's `Content/` (gitignored) |
| Startup map | a full showcase level | `/Engine/Maps/Entry` |
| MoonToon ramp atlases | loaded from the MoonToon project plugin | `None` |

Things that follow from this:

- **Only what is checked out in the worktree is tested.** Uncommitted work in `DevTest\Plugins\DreamFX`
  is not in the host until it is committed and checked out there (or copied over by hand).
- **The project's `.dfs` tree is not built here.** A change that only shows on the project's 49 sources
  (the long `NS_LevelUp_*` systems, the marketplace packs) still has to be measured in DevTest.
- **GPU shaders are not compiled headlessly**: a `-run=` commandlet always has a Null RHI, so the gate
  covers the translation half of a GPU emitter only. Confirm GPU effects in the host editor.
