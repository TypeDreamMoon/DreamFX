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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1696`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1707`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1257`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1298`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1351`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1415`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1478`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1848`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:923`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:965`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1017`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1280`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:948`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1792`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:306`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:893`
<!-- generated:end DFX3010 -->

**Cause.** A `Properties` entry is malformed -- see the inner message.

**Fix.** Fix the declaration. Types are listed in DFX4021's message.

## DFX3020

<!-- generated:begin DFX3020 -->
**Severity** error

**Message**

```
Unknown %s setting '%s'. Available settings: %s
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:563`
<!-- generated:end DFX3020 -->

**Cause.** A settings key that does not exist. An unchecked misspelling would do nothing at all, and the effect would simply be wrong with no sign why.

**Fix.** Use one of the names listed -- they are read from the live asset, so they are the real ones.

## DFX3021

<!-- generated:begin DFX3021 -->
**Severity** error

**Message**

```
User parameter '%s' is declared more than once.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1739`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:178`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:186`
<!-- generated:end DFX3031 -->

**Cause.** A dynamic input's return type cannot be inferred from its body.

**Fix.** Add `Output = float;` (or the type it returns).

## DFX3032

<!-- generated:begin DFX3032 -->
**Severity** error

**Message**

```
A DynamicInput's Usage must be DynamicInput, not '%s'.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:192`
<!-- generated:end DFX3032 -->

**Cause.** A `DynamicInput` document declaring a stack usage makes two statements about what it is that disagree.

**Fix.** Write `Usage = DynamicInput;`, or change the document kind to `Module`.

## DFX3033

<!-- generated:begin DFX3033 -->
**Severity** error

**Message**

```
Input '%s' is declared more than once.
```

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:203`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:218`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:224`
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

**Raised by** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:233`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1014`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:896`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:937`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1868`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1881`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1891`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1683`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:967`
<!-- generated:end DFX3044 -->

**Cause.** A module input's default is stored on the asset, so it cannot reference anything outside the module.

**Fix.** Use a literal or an enum entry. To make it caller-supplied, leave it and set it at the call site.

## DFX3045

<!-- generated:begin DFX3045 -->
**Severity** error

**Message**

```
'%s' is not a type a particle attribute can have.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:499`
<!-- generated:end DFX3045 -->

**Cause.** A particle attribute was declared in a `.dfm` body with a type that is not a Niagara value type.

**Fix.** Use one of the DSL types -- `float`, `int`, `bool`, `Vector2`, `Vector`, `Vector4`, `Color`, `Position`, `Quat`.

## DFX3046

<!-- generated:begin DFX3046 -->
**Severity** error

**Message**

```
'%s' is not a particle attribute DreamFX knows the type of. Write the type at its first use in the body -- `float %s = ...;` -- the way a .dfs declares a new attribute.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:599`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1003`
<!-- generated:end DFX3047 -->

**Cause.** A dynamic input computes a value in an input slot; it has no place in the stack to write from.

**Fix.** Move the write into a `Module`.

