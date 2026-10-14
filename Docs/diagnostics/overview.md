---
title: Diagnostics
description: Every DFXnnnn code — how to read one, which pipeline stage each family belongs to, and links to the generated code catalog.
---

Every DreamFX diagnostic is a `DFXnnnn` carrying a **file, line and column**:

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

| | |
| --- | --- |
| Log category | `LogDreamFX` |
| Severities | `error` / `warning` / `info` — all three are really used |
| Position | every code carries file, line and column |
| In the editor | a failed build toasts with an **Open in VSCode** link straight to the line and column |

**The leading digit is the stage that raised it.** That is both a reading cue and a pointer at where
to look:

| Range | Stage | Means |
| --- | --- | --- |
| [`DFX1xxx`](DFX1xxx.md) | Driver and file I/O | the file could not be read, or the lexer stopped |
| [`DFX2xxx`](DFX2xxx.md) | Lexer and syntax | the grammar: braces, semicolons, keywords |
| [`DFX3xxx`](DFX3xxx.md) | Declarations and structure | the syntax is fine, but a declaration disagrees with the real asset |
| [`DFX4xxx`](DFX4xxx.md) | Values, types and expressions | something on the right of an `=` |
| [`DFX5xxx`](DFX5xxx.md) | Generation and asset writing | writing the asset failed, or a deliberate degradation is being announced |
| [`DFX6xxx`](DFX6xxx.md) | Niagara compilation | the asset was written and Niagara itself objects |
| [`DFX7xxx`](DFX7xxx.md) | Provenance, drift and lint | the asset and the source are out of step, or lint sees a hazard |
| [`DFX8xxx`](DFX8xxx.md) | Decompiler | export, Adopt, round-trip gaps |

> **Note:**
>
> Half of these pages are **machine-written**: severity, message template and raise sites are scanned
> out of the plugin sources, and `gen-diagnostics.ps1 -Check` is the gate that stops them drifting.
> The human half — cause and fix — is written once. See [ci.ps1 and gates](../tools/ci.md).

## The order to debug in


### Look the code up
Use the [generated code index](README.md) or a family page below. Codes are stable: `DFX3003` is always the same failure mode.

### Check against the real signature
`dfx.ps1 schema <Module> -Stack <Stack>` prints the input signature as the build sees it — static
switches included. The authority on names is the asset, not memory.

### Still wrong? Read the limits
[Limits](limitations.md) lists what is not covered and the 1.0.0 known issues.


## Lookup by code

The [complete code index](README.md) is generated from the current plugin sources. It owns the
severity and message table; this guide links to that catalog instead of maintaining a second copy.

### DFX1xxx — Driver and file I/O

Read the [DFX1xxx reference](DFX1xxx.md) for current messages, causes and fixes.

### DFX2xxx — Lexer and syntax

Read the [DFX2xxx reference](DFX2xxx.md) for current messages, causes and fixes.

### DFX3xxx — Declarations and document structure

Read the [DFX3xxx reference](DFX3xxx.md) for current messages, causes and fixes.

Older links to [DFX3045](history/DFX3045.md#dfx3045) remain available as a historical entry.

### DFX4xxx — Values, types and expressions

Read the [DFX4xxx reference](DFX4xxx.md) for current messages, causes and fixes.

### DFX5xxx — Generation and asset writing

Read the [DFX5xxx reference](DFX5xxx.md) for current messages, causes and fixes.

Older links to [DFX5099](history/DFX5099.md#dfx5099) explain the former grouping limitation.

### DFX6xxx — Niagara compilation

Read the [DFX6xxx reference](DFX6xxx.md) for current messages, causes and fixes.

### DFX7xxx — Provenance, drift and lint

Read the [DFX7xxx reference](DFX7xxx.md) for current messages, causes and fixes.

### DFX8xxx — Decompiler

Read the [DFX8xxx reference](DFX8xxx.md) for current messages, causes and fixes.
