# Getting started with DreamFX

DreamFX compiles text into standard Niagara assets. You write a `.dfs` file, run one command, and get
a `UNiagaraSystem` that a level, a blueprint or a sequencer can reference like any other. The text is
the source; the `.uasset` is a build product and is not edited by hand.

This page takes you from nothing to a running effect. Fifteen minutes, no editor required.

---

## 1. Check the plugin is on

`DreamFX` and `DreamFXEditor` are editor modules, so they load with the editor and with a commandlet.
Confirm the driver can reach the engine:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list
```

That prints every Niagara module on the default search paths. If it errors before printing anything,
the engine could not be resolved — pass `-Engine <root>` or set `UE_ENGINE_ROOT`.

---

## 2. Where source lives

Sources live under a `DFX/` directory, either the project's own or a plugin's:

```
DevTest/DFX/                        Root=""  or  Root="Game"     -> /Game
DevTest/Plugins/MoonToon/DFX/       Root="Plugin.MoonToon"       -> /MoonToon
```

The `Root="..."` in a file's header says which content root its `Name="..."` is relative to. A file at
`DFX/Effects/NS_Spark.dfs` declaring `Name="Effects/NS_Spark"` with `Root="Game"` builds
`/Game/Effects/NS_Spark`.

Three extensions:

| File | Declares | Produces |
| --- | --- | --- |
| `.dfs` | `System` | `UNiagaraSystem` |
| `.dfe` | `Emitter` | nothing on its own — merged into a `.dfs` by `from` |
| `.dfm` | `Module` / `DynamicInput` | `UNiagaraScript` |

---

## 3. Your first effect

Create `DFX/Effects/NS_Hello.dfs`:

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-100, -100, -100, 100, 100, 100);
    }

    // Exposed to blueprint as User.Speed, and settable with SetNiagaraVariableFloat.
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];
    }

    Emitter Motes
    {
        Settings = {
            SimTarget          = CPU;
            Determinism        = true;
            RandomSeed         = 1;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode      = DirectSet,
                Lifetime          = 2.0,
                SpriteSizeMode    = Uniform,
                UniformSpriteSize = 8.0
            );
            SystemLocation();
            AddVelocityInCone(ConeAngle = 30.0, VelocityStrength = User.Speed);
        }

        ParticleUpdate = {
            // Advances NormalizedAge. Anything age-driven needs it, and Niagara reports an unmet
            // dependency if it is missing.
            ParticleState();
            GravityForce(Gravity = (0, 0, -400));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core
        {
            Alignment  = Unaligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;
        }
    }
}
```

Build it:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

You should see `1 built, 0 up to date, 0 failed | 0 error(s)`, and the asset listed under
"Assets written to disk by this run". Open `/Game/Effects/NS_Hello` in the editor and it plays.

A second build with no edits reports `0 built, 1 up to date` — the provenance stamp on the asset holds
a hash of the source, and a matching hash means there is nothing to do. `-Force` overrides it.

---

## 4. When it does not compile

Every diagnostic is a `DFXnnnn` with a file, line and column:

```
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

Two things to reach for:

- **[diagnostics/](diagnostics/README.md)** — every code, with what causes it and how to fix it.
- **`dfx.ps1 schema <Module>`** — a module's real input signature, read from the asset. This is the
  authority on names: Niagara input names contain spaces (`Loop Duration`), which DreamFX normalises,
  and some inputs only exist once a static switch above them is set.

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce
```

---

## 5. Iterating

Save-and-rebuild is wired up: the editor watches every `DFX/` root and rebuilds a source when it is
saved. Open the generated asset in the Niagara editor and it refreshes on each rebuild — that is the
live preview, and it costs nothing to set up.

One rule, and it is not optional: **despawn instances of a system before rebuilding it.** A live
`NiagaraActor` during a hot-swap can see a half-built renderer layout and assert on a render worker,
which takes the editor with it. It is an engine race, not something DreamFX can guard.

---

## 6. Working from inside the editor

Everything above works headlessly, and every one of it has a button. The buttons call the same code —
a menu is an entry point, never a second implementation.

| Where | What |
| --- | --- |
| *Tools ▸ DreamFX* | **Rebuild DFX**, **Verify DFX**, **Open DreamFX Workspace (VSCode)** |
| Level Editor toolbar | **DFX** (= Rebuild) and the workspace button |
| Right-click a Niagara System | **DreamFX ▸** Export .dfs / Adopt, or — if DreamFX generated it — Open Source / Rebuild from Source / Verify |
| Right-click a Niagara Emitter | **DreamFX ▸ Export .dfe** |
| Niagara system editor toolbar | the same **DreamFX** menu for the system being edited |

Two of these are worth reading about before using them:

- **Open DreamFX Workspace** writes `DFX/DreamFX.code-workspace` with one folder per source root and
  opens it in VSCode. The file is rewritten in full every time, so keep per-user settings in
  `DFX/.vscode/`.
- **Adopt** is not Export. Export writes a copy you can read *and edit*: saving it rebuilds a mirror
  at `/<mount>/Decompiled/<original directory>/<asset>`, never the asset it was read from. Adopt makes
  the text the only source of truth for that asset, and refuses if anything about the asset cannot be
  expressed. The full comparison is in
  [tools/editor-integration.md](tools/editor-integration.md#export-vs-adopt).

A failed build now toasts with an **Open in VSCode** link straight to the offending line, and the
"this asset is generated" warning has one to its source file.

Launch with `-NoDreamFXEditor` to turn the whole interactive surface off — menus, watcher and guard —
when DreamFX itself is the thing being debugged.

---

## 7. Before you commit

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

Four steps, each catching something the others cannot:

| Step | Catches |
| --- | --- |
| `lint` | GPU emitter with no bounds, uncapped spawn rate, randomness with no seed |
| `build` | anything that stops the text becoming an asset |
| `verify` | **a source edited and committed without a rebuild** — build alone passes, because build fixes it |
| `corpus` | a *behaviour* changing rather than breaking: diagnostics moving, topologies shifting, decompile losing idempotence |

`verify` is the one worth understanding. It compares each asset's provenance stamp against its source;
a mismatch means the committed asset and the committed text disagree, which is invisible any other way.

**Close the editor first.** `build` and `corpus` write `.uasset` files, and an editor open on the same
project writes them too — whichever saves second wins, and nothing says so. The run warns when it sees
an editor process, but it cannot tell which project that editor has open, so the warning is advice and
not a gate.

---

## Where to go next

- **[language/](language/README.md)** — the full language: [`.dfs`](language/dfs.md),
  [`.dfe`](language/dfe.md), [`.dfm`](language/dfm.md), and
  [values and rules](language/values.md).
- **[tools/editor-integration.md](tools/editor-integration.md)** — every menu, every toast, the
  workspace file, VSCode discovery, and how to call the same commands from Python.
- **[diagnostics/](diagnostics/README.md)** — every `DFXnnnn`.
- **[../CHANGELOG.md](../CHANGELOG.md)** — what each release covers, and the known-issue list.

To ask the coverage question of your own content — what fraction of it DreamFX can already
represent, and which gaps are worth closing — run it yourself:

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff       # L1 text + L2 compile
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff        # facts, independent of the exporter
```

There are four skills for agent-assisted work: `dream-fx-create`, `dream-fx-verify`,
`dream-fx-diagnose` and `dream-fx-decompile`.
