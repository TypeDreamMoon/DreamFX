# DFX5xxx --- Generation and asset writing

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX5001

<!-- generated:begin DFX5001 -->
**Severity** error

**Message**

```
Stack '%s' has no Niagara script usage mapping.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1095`
<!-- generated:end DFX5001 -->

**Cause.** A stack kind with no Niagara script usage behind it. Every kind the parser can produce
maps today — the six fixed stacks, `OnEvent` and `Stage` — so reaching this means a new stack kind
was added to the enum without extending `ScriptUsageForStack`.

**Fix.** Use one of the mapped stacks; if you are adding a kind, extend the adapter's mapping pair
(`ScriptUsageForStack`, `StackForScriptName`) together.

## DFX5002

<!-- generated:begin DFX5002 -->
**Severity** warning

**Message**

```
This system declares no emitters, so it will produce nothing.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2104`
<!-- generated:end DFX5002 -->

**Cause.** A system with no emitters compiles and produces nothing.

**Fix.** Add an `Emitter` block.

## DFX5003

<!-- generated:begin DFX5003 -->
**Severity** info

**Message**

```
'%s' is not declared in this source, so its existing modules are left as-is: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2836`
<!-- generated:end DFX5003 -->

**Cause.** Declaring a stack means taking it over; a stack this source never mentions keeps whatever it had. `CreateNiagaraSystem` puts a `SystemState` in `SystemUpdate`, and clearing it wholesale would make every `.dfs` without an explicit `SystemUpdate` produce a system that never runs. Informational so the difference is visible rather than silent.

**Fix.** Nothing, usually. To take the stack over, declare it -- an empty `SystemUpdate = { }` clears it.

## DFX5004

<!-- generated:begin DFX5004 -->
**Severity** info

**Message**

```
No Material was set, so the engine default was applied: %s. Write 'Material = \"...\";' to choose your own.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2949`
<!-- generated:end DFX5004 -->

**Cause.** A renderer with no `Material` gets the engine default, which is why an untextured effect still draws.

**Fix.** Write `Material = "...";` to choose your own.

## DFX5030

<!-- generated:begin DFX5030 -->
**Severity** error

**Message**

```
SavePackage failed for '%s'.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1302`
<!-- generated:end DFX5030 -->

**Cause.** Writing the package failed -- read-only file, source control lock, or a path the process cannot write.

**Fix.** Check the file's permissions and check it out if it is under source control.

## DFX5031

<!-- generated:begin DFX5031 -->
**Severity** error

**Message**

```
Emitter '%s' declares more than one OnEvent block. Only one event handler per emitter is representable; split the extra handlers into their own emitters.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2035`
<!-- generated:end DFX5031 -->

**Cause.** An emitter block declares two or more `OnEvent` blocks.

DreamFX reaches event stacks through the external edit API's ordinary stack rails, and those rails can only address an event script whose usage id is the zero guid — which means exactly one addressable handler per emitter. A second handler would exist on the asset but be invisible to every read and write that follows, so the generator refuses it up front instead of building something it could never round-trip.

**Fix.** Keep one `OnEvent` block per emitter. If one emitter genuinely needs to react to two event streams, split it into two emitters, one handler each — the stacks stay identical and both spawn against the same source events.

## DFX5032

<!-- generated:begin DFX5032 -->
**Severity** error

**Message**

```
Emitter '%s' declares two Stage blocks named '%s'. Stages are identified by name; rename one.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2013`
<!-- generated:end DFX5032 -->

**Cause.** One emitter declares two `Stage` blocks with the same name. The write side
creates-or-reuses a stage by its name (the engine UI's own identity for stages), so both blocks
would land on one stage and it would silently hold whichever stack was written last. The same code
carries adapter-reported stage failures at build time — a slice that was never closed, a stage on
the asset whose class is not the engine's generic stage.

**Fix.** Rename one of the blocks. Stages within one emitter are identified by name, so each needs
its own.

## DFX5033

<!-- generated:begin DFX5033 -->
**Severity** error

**Message**

```
Emitter '%s' declares Stage blocks but simulates on the CPU. Simulation stages are a GPU feature: set `SimTarget = GPU` in the emitter's Settings.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3078`
<!-- generated:end DFX5033 -->

**Cause.** An emitter declares `Stage` blocks while simulating on the CPU. Simulation stages are a
GPU-compute feature; a CPU emitter cannot run one, and Niagara's own complaint arrives later as a
compile error naming no source line. The check reads the live emitter rather than this file's
`Settings`, because a `from` emitter can be GPU without the `.dfs` saying so.

**Fix.** Set `SimTarget = GPU` in the emitter's `Settings` block (with a `FixedBounds`, which GPU
emitters cannot compute for themselves), or delete the `Stage` blocks.

## DFX5034

<!-- generated:begin DFX5034 -->
**Severity** error

**Message**

```
Emitter '%s' uses 'as %s' on two module calls. Node names are unique per emitter; rename one.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2087`
<!-- generated:end DFX5034 -->

**Cause.** Two module calls in one emitter carry the same `as <name>` suffix. The suffix names the
function call NODE, `Output.<node>.<value>` links resolve by that name, and node names are unique
per emitter graph — two claimants would leave every such link resolving to whichever node the
traversal happens to meet first.

**Fix.** Rename one. The decompiler only emits `as` with the original asset's node names, which
are unique by construction, so this fires on hand-edited sources.

## DFX5093

<!-- generated:begin DFX5093 -->
**Severity** error

**Message**

```
'MaterialParam' is reserved syntax and is not implemented in v1 (plan section 7).
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1513`
<!-- generated:end DFX5093 -->

**Cause.** `MaterialParam` is reserved syntax (L8) with no implementation in v1.

**Fix.** Not available. Set material parameters on the material instance instead.

## DFX5097

<!-- generated:begin DFX5097 -->
**Severity** error

**Message**

```
Only System documents can be generated right now; this file declares a %s.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3498`
<!-- generated:end DFX5097 -->

**Cause.** Only `.dfs` and `.dfm` produce assets. A `.dfe` is merged into its host by copy (R3) and has nothing of its own to generate.

**Fix.** Reference the `.dfe` from a `.dfs` with `from`.

## DFX5098

<!-- generated:begin DFX5098 -->
**Severity** error

**Message**

```
Data interface parameter '%s' takes its configuration as a quoted JSON object, the form the decompiler writes.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1778`
<!-- generated:end DFX5098 -->

**Cause.** A `DI<T>` user parameter's configuration is a quoted JSON object — the same verbatim form a module's data interface input takes, and the form the exporter writes. Anything else here is a value the applier cannot read. The code also reports under DFX5098 when the configuration was well-formed but the engine refused to apply it, which usually means a property name in the blob no longer exists on that interface class.

**Fix.** Write the configuration as a quoted JSON object, or leave the parameter bare — a declaration with no value is a slot to fill at runtime, which is legitimate and silent. `dfx decompile` on a system that already has the interface configured the way you want prints the exact blob.

> **Changed 2026-08-12.** This used to be a warning meaning "declared only, v1 does not apply defaults" (plan 3.5). Configuration is now applied. That scope cut was costing real behaviour: the collision sources and property readers on `NS_Spawn_Ninja_Root` are all user data interfaces, and every mirror default-constructed them — the effect rendered and interacted with nothing.

## DFX5099

<!-- generated:begin DFX5099 -->
**Severity** info

**Message**

```
[Group] and [SortPriority] are kept in source only: the external edit API's user variable struct has no metadata fields to write them to.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1815`
<!-- generated:end DFX5099 -->

**Cause.** `[Group]` and `[SortPriority]` have nowhere to go: the external edit API's user variable struct carries name, type and description and no other metadata.

**Fix.** Nothing to do -- the attributes stay in the source as documentation. Set the grouping by hand in the asset if it matters, and expect a rebuild to leave it alone.

## DFX5100

<!-- generated:begin DFX5100 -->
**Severity** error

**Message**

```
'%s' is a %s with no generated asset at '%s', and %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:717`
<!-- generated:end DFX5100 -->

**Cause.** No previously generated asset was found, and this build has no graph backend to make one. That is rarer than it used to be: an engine that does not export the five declarations still gets the reflection backend, so reaching this means its startup self-check failed and the message names which check it was. See [dfm.md](../language/dfm.md) for the three outcomes.

**Fix.** Read the named check --- it says which engine shape the backend expected and did not find, which is the actual thing to fix or report. Meanwhile, build the module on an engine where a backend does run and commit the asset; any engine loads, references and cooks it normally. Or sidestep the module entirely with an inline `hlsl { }` expression or an existing dynamic input asset.

## DFX5101

<!-- generated:begin DFX5101 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:599`
<!-- generated:end DFX5101 -->

**Cause.** The `Root="..."` on a `.dfm` does not name a mounted content root.

**Fix.** `Root` is `Game`, empty, or `Plugin.<PluginName>`.

## DFX5102

<!-- generated:begin DFX5102 -->
**Severity** info

**Message**

```
Input '%s' is marked [StaticSwitch]. Tier-one generation (plan 3.3) lowers the whole Body to a single custom HLSL node, which has no branch for a switch to select, so it is written as an ordinary input instead. The body reads it the same way; only the compile-time folding is lost.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:946`
<!-- generated:end DFX5102 -->

**Cause.** Tier-one generation (plan 3.3) puts the whole body in one custom HLSL node, which has no branch for a switch to select, so a `[StaticSwitch]` input becomes an ordinary one. Said out loud because silently downgrading a declared compile-time switch to a runtime value is the kind of difference that surfaces later as a performance question nobody can source.

**Fix.** Nothing -- the body reads it the same way. Only the compile-time folding is lost.

## DFX5103

<!-- generated:begin DFX5103 -->
**Severity** error

**Message**

```
Package '%s' exists on disk but could not be loaded.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:629`
<!-- generated:end DFX5103 -->

**Cause.** The package exists on disk but would not load. Usually a partially written file or one held by another process.

**Fix.** Check the file. If the editor has it open, close the asset and rebuild.

## DFX5104

<!-- generated:begin DFX5104 -->
**Severity** error

**Message**

```
Package '%s' exists but holds no Niagara script named '%s'. Refusing to overwrite it.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:638`
<!-- generated:end DFX5104 -->

**Cause.** The target package exists and holds something other than the expected script. DreamFX refuses to overwrite it rather than replacing an unrelated asset.

**Fix.** Change `Name="..."`, or delete the asset if it really is stale.

## DFX5105

<!-- generated:begin DFX5105 -->
**Severity** error

**Message**

```
Could not create package '%s'.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:994`
<!-- generated:end DFX5105 -->

**Cause.** The package could not be created -- usually an unmounted root or an invalid name.

**Fix.** Check `Root` and `Name`.

## DFX5106

<!-- generated:begin DFX5106 -->
**Severity** error

**Message**

```
Could not wire the module graph. The Niagara schema rejected a parameter map connection.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1084`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1109`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1118`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1132`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1209`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1218`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1235`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1247`
<!-- generated:end DFX5106 -->

**Cause.** The Niagara schema rejected a connection while building the module graph. A type mismatch between an input's declared type and the pin it feeds is the usual cause.

**Fix.** Check the input's type. If it is a type Niagara has no pin for, the module cannot take it.

## DFX5107

<!-- generated:begin DFX5107 -->
**Severity** error

**Message**

```
'%s' no longer matches the module asset at '%s', and this build cannot regenerate it. Rebuild it where a graph backend runs and commit the updated asset; %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:734`
<!-- generated:end DFX5107 -->

**Cause.** The `.dfm` source no longer matches its committed asset, and this build has no graph backend to regenerate it. Distinct from DFX5100 because the remedy differs: there is an asset, it is simply out of date.

**Fix.** Rebuild the module where a backend runs and commit the updated asset. The trailing half of the message names the check that failed, which is what to fix if you expected this engine to be able to generate.

