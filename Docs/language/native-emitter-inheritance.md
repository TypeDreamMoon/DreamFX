# Native emitter inheritance

An emitter in a `.dfs` system can inherit a standalone Niagara emitter asset:

```dfs
System(Name="NS_Child", Root="Game")
{
    Emitter Sparks inherits "/Game/FX/NE_Parent.NE_Parent"
    {
        Settings = { LocalSpace = false; }
        ParticleUpdate = { Particles.MyValue = 2; }
    }
}
```

This creates a real Niagara parent association. `from "...dfe"` continues to copy and merge DreamFX emitter source; `from` and `inherits` cannot appear on the same emitter.

The parent must be an inheritable `UNiagaraEmitter`. Without a version clause, generation selects its exposed version. Pin a specific version with its GUID:

```dfs
Emitter Sparks inherits "/Game/FX/NE_Parent.NE_Parent" version "01234567-89ab-cdef-0123-456789abcdef"
{
}
```

An unknown pinned GUID is rejected with `DFX3056`, including for an emitter with versioning disabled; it never silently selects another version. If a native parent itself inherits and has unmerged ancestor changes, generation also reports `DFX3056`. Merge and save that parent in Niagara before rebuilding the child. DreamFX does not modify parent assets to resolve those changes.

## Overrides

- Omitted settings and defaults retain their parent values. Removing a source override restores the parent value on the next rebuild.
- An omitted ordinary stack inherits its parent stack. An explicit stack replaces the complete block. `ParticleSpawn = {}` deliberately clears the inherited spawn stack.
- No renderer declarations retains all parent renderers. One or more declarations replace the complete renderer group.
- No stage declarations retains all parent stages. Explicit stage declarations replace the complete stage group.
- An explicit `OnEvent` replaces the entire inherited event-handler group. The source still permits only one handler; omitting `OnEvent` retains every parent handler.

There is currently no spelling for an empty renderer/stage group or removal of an inherited default or event handler. Omitting those declarations means inheritance, not removal.

Changes to the native parent invalidate the generated system's dependency fingerprint, so a subsequent build reapplies inherited values even when the `.dfs` text is unchanged.

## Export behavior

System export preserves `inherits` and always pins the actual parent version. It compares settings, defaults, ordinary stacks and renderer groups against that version of the parent. Identical content stays implicit, including inherited modules and renderers.

Export reports a coverage gap if the child has unmerged parent changes. Merge the child in Niagara before adopting its source: otherwise a stale inherited value may be emitted as an explicit override. Invalid, disabled or unsynchronized parent assets also produce a gap rather than an apparently complete export that cannot rebuild.

A changed ordinary stack is exported as a complete block, including an empty block. A changed renderer group is exported in full. `DFX8014` explains that subsequent parent changes within these explicit blocks will be overridden when rebuilding. Event handlers and simulation stages currently export as snapshots with the same limitation; the exporter does not claim a granular native merge for these blocks.

Removing all inherited renderers or stages, removing an inherited event handler, and removing an inherited default cannot be represented. Export records these as coverage gaps and emits `DFX8014`; inspect the gaps before using the result as the asset's source of truth.

Standalone `.dfe` export still has no native-parent header syntax. It exports a snapshot and reports the loss of native inheritance with `DFX8014`. Export the owning system to `.dfs` when the parent association must be retained.
