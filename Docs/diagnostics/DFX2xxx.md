# DFX2xxx --- Lexer and syntax

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX2000

<!-- generated:begin DFX2000 -->
**Severity** error

**Message**

```
Unknown document type '%s'. Expected System, Emitter, Module or DynamicInput.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1728`
<!-- generated:end DFX2000 -->

**Cause.** The first word of the file is not one of the four document kinds.

**Fix.** Write `System`, `Emitter`, `Module` or `DynamicInput`, matching the file extension.

## DFX2001

<!-- generated:begin DFX2001 -->
**Severity** error

**Message**

```
Expected '{' to open a raw block but found '%s'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:399`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:87`
<!-- generated:end DFX2001 -->

**Cause.** A construct that takes a raw block was not followed by `{`.

**Fix.** Add the brace. `hlsl` and `Body` are always `hlsl { ... }` / `Body = { ... }`.

## DFX2002

<!-- generated:begin DFX2002 -->
**Severity** error

**Message**

```
Expected a name but found %s.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:98`
<!-- generated:end DFX2002 -->

**Cause.** An identifier was expected -- a module name, an input name, an emitter name.

**Fix.** Check for a stray punctuation mark before the position reported.

## DFX2003

<!-- generated:begin DFX2003 -->
**Severity** error

**Message**

```
Expected a number but found '%s'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:390`
<!-- generated:end DFX2003 -->

**Cause.** A number was expected, inside a vector literal, a `box()` or a curve key.

**Fix.** Vector components and curve times are numbers only; a parameter reference cannot appear there.

## DFX2004

<!-- generated:begin DFX2004 -->
**Severity** error

**Message**

```
Expected a value but found %s.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:577`
<!-- generated:end DFX2004 -->

**Cause.** A value was expected after `=`.

**Fix.** Something is missing on the right-hand side, or the previous statement is missing its `;`.

## DFX2005

<!-- generated:begin DFX2005 -->
**Severity** error

**Message**

```
'#EndRegion' has no matching '#Region'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:785`
<!-- generated:end DFX2005 -->

**Cause.** `#EndRegion` appeared with no open `#Region`.

**Fix.** Delete it, or add the opening `#Region "..."`.

## DFX2006

<!-- generated:begin DFX2006 -->
**Severity** error

**Message**

```
Unknown directive '#%s'. Expected Region or EndRegion.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:795`
<!-- generated:end DFX2006 -->

**Cause.** The only directives are `#Region` and `#EndRegion`.

**Fix.** Check the spelling. `#Region` is a comment device (L5) and never reaches the asset.

## DFX2007

<!-- generated:begin DFX2007 -->
**Severity** error

**Message**

```
Expected a version after '@'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:456`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:477`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:855`
<!-- generated:end DFX2007 -->

**Cause.** `@` introduces an R7 version pin and must be followed by a version.

**Fix.** Write `ModuleName@1.2`, or drop the `@`.

## DFX2008

<!-- generated:begin DFX2008 -->
**Severity** error

**Message**

```
Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:874`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:971`
<!-- generated:end DFX2008 -->

**Cause.** A module call was given a bare value. Niagara addresses module inputs by name, and the order they appear in the stack UI is not the order they are declared in.

**Fix.** Write `Name = Value` for every argument. `dfx schema <Module>` lists the names.

## DFX2010

<!-- generated:begin DFX2010 -->
**Severity** warning

**Message**

```
'#Region \"%s\"' was never closed with '#EndRegion'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:959`
<!-- generated:end DFX2010 -->

**Cause.** A `#Region` ran to the end of the block without an `#EndRegion`. Harmless -- regions are text -- but usually means a section was moved and its closer left behind.

**Fix.** Add the `#EndRegion`.

## DFX2013

<!-- generated:begin DFX2013 -->
**Severity** error

**Message**

```
'%s' is a system-scope stack and must be written at the top level of a .dfs, not inside an emitter.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1149`
<!-- generated:end DFX2013 -->

**Cause.** `SystemSpawn` and `SystemUpdate` belong to the system, not to an emitter (L1).

**Fix.** Move the block to the top level of the `.dfs`, beside the `Emitter` blocks.

## DFX2015

<!-- generated:begin DFX2015 -->
**Severity** error

**Message**

```
'from' must be followed by a quoted path to a .dfe source file.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1471`
<!-- generated:end DFX2015 -->

**Cause.** `from` takes a quoted path to a `.dfe`.

**Fix.** Quote it. The extension is optional; the path resolves relative to this file first.

## DFX2016

<!-- generated:begin DFX2016 -->
**Severity** error

**Message**

```
(built at runtime)
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1093`
<!-- generated:end DFX2016 -->

**Cause.** A `Defaults = { … }` block contained something other than an assignment. A default says what reading a parameter produces when nothing set it, so a module call has nothing to mean there.

**Fix.** Move the module call into one of the stacks (`ParticleSpawn`, `ParticleUpdate`, …). Only `<type> Namespace.Name = value;` belongs in `Defaults`.

## DFX2018

<!-- generated:begin DFX2018 -->
**Severity** error

**Message**

```
A Module or DynamicInput must declare a 'Body = { }' block.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1638`
<!-- generated:end DFX2018 -->

**Cause.** A `.dfm` with no `Body` declares inputs and generates a module that does nothing.

**Fix.** Add `Body = { ... }`.

## DFX2019

<!-- generated:begin DFX2019 -->
**Severity** error

**Message**

```
Header argument '%s' must be a quoted string.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1665`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1682`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1690`
<!-- generated:end DFX2019 -->

**Cause.** Header arguments (`Name=`, `Root=`) are quoted strings.

**Fix.** Quote the value.

## DFX2020

<!-- generated:begin DFX2020 -->
**Severity** error

**Message**

```
Unknown curve key attribute '%s'. Expected Interp, Arrive or Leave.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:361`
<!-- generated:end DFX2020 -->

**Cause.** A curve key's attribute list accepts only `Interp`, `Arrive` and `Leave` (plan 3.5).

**Fix.** Check the spelling. Tangents are only honoured when `Interp` makes them meaningful.

## DFX2021

<!-- generated:begin DFX2021 -->
**Severity** error

**Message**

```
File declares '%s' but its extension is '%s'. Rename the file to '%s' or change the declaration.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1742`
<!-- generated:end DFX2021 -->

**Cause.** The declared document kind and the file extension disagree.

**Fix.** Rename the file or change the declaration -- the extension is what the build enumerates by.

## DFX2022

<!-- generated:begin DFX2022 -->
**Severity** error

**Message**

```
Unexpected '%s' after the end of the document. A DreamFX file declares exactly one top-level object.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1771`
<!-- generated:end DFX2022 -->

**Cause.** Content followed the closing brace of the top-level object. Usually a duplicated block or one brace too few somewhere above.

**Fix.** One file, one top-level object. The position reported is where the extra content starts, not where the imbalance is.

## DFX2023

<!-- generated:begin DFX2023 -->
**Severity** error

**Message**

```
'%s' is a module call, so it cannot be given a type. Types are written only on assignments.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:915`
<!-- generated:end DFX2023 -->

**Cause.** A type was written in front of a module call. Types annotate assignments, where L2 has to know what a new attribute is; a module call's types come from its schema.

**Fix.** Drop the type.

## DFX2024

<!-- generated:begin DFX2024 -->
**Severity** error

**Message**

```
'disabled' can only prefix a module call, and '%s' is an assignment.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:926`
<!-- generated:end DFX2024 -->

**Cause.** `disabled` prefixes a *module call*, and the statement it was written on is an assignment.

An assignment has nothing to disable: it is folded into the stack's own Set Parameters module alongside every other assignment in that stack, and turning that module off would silently drop all of them.

**Fix.** Delete the `disabled` and comment the line out instead, or move the assignment into a module call that can be parked as a whole.

## DFX2025

<!-- generated:begin DFX2025 -->
**Severity** error

**Message**

```
Unknown OnEvent argument '%s'. Expected Source, Event, Mode, SpawnNumber, MaxEventsPerFrame, UpdateAttributeInitialValues, RandomSpawnNumber or MinSpawnNumber.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1307`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1315`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1330`
<!-- generated:end DFX2025 -->

**Cause.** An `OnEvent(...)` header carries an argument the parser does not know, an argument is missing its value, or a required argument (`Source`, `Event`) was left out entirely.

The header is not a module call — its arguments configure the *handler itself* (which emitter's events it listens to and how it spends them), so the accepted set is fixed: `Source` (emitter name in this system), `Event` (the event name, quoted), `Mode` (`SpawnedParticles` or `EveryParticle`), `SpawnNumber`, `MaxEventsPerFrame`, `UpdateAttributeInitialValues`, `RandomSpawnNumber`, `MinSpawnNumber`. Modules that *react* to the event go inside the block, not in the header.

**Fix.** Spell the argument as listed above (matching ignores case), give every argument a value of the right shape — `Source` and `Mode` are identifiers, `Event` a name or string, the numbers integers, the flags `true`/`false` — and always write at least `Source` and `Event`:

```
OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 1) = {
    ReceiveLocationEvent();
}
```

## DFX2026

<!-- generated:begin DFX2026 -->
**Severity** error

**Message**

```
Unknown Stage argument '%s'. Expected Iteration, DataInterface, NumIterations or Enabled.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1435`, `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1443`
<!-- generated:end DFX2026 -->

**Cause.** A `Stage name(...)` header carries an argument the grammar does not know, or one whose
value has the wrong shape. The four arguments are `Iteration` (an identifier naming an
`ENiagaraIterationSource` entry: `Particles`, `DataInterface`, `DirectSet`), `DataInterface` (the
bound grid's dotted name, as a string or bare identifiers), `NumIterations` (an integer) and
`Enabled` (`true`/`false`). Everything is optional — a bare `Stage name = { }` is an enabled
particles-iteration stage that runs once.

**Fix.** Spell the argument as the list above; a stage property beyond these four (execute
behaviour, the state-iteration trio) has no text form yet and cannot be requested here.

