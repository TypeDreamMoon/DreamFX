---
title: Agent skills
description: The four skills that ship with the plugin, and the "read the schema first" loop an agent has to follow when writing DreamFXLang.
---

The plugin ships four agent skills under
[`.skill/`](https://github.com/TypeDreamMoon/DreamFX/tree/main/.skill). They are instructions for a
coding agent (Claude Code and the like): how to author, build, debug and migrate effects, all
headlessly.

| Skill | What it does |
| --- | --- |
| `dream-fx-create` | Writes a new `.dfs` / `.dfe` / `.dfm` from a plain-language description, then **builds it to prove it compiles** |
| `dream-fx-verify` | Builds or checks headlessly: one file, or the whole DFX tree. The harness the other three call |
| `dream-fx-diagnose` | Takes a `DFXnnnn`, looks up the cause, explains it, fixes the source |
| `dream-fx-decompile` | Exports an existing Niagara system to source, or answers "how much of this project's VFX is expressible" |

## The step everyone skips

`dream-fx-create` is a five-step loop, and step 2 is the one that matters:


### Find the modules
`dfx.ps1 list` prints every module the search paths expose.

### Read their real signatures
`dfx.ps1 schema <Module>` — **once per module you intend to call.**

This is the step people skip and then spend twenty minutes on. Module input names are read off real
assets: names with spaces, inputs revealed by a static switch, inputs that only exist in one stack.
Nobody — including a model — can guess them reliably.

### Write the file
Under a `DFX/` root.

### Build it
`dfx.ps1 build <file> -Force`.

### Fix and repeat
Diagnostics carry line and column; look the code up in [diagnostics](../diagnostics/overview.md).


> **Note:**
>
> **The proving is not optional.** "It looks right" means nothing here — only a green build says the
> module names, the input names, the types and the switch ordering are all correct. That is why one
> of the four skills is nothing but a build gate.

## Wiring them into your own agent

The skills are plain text plus `dfx.ps1`, with no other dependency. To use them in your project:

1. let the agent run `pwsh`;
2. make the engine resolvable (`UE_ENGINE_ROOT` or a registered `EngineAssociation`);
3. **package-writing commands need the editor closed** — make sure the agent knows;
4. drop the four `SKILL.md` files from `.skill/` into your agent's skills directory.

## Why this language suits agents

Not because of the era — because of three concrete things:

- **The text is the only authoring surface.** An agent can write text; it cannot click a Slate
  button.
- **Diagnostics carry a stable code and a position.** `DFX3003` is always the same failure mode, and
  the line and column point at the token — a feedback channel a program can consume, rather than a
  paragraph of prose.
- **The build is headless and has an exit code.** "Is it right" has an answer that does not need a
  pair of eyes.

All three are just as good for humans. Agents only make the requirement — that the feedback channel
be machine-readable — harder to ignore.
