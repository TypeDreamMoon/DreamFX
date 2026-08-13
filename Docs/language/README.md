# DreamFXLang reference

The language, split by what you are writing. Where a rule exists because the engine forced it —
why the stack is a statement list, why the expression whitelist is short, why `#Region` is only a
comment — the page says so where the rule is stated.

| Page | Covers |
| --- | --- |
| [dfs.md](dfs.md) | `.dfs` — a system: user parameters, system stacks, emitters, renderers |
| [dfe.md](dfe.md) | `.dfe` — a reusable emitter, and what `from` does to it |
| [dfm.md](dfm.md) | `.dfm` — a module or dynamic input, and which engines can generate one |
| [values.md](values.md) | Every value position, the type rules, and the eight `L*` rules by name |

## Shape of every file

One top-level object, declared with the same header everywhere:

```cpp
<Kind>(Name="<path under the root>", Root="<root token>")
{
    ...
}
```

`Kind` is `System`, `Emitter`, `Module` or `DynamicInput`, and has to agree with the file extension
(DFX2021). `Root` is `Game`, empty (the same thing), or `Plugin.<PluginName>`; `Name` is the asset
path relative to that root's content directory.

Blocks are `Name = { ... }`, statements end in `;`, and `//` and `/* */` are comments.
Attributes hang off a declaration in brackets: `[ Group="Burst"; SortPriority=10 ]`.

## What is deliberately not here

- **Emitter inheritance.** `from` is copy, not inherit (R3). Editing a `.dfe` does not reach back into
  systems that already copied it until they are rebuilt.
- **`MaterialParam`.** Reserved (L8), not implemented (DFX5093).
- **Anything that would need a general expression compiler.** See L6 in [values.md](values.md).
