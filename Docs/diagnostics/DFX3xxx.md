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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1989`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2000`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1520`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1561`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1614`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1678`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1741`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2176`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1184`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1226`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1278`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1208`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1543`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1146`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2110`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:338`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2024`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:738`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:767`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2057`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:187`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:195`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:219`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:229`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:244`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:250`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:259`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1054`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:931`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:972`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2195`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2208`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2218`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1975`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1007`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:623`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1043`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1792`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:178`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:171`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:194`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:201`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:184`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:85`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:22`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:34`, `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:45`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2256`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2262`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2275`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2282`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:570`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:577`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:695`
<!-- generated:end DFX3057 -->

**Cause.** A `.dfm` custom attribute has conflicting types, is read during its own initialization, or is first initialized on a path that might not execute.

**Fix.** Give a new attribute one unconditional whole-value initializer before control flow; to update a custom attribute supplied by an earlier module, declare its type without an initializer and keep subsequent types consistent.

## DFX3058

<!-- generated:begin DFX3058 -->
**Severity** error

**Message**

```
Patch '%s': unknown topology '%s'. Expected Grid(Columns, Rows), Strip(Columns), Ring(Columns, Rows), Tube(Columns, Rows) or Sphere(Columns, Rows).
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:428`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:438`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:449`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:465`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:476`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:487`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:505`
<!-- generated:end DFX3058 -->

**Cause.** A patch's topology call is not one the lowering knows: an unknown topology, the wrong number of vertex counts (a `Strip` takes one, the others two), a count that is not an integer literal of at least 2, or an unknown or misshapen option (`Copies`, `Material`, `Flip`).

**Fix.** Write `Grid`, `Strip`, `Ring`, `Tube` or `Sphere` with integer vertex counts, then the options by name: `Tube(65, 17, Copies = 3, Flip = true)`. Counts are vertex counts, so a closed shape's seam column is included.

## DFX3059

<!-- generated:begin DFX3059 -->
**Severity** error

**Message**

```
%s needs an emitter named '%s', which %s already uses. Rename one of them.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1041`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1066`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1125`
<!-- generated:end DFX3059 -->

**Cause.** A `DynamicMesh` named `X` becomes the user parameter `User.X`, the emitter `X` that draws it and one emitter `X_<Patch>` per patch. One of those names is already taken -- by `Properties`, by an `Emitter` block, by another block -- or two patches of one block share a name.

**Fix.** Rename the block, the patch, or whatever already holds the name. The names are part of the built asset, so blueprints that set `User.X` keep working only while the block keeps its name.

## DFX3060

<!-- generated:begin DFX3060 -->
**Severity** error

**Message**

```
Material takes a quoted material asset path, e.g. Material = \"/Game/FX/M_Shell.M_Shell\";
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1404`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:274`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:307`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:316`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:392`
<!-- generated:end DFX3060 -->

**Cause.** A `Settings` entry of a `DynamicMesh` block is unknown, written twice, or has the wrong shape: `TexCoords` is an integer 0-8, `Colors` true/false, `Normals` one of None/Auto/Explicit, `Bounds` a `box(...)` whose minimum is not above its maximum, `Duration` a positive number. A `Renderer` block that sets `Meshes` or `MeshBindings` is refused too -- the renderer draws this mesh and nothing else.

**Fix.** Use the documented key and shape; the message names the expected form.

## DFX3061

<!-- generated:begin DFX3061 -->
**Severity** error

**Message**

```
Material '%s': %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:282`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:289`
<!-- generated:end DFX3061 -->

**Cause.** A `Material` (on the block or on a patch) does not resolve to a material asset. The engine would draw the mesh with its default material, which is never what was meant. Lint does not look assets up, so this is reported by a build.

**Fix.** Fix the path. Every spelling a `.dfs` accepts for an asset works: `/Game/FX/M_X`, `/Game/FX/M_X.M_X`, `Plugin.Name:Materials/M_X`.

## DFX3062

<!-- generated:begin DFX3062 -->
**Severity** error

**Message**

```
Input '%s' of DynamicMesh '%s' uses a name the patch prologue already declares (U, V, I, J, NU, NV, K, NK, Rand, T, Index, Theta, Phi, Dir, the outputs, or anything starting with DM_). Rename it.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1088`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1095`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1102`
<!-- generated:end DFX3062 -->

**Cause.** An `Inputs` entry cannot be passed to the patch bodies: its name is one the generated prologue already declares (`U`, `V`, `I`, `J`, `NU`, `NV`, `K`, `NK`, `Rand`, `T`, `Index`, `Theta`, `Phi`, `Dir`, the outputs, anything starting with `DM_`), it is declared twice, or it has no value.

**Fix.** Rename it, or give it the value to pass: `float Gain = User.Gain;`. Inputs are not module inputs with defaults -- each one says what every patch receives.

## DFX3063

<!-- generated:begin DFX3063 -->
**Severity** error

**Message**

```
DynamicMesh '%s' has no Patch, so there is nothing to draw. Add one, e.g. `Patch Sheet = Grid(33, 9) { }`.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1115`
<!-- generated:end DFX3063 -->

**Cause.** The block declares no `Patch`, so the mesh would have no vertices.

**Fix.** Add a patch, e.g. `Patch Sheet = Grid(33, 9) { }` -- an empty body draws the topology's default shape.

## DFX3064

<!-- generated:begin DFX3064 -->
**Severity** error

**Message**

```
Patch '%s' has %lld vertices (%d x %d x %d copies); a patch holds at most %lld, because the generated index arithmetic is exact in float only that far. Split it into more patches.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1198`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:515`
<!-- generated:end DFX3064 -->

**Cause.** A patch has more than 16,777,216 vertices (the generated index arithmetic is exact in float only that far), or the whole mesh -- with the texture coordinate workaround multiplying the declared vertex count by `TexCoords` -- does not fit the data interface's int32 counts.

**Fix.** Split the patch into several, lower its resolution or copies, or use fewer UV channels.

## DFX3065

<!-- generated:begin DFX3065 -->
**Severity** error

**Message**

```
Patch '%s' writes '%s', but DynamicMesh '%s' has Normals = Auto, which evaluates the body four times per vertex -- the write would run four times. Keep particle state out of the body, or use Normals = Explicit and write Normal yourself.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1157`
<!-- generated:end DFX3065 -->

**Cause.** With `Normals = Auto` the patch body is evaluated four times per vertex (three samples a quarter grid step away, then the vertex itself), so a write to a `Particles.*` attribute in it would run four times, the last with the vertex's own values. Accumulating state (`+=`, `++`) would be wrong outright.

**Fix.** Keep particle state out of the body, or switch to `Normals = Explicit` and write `Normal` yourself (the topology's analytic normal is the starting value).

## DFX3066

<!-- generated:begin DFX3066 -->
**Severity** error

**Message**

```
DynamicMesh '%s' needs a plain name (letters, digits, underscores): it becomes a user parameter, emitter names and generated asset names.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1058`, `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1133`
<!-- generated:end DFX3066 -->

**Cause.** A `DynamicMesh` or `Patch` name is not a plain identifier -- typically a back-quoted name with a space or punctuation. The names become the user parameter, emitter names and generated module asset names, and an asset name cannot hold them.

**Fix.** Use letters, digits and underscores, starting with a letter: `DynamicMesh MyMesh`.

