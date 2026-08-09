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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2750`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2772`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2813`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2817`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2902`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:3190`
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
This build has no Niagara fast-edit API (DREAMFX_HAS_NIAGARA_FAST_EDIT=0), so the generator could not set that parameter's default mode to Value, and Niagara rejects reading it before something writes it. Two ways out: write the parameter earlier in the same stack than whatever reads it -- for an 'Emitter.<Module>.<Output>' name that means moving the module that produces it ahead of its readers -- or build against an engine carrying the MoonEngine Niagara additions. If nothing writes it anywhere, the read is a genuine defect that the other engine was hiding.
```

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2765`
<!-- generated:end DFX6007 -->

**Cause.** Attached to a DFX6001 "read before being set", and only on a build without the fast-edit API. A parameter a link writes gets an entry whose default mode is `FailIfPreviouslyNotSet`, and setting that mode to `Value` needs an engine call this build does not have --- so Niagara refuses a read that would otherwise have compiled. The DFX6001 is real either way; this note exists because the *reason* differs from engine to engine, and without it the same source looks broken on one machine and fine on another.

**Fix.** Reorder so the write happens before the read: for an `Emitter.<Module>.<Output>` name, move the module that produces it above its readers in the stack. Building against MoonEngine also clears it. If nothing writes the parameter anywhere in the system, neither will help --- the read is a genuine defect that the other engine's default was covering.

**Not** a reason to distrust the build. It is an explanation attached to an error, never an error of its own.

