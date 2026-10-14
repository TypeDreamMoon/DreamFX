---
title: What DreamFX is
description: What it does and does not do, and the stances that run through all of it — the text is the truth, a gap must be visible, a menu is not a second implementation.
---

DreamFX compiles text into standard Niagara assets, and decompiles standard Niagara assets back into
text. That is the whole scope — it is not a new particle runtime, and it does not replace Niagara.
What it generates is an ordinary `UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript`: any engine
loads it, any `.dfs` references it, it cooks and it runs, exactly like one wired by hand.

## Why text at all

Niagara's authoring surface is a graph and a stack. Graphs are good, right up until you need to:

- **diff a change** — who moved the spawn rate from 20 to 60? A binary asset cannot answer that;
- **edit 40 effects at once** — 40 rounds of clicking, or 40 text files?
- **review code** — a PR containing a `.uasset` shows the conclusion and none of the reasoning;
- **hand the work to an agent** — a coding agent can write text, run a build and read diagnostics. It
  cannot click a Slate button.

Text is not about being more "advanced". It is about making **version control, bulk edits and
automation** possible again.

## The stances

### The text is the truth; the asset is build output

A generated asset carries a provenance stamp: source hash, generator version, module version GUIDs.
Edit it by hand and the next rebuild wipes the edit; edit the source without rebuilding and `verify`
catches it. "The asset can always be deleted and regenerated" has to stay true, or the whole model
collapses.

### A gap must be visible, never dropped

When a decompile meets something DreamFXLang cannot express, it does **not** quietly drop it: the item
is written into the exported file's header as a gap, and reported as a `DFX8xxx` warning. `Adopt` is
stricter still — any gap at all and it refuses to take the asset over.

A tool that degrades silently leaves you staring at a wrong-looking effect three months later with
no thread to pull.

### A menu is an entry point, not a second implementation

Every button in the editor calls the same code the commandlet calls. If *Rebuild DFX* works,
`dfx build -All` works, and the other way round. Every command is reachable from Python too — a
command only a human click can reach is a command that never gets regression tested.

### A claim has to be measurable

"Decompilation is lossless" means nothing unless something can falsify it. Hence four layers: text
line by line, the mirror's compile, a fixed-step SimCache comparison at runtime, and a
reflection-walked comparison of the assets themselves. The fourth exists for a specific reason — the
earlier ones compare two outputs of the *same* exporter, so a loss that exporter makes on both sides
is symmetric and invisible to them.

## What it does not do

| | |
| --- | --- |
| **Scratch Pad** | Not expressible. A scratch pad script is a graph embedded in the asset with no stable name |
| **Lowering a module body into a node graph** | A `.dfm` body becomes one custom HLSL node. Lowering it is the ~13k-line problem DreamShader already has, reproduced |
| **GPU/CPU branch conditions, Scalability conditions** | Not covered |
| **Implicit native inheritance through `from`** | `from` copies and merges `.dfe` source on rebuild. Use [`inherits`](../language/native-emitter-inheritance.md) for a native Niagara parent relationship |
| **A general expression compiler** | Inline expressions are arithmetic plus a short whitelist. Anything more belongs in `hlsl { }` or a `.dfm` |

The full list, plus the 1.0.0 known issues, is in [limits](../diagnostics/limitations.md).

## Its relationship to DreamShader

[DreamShaderLang](https://lang.64hz.cn) is the sister project from the same author, doing the same
thing for materials: `.dsm` / `.dsf` / `.dsh` generate `UMaterial` and `UMaterialFunction` assets.

The two share their stances (text is the source, diagnostics carry positions, the editor integration
runs the same pipeline) but **share no code**, and the grammars are not interchangeable. One visible
fork in the road: DreamShader has a full expression compiler and DreamFX deliberately does not —
Niagara's module ecosystem already solved "computing things", so DreamFX's job is to **wire modules
together**, not to reinvent operators.

## Names and versions

| | |
| --- | --- |
| Plugin | DreamFX `1.0.0` |
| Language | DreamFXLang |
| Modules | `DreamFX` (Runtime), `DreamFXEditor` (Editor) |
| Commandlet | `-run=DreamFX` |
| Drivers | `.skill/dfx.ps1`, `.skill/ci.ps1` |
| Log category | `LogDreamFX` |
| Author | TypeDreamMoon · MIT |
