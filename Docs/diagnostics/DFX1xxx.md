# DFX1xxx --- Driver and file I/O

> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.
> Everything below a marker is written by hand and survives a regeneration.

## DFX1000

<!-- generated:begin DFX1000 -->
**Severity** error

**Message**

```
Could not read source file '%s'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1485`
<!-- generated:end DFX1000 -->

**Cause.** The path does not exist, or the process cannot read it. A `from` reference and a `-File=` argument both land here.

**Fix.** Check the path. Relative `from` paths resolve against the referencing file first, then against every DFX root.

## DFX1001

<!-- generated:begin DFX1001 -->
**Severity** error

**Message**

```
Unterminated string literal.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:304`
<!-- generated:end DFX1001 -->

**Cause.** A quoted string ran to the end of the line without a closing quote.

**Fix.** Close the quote. DreamFX strings do not span lines.

## DFX1002

<!-- generated:begin DFX1002 -->
**Severity** error

**Message**

```
Unterminated block comment.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:127`
<!-- generated:end DFX1002 -->

**Cause.** A `/*` was never closed.

**Fix.** Add the `*/`. The reported position is where the comment started, not where the file ended.

## DFX1003

<!-- generated:begin DFX1003 -->
**Severity** error

**Message**

```
Unexpected character '%c' (U+%04X).
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:335`
<!-- generated:end DFX1003 -->

**Cause.** A character that is not part of the language appeared outside a string, comment or raw block. Smart quotes pasted from a document are the usual cause.

**Fix.** Delete it. The code point is printed so a look-alike character is identifiable.

## DFX1004

<!-- generated:begin DFX1004 -->
**Severity** error

**Message**

```
Unterminated raw block: missing '}'.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:488`
<!-- generated:end DFX1004 -->

**Cause.** A raw block -- `hlsl { }`, `Body = { }` -- was opened and never closed. Raw blocks are brace-balanced, so an unbalanced brace *inside* the HLSL runs the block to end of file.

**Fix.** Balance the braces inside the block.

## DFX1005

<!-- generated:begin DFX1005 -->
**Severity** error

**Message**

```
Unterminated back-quoted name. A `name` must close on the same line.
```

**Raised by** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:185`, `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:198`
<!-- generated:end DFX1005 -->

**Cause.** A back-quote opened a name that never closed before the end of the line, or closed
immediately with nothing between the quotes.

A back-quoted name is how the language holds a Niagara name it could not otherwise spell —
`` `PillarPower(0~1)` ``, `` `Ring/DiscDistributionMode` ``. Names never span lines, so a missing
closing quote is reported at the end of the line rather than swallowing the rest of the file.

**Fix.** Close the quote. If the name itself contains a back-quote there is no way to write it: the
escape would need an escape, and nothing in Niagara produces one — rename the parameter instead.

Back-quotes are only needed when the name is not already an identifier. `` `Speed` `` is legal but
noise; write `Speed`. The decompiler applies exactly that rule, so a re-export is the quickest way to
see which names need quoting.

