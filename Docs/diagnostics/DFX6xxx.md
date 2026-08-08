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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1894`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1898`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1939`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1943`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2202`
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

**Raised by** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1243`
<!-- generated:end DFX6006 -->

**Cause.** A `.dfm`'s body is not valid HLSL once lowered. The Niagara message follows.

**Fix.** Read the translator error. `Cannot access field 'X' of structure` means a namespaced reference did not resolve -- `Particles.*` has to be a known attribute or declared with a type in the body (DFX3046).

