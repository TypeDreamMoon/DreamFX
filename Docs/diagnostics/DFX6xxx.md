# DFX6xxx --- Niagara compilation

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX6001

<!-- generated:begin DFX6001 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2960`
<!-- generated:end DFX6001 -->

**Cause.** A Niagara compile error, mapped back to the source line of the module that raised it.

**Fix.** The message is Niagara's own. An unresolved attribute usually means a module that writes it is missing or runs later in the stack than the module that reads it.

## DFX6002

<!-- generated:begin DFX6002 -->
**Severity** warning

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2994`
<!-- generated:end DFX6002 -->

**Cause.** A Niagara compile warning, mapped back to source.

**Fix.** Read the message. Warnings from a dependency are marked as such.

## DFX6003

<!-- generated:begin DFX6003 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3035`
<!-- generated:end DFX6003 -->

**Cause.** A Niagara stack issue at error level -- an unmet module dependency, most often. Stack issues are only readable where Slate exists, so these appear in the editor and in the corpus suite but not in a headless build (`GetStackIssues` is not headless-safe).

**Fix.** Add the module the dependency names. `ScaleSpriteSize` requiring `UpdateAge` (provided by `ParticleState`) is the common one -- without it `NormalizedAge` never advances and every age-driven curve evaluates at zero.

## DFX6004

<!-- generated:begin DFX6004 -->
**Severity** warning

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3039`
<!-- generated:end DFX6004 -->

**Cause.** A Niagara stack issue at warning level. A deprecated module reports here.

**Fix.** The message usually names the replacement asset.

## DFX6005

<!-- generated:begin DFX6005 -->
**Severity** error

**Message**

```
Niagara compilation of '%s' did not succeed (status %s).
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3124`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3459`
<!-- generated:end DFX6005 -->

**Cause.** The system's compile did not reach a successful state. Individual errors are reported as DFX6001 above this.

**Fix.** Fix the errors above. A status of `Dirty` with no errors means the compile did not finish.

## DFX6006

<!-- generated:begin DFX6006 -->
**Severity** error

**Message**

```
Niagara could not compile the body of '%s':\n%s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1264`
<!-- generated:end DFX6006 -->

**Cause.** A `.dfm`'s body is not valid HLSL once lowered. The Niagara message follows.

**Fix.** Read the translator error. `Cannot access field 'X' of structure` means a namespaced reference did not resolve -- `Particles.*` has to be a known attribute or declared with a type in the body (DFX3046).

## DFX6007

<!-- generated:begin DFX6007 -->
**Severity** info

**Message**

```
Nothing in this source writes that parameter, so there is no graph parameter for a default to sit on and Niagara refuses the read. Check the asset before the effect: the decompiler drops any module input whose value matches a freshly probed module, and a value the author set that happens to equal the default is dropped with them -- NS_Spawn_Ground_Root reads Particles.MySize and builds clean once the suppressed writes are exported, so its text was missing a write its asset had. If the source really is the whole story, write the parameter before whatever reads it -- for an 'Emitter.<Module>.<Output>' name, move the module that produces it ahead of its readers -- or drop the read. Building on an engine with the MoonEngine additions makes the message go away by supplying the type's zero, which is not the same as making the effect work.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2987`
<!-- generated:end DFX6007 -->

**Cause.** Attached to a DFX6001 "read before being set": nothing in the **source text** writes the parameter, so no link write created it and there is no graph parameter for a default to sit on.

**Check the asset before the effect.** The first asset to carry this message, `NS_Spawn_Ground_Root`, was ruled a defective effect on the evidence that its export had no writer --- and the export was the lossy step. A suppressed static switch several stacks away had been dropped by the decompiler, the rebuild compiled the other branch, and the missing write was a casualty with an unrelated name. The asset wrote the parameter all along; once switches export unconditionally (2026-08-11) it builds clean on both engines. That ruling is void, and the shape of the mistake is the house rule it produced: proving "the asset does not contain X" takes asset-level evidence (PkgInfo, a reflection walk, asset-diff), never an export product.

**Fix.** First re-export the asset with a current build --- if the message goes away, the source was missing something the exporter used to drop, and nothing was ever wrong with the effect. If the source genuinely is the whole story (hand-written, or the asset really has no writer), fix it there: write the parameter before whatever reads it --- for an `Emitter.<Module>.<Output>` name, move the module that produces it above its readers --- or drop the read. Building on MoonEngine makes the message go away by supplying the type's zero, which is not the same as making the effect work.

**Why DreamFX does not just create the parameter.** It could: `FGraphSurgeon::AddParameter` already creates one by reflection for the `.dfm` path. Deliberately not wired in (decision record, `Plan/open-problems-fixes.md`, 2026-08-10; the record's factual basis is void but this guardrail stands on its own) --- reflection parameter creation in the `.dfs` path would invert the layer dependency and paper over real source defects on every engine.

**Not** a reason to distrust the build. It is an explanation attached to an error, never an error of its own.

## DFX6008

<!-- generated:begin DFX6008 -->
**Severity** error

**Message**

```
'%s' finished its compile with stale scripts: %s. The compiled VM was not rebuilt from the graphs this build wrote, so the asset would simulate something other than what the source says. This is a DreamFX pipeline defect -- report it with this source file.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3158`
<!-- generated:end DFX6008 -->

**Cause.** The build finished its Niagara compile, and the engine's own synchronization test then said some script's stored VM was **not** compiled from the graphs as this build left them. That should be impossible with the build forcing its final compile, which is exactly why it is an error: either a write path mutated a graph after the compile was issued, or the force was lost somewhere. The check exists because this class of asset has shipped before --- `NE_C` of `NS_Spawn_Teleport_Root` carried bytecode from an all-Unset-switches era under the compile id of the correct graph, and simulated the wrong branches for days while every text-level check agreed with the source (2026-08-11).

**Fix.** Not in the source --- the message names a DreamFX pipeline defect. Rebuild once with `-Force`; if it recurs, report it with the source file and the named scripts. The stale scripts list says which emitter and which stage to look at.

