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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2827`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2860`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2901`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2905`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2990`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3278`
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
Nothing in this system writes that parameter, so there is no graph parameter for a default to sit on and Niagara refuses the read. This is a defect in the effect rather than a limit of this engine -- an engine carrying the MoonEngine Niagara additions creates the parameter and quietly hands back the type's zero, so the same source builds there and the effect silently does nothing (a scale that reads an unwritten value draws nothing either way). Fix it at the source: write the parameter before whatever reads it -- for an 'Emitter.<Module>.<Output>' name, move the module that produces it ahead of its readers -- or drop the read. DreamFX will not invent the parameter to make the build pass; that would hide the defect on every engine instead of one.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2853`
<!-- generated:end DFX6007 -->

**Cause.** Attached to a DFX6001 "read before being set". **Nothing anywhere in the system writes the parameter**, so no link write ever created it and there is no graph parameter for a default to sit on.

This note used to say the stock API could not write a default mode at all. It can --- `UNiagaraGraph::GetScriptVariable` is exported and `DefaultMode` is public on what it returns, so defaults land on any engine (2026-08-10). What is left is the narrower case above, and it is **a defect in the effect, not a limit of the engine**.

The two engines disagree only about whether you are told. MoonEngine's defaults call creates the parameter and hands back the type's zero, so the build passes --- and the effect silently does nothing. `NS_Spawn_Ground_Root` is the worked example: nothing writes `Particles.MySize`, so `MeshUniformScale = 1.0 * Particles.MySize` is a decal scaled to zero. It has been drawing nothing on both engines all along; only one of them said so.

**Fix.** At the source, not in the pipeline. Write the parameter before whatever reads it --- for an `Emitter.<Module>.<Output>` name, move the module that produces it above its readers --- or drop the read. Building on MoonEngine makes the message go away without making the effect work.

**Why DreamFX does not just create the parameter.** It could: `FGraphSurgeon::AddParameter` already creates one by reflection for the `.dfm` path. Deliberately not wired in (decision record, `Plan/open-problems-fixes.md`, 2026-08-10) --- it would hide the defect on every engine instead of one, and this project chooses visible over convenient. Reopen only for an asset of the same shape that has a **real L3 behavioural difference**.

**Not** a reason to distrust the build. It is an explanation attached to an error, never an error of its own.

