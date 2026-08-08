# DreamFXLang reference

The language, split by what you are writing. For the design reasoning behind any of it — why the
stack is a statement list, why the expression whitelist is short, why `#Region` is only a comment —
see [plan.md](../plan.md) §3. This tree is the reference; plan.md is the argument.

| Page | Covers |
| --- | --- |
| [dfs.md](dfs.md) | `.dfs` — a system: user parameters, system stacks, emitters, renderers |
| [dfe.md](dfe.md) | `.dfe` — a reusable emitter, and what `from` does to it |
| [dfm.md](dfm.md) | `.dfm` — a module or dynamic input, and the MoonEngine limit on generating one |
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
- **Event handlers and named simulation stages.** `OnEvent <name> = { }` and `Stage <name> = { }`
  parse, so the syntax is settled, but neither generates (DFX2012). The 2026-08-07 coverage sweep
  found zero uses of either across the project — see [coverage-2026-08-07.md](../coverage-2026-08-07.md).
- **`MaterialParam`.** Reserved (L8), not implemented (DFX5093).
- **Anything that would need a general expression compiler.** See L6 in [values.md](values.md).
