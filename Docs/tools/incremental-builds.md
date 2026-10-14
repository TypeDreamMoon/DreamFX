# Incremental builds

> [DreamFX](../../README.md) » **Incremental builds**

Saving a `.dfs` used to clear every stack of its system and rebuild everything, so the cost of a save
was proportional to the size of the whole system, not to the edit. An incremental build compares the
new build plan with the one the last build recorded and writes only the difference.

| | |
| :-- | :-- |
| Editor | on by default for the source watcher's save-triggered rebuilds — *Project Settings ▸ Plugins ▸ DreamFX ▸ Workspace ▸ Incremental Build on Save* |
| Command line | off by default; `-Incremental` opts in |
| Always full | *Rebuild DFX*, *Rebuild from Source* and the bulk-backlog **Rebuild them now** link |
| Stored in | the asset's package metadata, key `DreamFX.PlanDigest`, next to the provenance stamp (`DreamFX.SourceHash` and friends) |

Measured on a 1230-line system (`NS_Magic_Explosion`), save to rebuilt asset, in the editor:

| Save | MoonEngine build | Stock prebuilt UE 5.8 |
| :-- | :-- | :-- |
| Full rebuild (before) | 17 s | 56 s |
| One value changed (five places) | 1.0–1.3 s | 1.7 s |
| The same, first saves after the editor starts | ~3 s | ~4 s |
| A module enabled (two stacks rebuilt) | 2.5–2.7 s | full rebuild |

Most of a value save is now Niagara's own recompile of the system, which a rapid-iteration value
write still asks for. The first saves after start-up also pay the module probes that the running
editor then keeps.

On the stock prebuilt engine the incremental path covers value edits and emitters added or removed;
any other structural change — a stack or an emitter that has to be rebuilt — runs the full rebuild.
Rebuilding one stack has to drop the rapid-iteration constants of the modules it removed first, or a
re-added module keeps the constant its previous node left — a hidden input's old value where the full
rebuild has the module default. The engine's own cleanup for that is not exported there; the per-edit
compiles that engine used to launch did it as a side effect, and the build now holds those off. The
MoonEngine build calls the cleanup and rebuilds single stacks.

The full rebuild itself is faster on the stock engine than the table above says: the build window
keeps a compile request pending so the engine launches no compile until the build's own, a static
switch write refreshes only its module, a batch of module adds refreshes its stack once, an emptied
stack is cut out of the graph in one step, module edits keep the edit context wherever it can be
refreshed in place, and each module probe shares one context. A headless full rebuild of
`NS_Magic_Explosion` went from 110 s to about 32 s, engine start-up included; its system view models
went from several hundred to 163. Each step has a switch that restores the old behaviour,
for one-binary comparisons and as an escape hatch: `-RebuildOnSwitch`, `-RebuildPerAdd` and
`-ClearPerModule` (the same names on `dfx.ps1 build`).

## How it works

Each successful system build stores a **plan digest**: a hash tree of the *flattened* build plan —
`.dfe` merges, `Parent` / `inherits` inheritance and module versions already resolved — plus the node
names the build gave each module and a hash of the asset's build-safety facts. The digest sits in the
package metadata beside the provenance stamp, so it is saved with the asset.

The next incremental build computes the new plan, compares it with the stored digest and writes the
difference:

| What changed | What the build does |
| :-- | :-- |
| An argument value — a literal, enum, link or `hlsl` expression | writes that value |
| The shape of an argument's dynamic-input chain — a dynamic input added, replaced or taken away | resets the argument and rewrites it as a group |
| A literal or enum argument the source no longer sets, or a data interface value such as a curve | clears and rebuilds that one stack |
| A stack's modules, their order or versions, `as` names, `disabled` flags, Set Parameters entries or static switches | clears and rebuilds that one stack |
| Emitter settings, renderers, event handlers, simulation stages, parameter defaults, user parameters, system settings | re-applies that part, and only when it changed |
| An emitter was added, its parent (`inherits`) or `SimTarget` changed, or its stacks share a module with another stack being rebuilt | rebuilds that emitter whole |
| An emitter was removed | removes it |

Removing a literal argument rebuilds its stack rather than resetting the argument, because that is
what the full rebuild amounts to: it re-adds the module, and the rapid-iteration constants it does not
clean away keep the value the argument last had. A reset would put the module default there instead,
and the build would no longer equal the full rebuild — the build safety gate refuses it as a lost
value, too. A data interface value goes the same way because a value written into an interface already
in memory does not always reach the asset.

The shared-module case is about engine node names: when two stacks use the same module and one of
them is rebuilt, the engine would renumber the node names the other stack's inputs are stored under,
so the whole emitter is rebuilt instead of risking a mismatch.

### What each kind of edit costs

| Edit | Scope of the write | Cost follows |
| :-- | :-- | :-- |
| Change a literal or an expression | the argument | the edit — about a second on the system above, most of it the recompile |
| Edit inside a dynamic-input chain so its shape changes | that argument, as a group | the chain |
| Remove a line that set a literal argument; change a curve or other data interface | one stack | the stack |
| Add, remove, reorder or re-version a module; toggle `disabled`; change a Set Parameters entry or static switch | one stack | the stack |
| Add an emitter; change `inherits` or `SimTarget` | one emitter | the emitter |
| Reorder emitters; anything on the [fallback list](#when-it-falls-back-to-a-full-rebuild) | the whole system | the full rebuild |

### Compile

An incremental build asks Niagara for an ordinary, **non-forced** compile. Niagara recompiles only the
scripts whose graphs the writes reached; value writes that only land on rapid-iteration parameters
usually reach none, which is why a one-value edit can end with `nothing to recompile`. A full rebuild
still forces the compile.

The build verifies the compiled state afterwards exactly as a full build does — see
[Headless Niagara compilation](headless-compilation.md) — and forces a recompile if anything is
stale.

## When it falls back to a full rebuild

The Output Log says why, in the form `full rebuild (<reason>)`. The reasons:

| Reason | Meaning |
| :-- | :-- |
| the asset is new | there is nothing to compare with |
| the asset carries no stored plan | it was built by an older DreamFX, or adopted |
| the stored plan has another format, generator version or write path | the digest is from a different DreamFX generation, or from the other backend — MoonEngine fast-edit and the stock engine do not write the same way |
| the asset changed since its last build | the build-safety facts no longer match what the last build recorded: the asset was edited outside DreamFX (a hand edit in the Niagara editor, a revert from source control) |
| the emitters were reordered | emitter order is part of the plan's identity |
| a system stack or an emitter changes structure, which this engine rebuilds in full | stock prebuilt engine only, see above |
| a data interface refers to an emitter or renderer by position, and an emitter or renderer set changed | self-reference tokens would point at the wrong target |
| the incremental write itself failed | the full rebuild runs in the same build and reports its own diagnostics |

A fallback is a correct build, only a slower one. The next successful build stores a fresh digest, so
the build after a fallback is incremental again.

> [!NOTE]
> The "asset changed" fallback is the safety net, not a nuisance. A stored plan describes the asset as
> the last build left it. Once the asset has been edited outside DreamFX the plan no longer describes
> what is there, so a difference computed against it cannot be trusted, and only the full rebuild —
> the reference — is.

## The equivalence guarantee

**An incremental build produces the same asset a full rebuild of the same text would.** The full
rebuild stays the reference, and the incremental path is tested against it:

- An automation test builds the same edit history twice — once incrementally, edit by edit, and once
  in full — and compares the two assets' facts (`DescribeSystemFacts`, the facts the build safety gate
  and `asset-diff` read).

The test is `DreamFX.Incremental.Equivalence`; `DreamFX.Incremental.Coverage` runs the same histories
and also asserts which edits were applied incrementally rather than by falling back. Three fixture
systems (several emitters with a module shared by two stacks, an event source and receiver, a GPU
emitter with simulation stages) are edited by about ninety mutations, each a find-and-replace pair in
the mutation table at the top of `Tests/DreamFXIncrementalEquivalenceTest.cpp`, so a new case is one
more entry. Besides the facts, each step compares the raw node names and every module input as the
adapter reads it, and the decompiled text is compared once after the last step.

```text
UnrealEditor-Cmd <uproject> -ExecCmds="Automation RunTests DreamFX.Incremental; Quit" -unattended -nullrhi -nosplash -stdout -FullStdOutLogOutput -TestExit="Automation Test Queue Empty"
```

The log has one `DFXINCR` line per step with the incremental report and the wall time of each build,
which is also the before-and-after measurement. A difference is reported at the step that made it,
with both sides' facts, and both assets are then rebuilt from scratch so the next step is judged on
its own. `-DreamFXIncrementalDump=<dir>` writes each step's facts, names and inputs for both assets,
and `-DreamFXIncrementalFixture=<name>` with `-DreamFXIncrementalSteps=<word>|<word>` narrows a run to
a few mutations.

`DescribeSystemFacts` counts only data interfaces **reachable from the system through non-transient
references**. Interfaces a rebuild leaves behind in memory — removed nodes, `Invalidated_*` cached
defaults, transient placeholders — are not reported. This matters to the comparison: before, the same
asset could describe itself differently before and after a garbage collection or a save, so a
comparison of facts could depend on when the asset was last saved or collected. The build safety gate and `asset-diff` read the same facts, so
both are stable across a save and reload.

## Switching it on and off

| Where | Control | Default |
| :-- | :-- | :-- |
| Editor, save-triggered rebuilds | *Project Settings ▸ Plugins ▸ DreamFX ▸ Workspace ▸ Incremental Build on Save* (`bIncrementalBuildOnSave`) | on |
| Editor, *Rebuild DFX* / *Rebuild from Source* / **Rebuild them now** | not configurable | always full |
| Command line | `dfx.ps1 build <file> -Incremental`, or `UnrealEditor-Cmd <uproject> -run=DreamFX -File=<file> -Incremental` | off |

The command line is off by default on purpose: CI and the verify gate want the full rebuild's answer,
not an answer that depends on what the asset held before. Use `-Incremental` for a local edit loop
without an editor, never in a gate. See [Headless Niagara compilation](headless-compilation.md).

The editor's explicit commands are the escape hatch: if an asset ever looks wrong after a save,
*Rebuild from Source* rebuilds it in full from the text.

## Reading the log

The commandlet and the watcher log one line per file:

```text
incremental: 0 emitter(s) and 1 stack(s) rebuilt, 3 input(s) written, 1 reset, nothing to recompile
full rebuild (the asset changed since its last build)
```

Before it writes, the generator logs what it is about to touch, per stack:

```text
incremental build -- Sparks ParticleSpawnScript values(+0 ~1 -0)
```

In `values(+a ~b -c)` the three counts are the argument values added, changed and removed in that
stack.

## Module probes are made once

Planning reuses one module library per process. The module probes a build needs — resolving a module
name, reading its input signature — are made once per editor session or commandlet run instead of on
every build. In the editor the first build after startup can therefore be slower than the ones after it.

## Troubleshooting

| Symptom | Cause and fix |
| :-- | :-- |
| "My save always does a full rebuild" | Read the reason in the Output Log, on the `full rebuild (…)` line, and find it in the [fallback table](#when-it-falls-back-to-a-full-rebuild). The usual ones are the asset changing outside DreamFX between saves, and a system built by an older DreamFX — the first build after an upgrade is full, the next is not |
| Every save is full and the reason says the asset changed | Something rewrites the asset between your saves: a hand edit in the Niagara editor, a source-control revert, or another process saving the same package. The build after the next full one will be incremental unless it happens again |
| The first save after opening the editor is slower | The module probes are being made for the first time in this session; see [above](#module-probes-are-made-once) |
| `-Incremental` is passed but the log says `full rebuild (<reason>)` | Read the reason. For a new asset, an asset with no stored plan or a changed generator version, the full rebuild this run did stored a plan: the next `-Incremental` run is incremental |
| An asset looks different from what the source says | Run *Rebuild from Source* (or `dfx.ps1 build <file> -Force`) for the full-rebuild answer, then `verify` it. If the full rebuild and the incremental one disagree, that is a defect in the incremental path — keep the source and both assets |
| A save in a bulk batch did not build | That is the [bulk-batch gate](editor-integration.md#the-bulk-batch), not the incremental build; the **Rebuild them now** link is always a full rebuild |
| A gate or CI job should not depend on the previous asset | Do not pass `-Incremental` there. It is off unless asked for |

## See also

- [Editor integration](editor-integration.md) — the source watcher and the setting
- [Headless Niagara compilation](headless-compilation.md) — the `-Incremental` flag and compile verification
- [Asset comparison and export](asset-comparison-and-export.md) — the facts the guarantee is stated in
