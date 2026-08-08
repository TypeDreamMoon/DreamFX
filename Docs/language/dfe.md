# `.dfe` — a reusable emitter

A `.dfe` holds one `Emitter` block, written exactly as it would be inline in a `.dfs`:

```cpp
Emitter(Name="Emitters/E_MoonFlashCard", Root="Plugin.DreamFX")
{
    Settings = { SimTarget = CPU; LocalSpace = true; QualityLevelMask = 31; }

    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.12);
        SpawnBurst_Instantaneous(SpawnCount = 1);
    }

    ParticleSpawn  = { ... }
    ParticleUpdate = { ... }

    SpriteRenderer Card { ... }
}
```

`QualityLevelMask` is the scalability bitmask, which lives inside the emitter's `FNiagaraPlatformSet`
rather than at the top level. Settings whose property sits inside a struct are spelled with a dotted
path internally; the DSL name is the leaf, because that is the part an author sets.

A `.dfe` generates nothing on its own (DFX5097). It exists to be pulled into a system:

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

The path resolves relative to the referencing file first, then against every DFX root. The extension
is optional.

## Copy, not inheritance

`from` takes a snapshot (R3). Editing the `.dfe` afterwards changes nothing in systems that already
copied it until they are rebuilt, and the generated asset carries no link back.

That decision is also why a decompiled system is always self-contained: once the copy is made, "which
of these were overrides" is not recoverable, so the export cannot reconstruct the `from`. It is
one-way syntax sugar.

## What the override block replaces

The merge granularity is the *block*, not the module:

| Part | Merge |
| --- | --- |
| `Settings` | per key — names are unambiguous, so an override replaces just that setting |
| each stack | **whole block replaced** if the host declares it; otherwise the `.dfe`'s is kept |
| renderers | **all or nothing** — declaring any renderer replaces every one |

Stacks are replaced wholesale because per-module merging needs a module identity the language does not
have: two calls to the same module in one stack are indistinguishable, so a merge would have to guess,
and guessing wrong silently reorders somebody's effect.

Renderers are all-or-nothing for the same reason from the other side: they are addressed by
declaration order, so replacing one of three would silently renumber the other two.

## User parameters

A `.dfe` may read `User.*`. The host system has to declare those parameters, and the check happens at
the `from` line rather than inside the `.dfe` — that is where the decision to pull it in was made
(DFX3043).

```
NS_Host.dfs(7,5): error DFX3043: '../Emitters/E_Flash' reads user parameters this system does not
declare: User.FlashCount. Add them to the Properties block.
```
