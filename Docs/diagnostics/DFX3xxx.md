# DFX3xxx --- Declarations and document structure

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX3000

<!-- generated:begin DFX3000 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1738`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1749`
<!-- generated:end DFX3000 -->

**Cause.** The `Root="..."` does not name a mounted content root, or `Name="..."` has no asset name after its last slash.

**Fix.** `Root` is `Game`, empty, or `Plugin.<PluginName>`. `Name` is a path ending in the asset's name, e.g. `Systems/NS_Spark`.

## DFX3001

<!-- generated:begin DFX3001 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1328`
<!-- generated:end DFX3001 -->

**Cause.** The module name resolved to nothing on the search paths. Distinct from DFX3003, which means the module exists but has no such input -- a typo in a path and a typo in an argument would otherwise read the same.

**Fix.** Add the folder to `Settings.ModulePaths`, or write the full asset path. `dfx list` prints every module on the current paths.

## DFX3002

<!-- generated:begin DFX3002 -->
**Severity** error

**Message**

```
Could not read the input schema of module '%s': %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1369`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1422`
<!-- generated:end DFX3002 -->

**Cause.** The module was found but its input signature could not be read. The schema is probed by adding the module to a transient system, so this usually means the module cannot live in the stack it was written in.

**Fix.** Check the stack. `dfx schema <Module> -Stack <Stack>` reproduces the probe.

## DFX3003

<!-- generated:begin DFX3003 -->
**Severity** error

**Message**

```
Module '%s' has no input named '%s'.%s Available inputs: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1486`
<!-- generated:end DFX3003 -->

**Cause.** The module has no input by that name. Niagara input names contain spaces (`Loop Duration`); DreamFX normalises both sides, so `LoopDuration` matches, but a misspelling does not.

**Fix.** Use one of the names listed. A near-miss is suggested. `dfx schema <Module>` prints the full signature, including inputs hidden behind a static switch.

## DFX3004

<!-- generated:begin DFX3004 -->
**Severity** error

**Message**

```
Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, LightRenderer, DecalRenderer, ComponentRenderer, VolumeRenderer, or any UNiagaraRendererProperties subclass.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1549`
<!-- generated:end DFX3004 -->

**Cause.** Renderer types are a closed set.

**Fix.** Use one of the listed types. Renderer *properties* are schema-driven and open (L8); only the type keyword is fixed.

## DFX3005

<!-- generated:begin DFX3005 -->
**Severity** error

**Message**

```
Emitter '%s' is declared more than once. Emitter names are stable keys and must be unique.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1925`
<!-- generated:end DFX3005 -->

**Cause.** Two emitters share a name. The name is the stable key the regeneration contract matches handles by (plan 4.5), so a duplicate is data loss, not a naming nit.

**Fix.** Rename one. To rename an emitter that already exists in an asset, use `dfx rename` first so the handle and its rapid-iteration parameters survive (R4).

## DFX3006

<!-- generated:begin DFX3006 -->
**Severity** error

**Message**

```
'%s' is neither an allowed inline function (%s) nor a dynamic input: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:992`
<!-- generated:end DFX3006 -->

**Cause.** A call in a value position matched neither the L6 builtin whitelist nor any dynamic input asset.

**Fix.** Use one of the builtins listed, reference a real dynamic input, or write the maths in an `hlsl { }` block. Widening the whitelist is a design decision (L6), not a config change.

## DFX3007

<!-- generated:begin DFX3007 -->
**Severity** error

**Message**

```
Could not read the input schema of dynamic input '%s': %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1034`
<!-- generated:end DFX3007 -->

**Cause.** The dynamic input asset was found but its signature could not be read.

**Fix.** `dfx schema <Name>` reproduces the read against the same asset.

## DFX3008

<!-- generated:begin DFX3008 -->
**Severity** error

**Message**

```
Dynamic input '%s' has no input named '%s'. Available inputs: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1086`
<!-- generated:end DFX3008 -->

**Cause.** The dynamic input has no input by that name -- DFX3003 one level down a chain.

**Fix.** Use one of the names listed.

## DFX3009

<!-- generated:begin DFX3009 -->
**Severity** error

**Message**

```
Dynamic input '%s' is pinned to version %s, which its asset does not offer. Available version(s): %s.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1016`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1351`
<!-- generated:end DFX3009 -->

**Cause.** An R7 `@version` pin disagrees with the module asset's exposed version, or pins an asset that never opted into versioning. The pin records which version the source was written against; DreamFX cannot build against any other one, because the external edit API has no way to select a version.

**Fix.** Retest against the exposed version and update the pin, restore the version on the asset, or drop the `@` if the module is unversioned.

## DFX3010

<!-- generated:begin DFX3010 -->
**Severity** error

**Message**

```
Property '%s': %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1859`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:328`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:954`
<!-- generated:end DFX3010 -->

**Cause.** A `Properties` entry is malformed -- see the inner message.

**Fix.** Fix the declaration. Types are listed in DFX4021's message.

## DFX3020

<!-- generated:begin DFX3020 -->
**Severity** error

**Message**

```
Cannot read the default for %s setting '%s'.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1773`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:595`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:624`
<!-- generated:end DFX3020 -->

**Cause.** A settings key is not supported, or the generator cannot read a mapped setting's fresh-asset default from the current engine. The latter prevents rebuilding with stale values for settings removed from the source.

**Fix.** For an unknown key, use one of the supported names listed in the diagnostic. If reading defaults failed, check the accompanying engine/probe error and report the engine version and setting name; rebuilding requires a valid default schema.

## DFX3021

<!-- generated:begin DFX3021 -->
**Severity** error

**Message**

```
User parameter '%s' is declared more than once.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1806`
<!-- generated:end DFX3021 -->

**Cause.** Two user parameters share a name; a blueprint `SetNiagaraVariable` would reach whichever won.

**Fix.** Rename one.

## DFX3030

<!-- generated:begin DFX3030 -->
**Severity** error

**Message**

```
A Module or DynamicInput must declare Settings.Usage -- it decides which stacks the module can be placed in.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:186`
<!-- generated:end DFX3030 -->

**Cause.** `Usage` decides which stacks a module may be placed in. Without it the module exists and is unreachable from every stack.

**Fix.** Add `Usage = ParticleUpdate;` (or another stack). An array selects several: `Usage = [ParticleSpawn, ParticleUpdate];`

## DFX3031

<!-- generated:begin DFX3031 -->
**Severity** error

**Message**

```
A DynamicInput must declare Settings.Output -- its return type cannot be inferred from the body.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:194`
<!-- generated:end DFX3031 -->

**Cause.** A dynamic input's return type cannot be inferred from its body.

**Fix.** Add `Output = float;` (or the type it returns).

## DFX3032

<!-- generated:begin DFX3032 -->
**Severity** error

**Message**

```
A DynamicInput's Usage must be DynamicInput or a nonempty array of stack names.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:218`
<!-- generated:end DFX3032 -->

**Cause.** A `DynamicInput` declares an invalid Usage value: a bare stack name, an empty array, or an array containing an unknown stack name.

**Fix.** Use `Usage = DynamicInput;` for its default stack mask, or a nonempty stack array such as `Usage = [SystemSpawn, SystemUpdate];` to select where the input is available.

## DFX3033

<!-- generated:begin DFX3033 -->
**Severity** error

**Message**

```
Input '%s' is declared more than once.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:228`
<!-- generated:end DFX3033 -->

**Cause.** Two inputs share a name and would collide on the same `Module.` parameter.

**Fix.** Rename one.

## DFX3034

<!-- generated:begin DFX3034 -->
**Severity** error

**Message**

```
Input '%s' is marked [StaticSwitch] but is a %s. A static switch must be a bool, an int or an enum.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:243`
<!-- generated:end DFX3034 -->

**Cause.** R5: a static switch is resolved at compile time, so it has to be something a switch can branch on.

**Fix.** Make it `bool`, `int`, or an enum -- or drop the `[StaticSwitch]`.

## DFX3035

<!-- generated:begin DFX3035 -->
**Severity** error

**Message**

```
Input '%s' is a [StaticSwitch], so its default must be a compile-time constant.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:249`
<!-- generated:end DFX3035 -->

**Cause.** R5 from the other side: a switch resolved at compile time cannot take a runtime value.

**Fix.** Give it a literal default.

## DFX3036

<!-- generated:begin DFX3036 -->
**Severity** error

**Message**

```
The Body block is empty.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:258`
<!-- generated:end DFX3036 -->

**Cause.** An empty body generates a module that occupies a stack slot and does nothing.

**Fix.** Write the body, or delete the file.

## DFX3037

<!-- generated:begin DFX3037 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1052`
<!-- generated:end DFX3037 -->

**Cause.** A dynamic input's body is not a single expression. The Niagara translator wraps it as `Output = (Type)( <body> );`, so statements before the return produce invalid HLSL rather than an error naming the real problem.

**Fix.** Fold it into one expression, or write it as a `Module` -- a module's body is emitted verbatim and can hold as many statements as it likes.

## DFX3038

<!-- generated:begin DFX3038 -->
**Severity** error

**Message**

```
'%s' is not a stack a module can be placed in. Use one of: %s.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:929`
<!-- generated:end DFX3038 -->

**Cause.** `Usage` names one of the six stacks (L1) and nothing else.

**Fix.** Use one of the names listed.

## DFX3039

<!-- generated:begin DFX3039 -->
**Severity** error

**Message**

```
A DynamicInput cannot return a data interface; its Output must be a value type.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:970`
<!-- generated:end DFX3039 -->

**Cause.** A dynamic input feeds a value into an input slot; a data interface is not a value.

**Fix.** Return a value type. Data interfaces are declared in `Properties` and fed at runtime (plan 3.5).

## DFX3040

<!-- generated:begin DFX3040 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1944`
<!-- generated:end DFX3040 -->

**Cause.** The `from` path did not resolve to a `.dfe` on disk.

**Fix.** Paths resolve relative to the referencing file first, then against every DFX root. The extension is optional.

## DFX3041

<!-- generated:begin DFX3041 -->
**Severity** error

**Message**

```
'%s' could not be parsed; see the errors above.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1957`
<!-- generated:end DFX3041 -->

**Cause.** The referenced `.dfe` has its own errors; they are reported above this one against the `.dfe`'s own path.

**Fix.** Fix the `.dfe`. Its diagnostics carry its own file and line, not the host's.

## DFX3042

<!-- generated:begin DFX3042 -->
**Severity** error

**Message**

```
'%s' declares a %s, but 'from' needs an Emitter document.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1967`
<!-- generated:end DFX3042 -->

**Cause.** `from` pulls in an emitter, and the referenced file declares something else.

**Fix.** Point `from` at a `.dfe`.

## DFX3043

<!-- generated:begin DFX3043 -->
**Severity** error

**Message**

```
'%s' reads user parameters this system does not declare: %s. Add them to the Properties block.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1725`
<!-- generated:end DFX3043 -->

**Cause.** A `.dfe` is merged by copy (R3), including whatever `User.*` it reads. The host has to declare those or the copied emitter reads a parameter that does not exist. The diagnostic points at the `from` line, because that is where the decision was made.

**Fix.** Add the named parameters to the host's `Properties` block.

## DFX3044

<!-- generated:begin DFX3044 -->
**Severity** error

**Message**

```
The default for input '%s' has to be a literal or an enum entry. A module input default is stored on the asset, so it cannot reference anything outside the module.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1005`
<!-- generated:end DFX3044 -->

**Cause.** A module input's default is stored on the asset, so it cannot reference anything outside the module.

**Fix.** Use a literal or an enum entry. To make it caller-supplied, leave it and set it at the call site.

## DFX3046

<!-- generated:begin DFX3046 -->
**Severity** error

**Message**

```
'%s' is not a particle attribute DreamFX knows the type of. Write its type at first use, for example `float %s = ...;`.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:621`
<!-- generated:end DFX3046 -->

**Cause.** A `.dfm` body touches a `Particles.*` attribute that is neither a common Niagara attribute nor declared in the body. The pin wired for it needs a type, and guessing would wire one of the wrong width.

**Fix.** Write the type at its first use, the way a `.dfs` declares a new attribute: `float Particles.Moon.Phase = 0.0;`

## DFX3047

<!-- generated:begin DFX3047 -->
**Severity** error

**Message**

```
A DynamicInput computes a value; it cannot write '%s'. Move the write into a Module.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1041`
<!-- generated:end DFX3047 -->

**Cause.** A dynamic input computes a value in an input slot; it has no place in the stack to write from.

**Fix.** Move the write into a `Module`.

## DFX3048

<!-- generated:begin DFX3048 -->
**Severity** error

**Message**

```
Default override '%s' changes its type from %s to %s. Overrides must keep the base parameter's type.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXEmitterMerge.cpp:84`
<!-- generated:end DFX3048 -->

**Cause.** An emitter using `from` overrides a base `Defaults` parameter with a different type. Base stacks may still read the parameter using its original type.

**Fix.** Keep the base parameter's type when overriding its value. Use a separate parameter name if a different type is required.

## DFX3049

<!-- generated:begin DFX3049 -->
**Severity** error

**Message**

```
MeshRenderer has no Material property. Set OverrideMaterials = [\"/path/to/material\"]; and bOverrideMaterials = true; to override its mesh materials.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1600`
<!-- generated:end DFX3049 -->

**Cause.** A `MeshRenderer` declares `Material`, but mesh materials are controlled by its override array and enable switch.

**Fix.** Replace `Material` with `OverrideMaterials = ["/path/to/material"];` and set `bOverrideMaterials = true;`.

## DFX3050

<!-- generated:begin DFX3050 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:138`
<!-- generated:end DFX3050 -->

**Cause.** The system's `Parent` reference cannot be resolved to a `.dfs` source file.

**Fix.** Correct the parent source path and ensure the referenced `.dfs` file exists in a supported source location.

## DFX3051

<!-- generated:begin DFX3051 -->
**Severity** error

**Message**

```
Only a System document can inherit a .dfs parent.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:131`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:154`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:161`
<!-- generated:end DFX3051 -->

**Cause.** The parent source cannot be parsed, does not declare a `System`, or is being inherited by a non-system document.

**Fix.** Use system `Parent` inheritance only between `.dfs` system documents and fix any diagnostics reported for the parent file.

## DFX3052

<!-- generated:begin DFX3052 -->
**Severity** error

**Message**

```
System inheritance contains a cycle or exceeds 128 sources: %s -> %s.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:144`
<!-- generated:end DFX3052 -->

**Cause.** The system's parent chain revisits a source file or exceeds the limit of 128 sources.

**Fix.** Remove the circular parent reference shown in the diagnostic or shorten the inheritance chain.

## DFX3053

<!-- generated:begin DFX3053 -->
**Severity** error

**Message**

```
Inherited parameter '%s' changes type from %s to %s. An override must preserve the parent's parameter type.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:74`
<!-- generated:end DFX3053 -->

**Cause.** A child system redeclares an inherited parameter with a different Niagara type.

**Fix.** Preserve the parent's parameter type when overriding its value, or choose a different name for a new parameter.

## DFX3054

<!-- generated:begin DFX3054 -->
**Severity** error

**Message**

```
Parameter '%s' is declared twice in one inheritance layer. An override must be declared in the child source, not twice in the same source.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:22`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:34`
<!-- generated:end DFX3054 -->

**Cause.** One source in a system inheritance chain declares the same parameter or emitter name more than once.

**Fix.** Keep one declaration per name in each source file and place inherited overrides in the child source.

## DFX3055

<!-- generated:begin DFX3055 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2005`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2011`
<!-- generated:end DFX3055 -->

**Cause.** An emitter's `inherits` path cannot be resolved to a standalone Niagara emitter asset with inheritance enabled.

**Fix.** Correct the asset path and enable inheritance on that emitter, or use `from "...dfe"` when the intended parent is DreamFX source.

## DFX3056

<!-- generated:begin DFX3056 -->
**Severity** error

**Message**

```
Native parent has an unavailable version or a cyclic parent chain: %s.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2024`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2031`
<!-- generated:end DFX3056 -->

**Cause.** A native emitter parent chain contains an unavailable version, a cycle, or unmerged ancestor changes.

**Fix.** Select an existing version GUID, remove any parent cycle, and merge and save pending ancestor changes in Niagara before rebuilding.

## DFX3057

<!-- generated:begin DFX3057 -->
**Severity** error

**Message**

```
Attribute '%s' is declared with conflicting types.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:568`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:575`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:693`
<!-- generated:end DFX3057 -->

**Cause.** A `.dfm` custom attribute has conflicting types, is read during its own initialization, or is first initialized on a path that might not execute.

**Fix.** Give a new attribute one unconditional whole-value initializer before control flow; to update a custom attribute supplied by an earlier module, declare its type without an initializer and keep subsequent types consistent.

