---
name: dream-fx-verify
description: Build or check DreamFXLang sources headlessly — one .dfs file or the whole DFX tree — turning text into Niagara system assets without opening the Unreal editor. Use when asked to verify, validate, build, compile, test, or CI-gate .dfs / .dfe / .dfm / DreamFX sources, or to check whether a Niagara effect still generates from its source.
---

# dream-fx-verify `<file>` | `-All`

The DreamFX compile gate. Wraps `UnrealEditor-Cmd.exe -run=DreamFX` so a check is one command and
one exit code. This is the harness the other DreamFX skills call.

Paths below are relative to the plugin root, `Plugins/DreamFX/`.

## Run it

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Samples/NS_DreamFXPhase1.dfs -Force
```

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build -All -Force
```

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 verify -All
```

Exit `0` when the run produced no errors; otherwise the exit code **is the error count**. Run it
from anywhere inside the project.

Editor boot dominates the cost — roughly 12 s of the wall time regardless of how many files are
built. Batch edits rather than looping per file.

Close the editor before a build. Both processes write the same packages otherwise, and the one that
saves second wins silently; the run warns when it sees an editor but cannot tell which project that
editor has open. Large sources are slow for a structural reason, not a fixable one —
[Docs/performance-2026-08-08.md](../../Docs/performance-2026-08-08.md) has the measurements.

| Argument | Effect |
| :-- | :-- |
| *(positional)* | the source file — absolute, or relative to the working directory |
| `-All` | every source under every DFX root |
| `-Force` | bypass the provenance-hash skip. Without it an unchanged file reports `up to date` and proves nothing |
| `-NoSave` | build in memory without writing packages — the right flag for checking that source is valid |
| `-CleanNew` | delete the `.uasset` files this run wrote, **but only those git reports untracked**, then report the rest |
| `-NoWriteScope` | build the slow way, rebuilding the edit context per write. Diagnostic only — it exists so a benchmark can measure both halves on one binary |
| `-Project` | the `.uproject`. Defaults to the nearest one at or above the target, then the working directory |
| `-Engine` | engine root. Defaults to the `EngineAssociation` lookup; `UE_ENGINE_ROOT` also works |
| `-Raw` | print the whole engine log instead of just the `LogDreamFX` lines |

## Commands

| Command | Purpose |
| :-- | :-- |
| `build` | generate assets from source |
| `verify` | check assets against source, writing nothing — the drift gate |
| `lint` | static checks only; no asset access, so it is the fastest failure |
| `decompile <asset>` | export an existing system back to source — see [`dream-fx-decompile`](../dream-fx-decompile/SKILL.md) |
| `coverage` | how much of a project's VFX DreamFX can represent |
| `graph` | what each source file depends on |
| `rename <asset>:<old>:<new>` | rename an emitter safely — see below |
| `schema <Module>` | print a module's real input signature; `-Stack` picks the stack |
| `list` | every module the search paths expose; `-DynamicInputs` for dynamic inputs |

## The CI gate

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

Runs lint, then build, then verify, and stops at the first failure with that step's exit code.
`-SkipBuild` checks without writing; `-CleanNew` removes assets the build newly created.

The verify step is the one worth having. It catches the case nobody notices: someone edited a `.dfs`,
did not rebuild, and committed both. Build alone passes, because build fixes it.

## Renaming an emitter

An emitter name is a **stable key, not a display name** — Niagara stores literal module inputs under
an emitter-prefixed alias. Renaming in source and rebuilding would drop the old emitter and add a new
one, taking its handle GUID with it, which breaks anything referencing the emitter by id.

Rename the asset first, then the source:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 rename /Game/FX/NS_Spark:Sparks:Embers
```

Then change the name in the `.dfs` and rebuild. The rebuild matches by the new name and reuses the
existing handle.

## What you get back

```text
dfx: build  project=DevTest.uproject  engine=F:\UnrealEngine\UE_Moon
=== DreamFX build: 1 source file(s) ===
=== DreamFX done: 1 built, 0 up to date, 0 failed | 0 error(s), 0 warning(s) ===

Assets written to disk by this run:
  Plugins/DreamFX/Content/Samples/NS_DreamFXPhase1.uasset  [NEW (untracked)]

dfx: OK (exit 0)
```

A failure names the file, line and column:

```text
…/BadInputName.bad.dfs(11,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
dfx: FAILED (exit 1)
```

## The thing to understand about this command

**Niagara input names contain spaces; DreamFX identifiers cannot.** The real input is
`Loop Duration`, and you write `LoopDuration`. Matching is done on a normalised form — lowercase,
spaces and underscores removed — so `loop_duration` also resolves. Inline checkbox inputs live in
the `Module.` namespace (`Module.WriteLifetime`); the prefix is implied inside a module call, so
write `WriteLifetime`.

**`schema` is how you discover those names.** Do not guess them, and do not read them off the plan
document — modules get revised.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema InitializeParticle
```

Ambiguous short names are refused with the full candidate list rather than resolved arbitrarily:

```text
'InitializeParticle' is ambiguous -- 2 modules match. Write the full path instead:
/Niagara/Modules/Spawn/Initialization/InitializeParticle.InitializeParticle,
/Niagara/Modules/Spawn/Initialization/V2/InitializeParticle.InitializeParticle
```

A partial path disambiguates and is anchored at the end, so
`Spawn/Initialization/InitializeParticle` selects exactly that asset and not its `V2/` sibling.

## Gotchas

- **A failed build leaves the previous asset untouched.** Parsing, module resolution, input-name
  checks and value lowering all complete against an in-memory plan before anything is mutated. This
  is deliberate and differs from DreamShader, whose regeneration notes record the opposite ordering
  as a defect.
- **Stack issues are not reported headlessly.** Reading them needs a Slate-backed view model, which
  does not exist in a commandlet — asking for them there crashes the process, so the driver does
  not. A headless run reports Niagara *compile events* instead, which is what a CI gate needs.
  Open the asset in the editor to see the rest.
- **An input gated by a static switch cannot be written until the switch is set.** Source order is
  write order, so put `WriteSpriteSize = true` before `SpriteSize = …`. Writing the gated input
  first fails with `DFX5021` and a message about the EditCondition.
- **Undeclared system stacks keep whatever they already hold.** Declaring `SystemUpdate = { }` is
  how you take ownership of that stack; leaving it out preserves the engine's default `SystemState`
  module, and the run says so with a `DFX5003` info line.
- **`-All` on an empty source tree exits `0`**, logging a warning that no sources were found. A
  green run does not prove anything was built — check the `built` count in the summary line.
- **Emitter names are stable keys, not display names.** Renaming an emitter in source orphans every
  literal input on it, because Niagara stores those under an emitter-prefixed alias. The build
  refuses a name collision rather than silently uniquifying.

## Diagnostic codes

| Range | Meaning |
| :-- | :-- |
| `DFX1xxx` | lexical — unterminated string, bad character |
| `DFX2xxx` | syntax |
| `DFX3xxx` | name / asset resolution — unknown module, unknown input, ambiguous name |
| `DFX4xxx` | type checking — wrong type, wrong component count, narrowing conversion |
| `DFX5xxx` | lowering and asset generation |
| `DFX6xxx` | Niagara's own compile events, mapped back onto source |
| `DFX7xxx` | drift verification (`verify`) and lint |

## Troubleshooting

| Symptom | Fix |
| :-- | :-- |
| `EngineAssociation '{…}' is not registered.` | pass `-Engine <root>` or set `UE_ENGINE_ROOT` |
| `Could not find a .uproject at or above …` | pass `-Project` |
| `UnrealEditor-Cmd.exe not found at …` | the engine root is wrong — it must be the directory *containing* `Engine/` |
| `dfx: FAILED` with no `LogDreamFX` line | re-run with `-Raw`; the failure was before DreamFX got control |
| `0 built, 1 up to date` | add `-Force` |
| `DFX3001` no module named X | run `dfx.ps1 list` to see what the search paths expose, or add the folder to `Settings.ModulePaths` |
| `DFX7001` no provenance stamp | the asset was not generated from this source. Build it once |
| `DFX7002` stale asset | source changed since the last build. Run `build` |
