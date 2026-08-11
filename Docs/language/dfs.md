# `.dfs` — a system

Produces one `UNiagaraSystem`.

```cpp
System(Name="Effects/NS_Spark", Root="Game")
{
    Settings   = { ... }        // system properties
    Properties = { ... }        // user parameters (User.*)

    SystemSpawn  = { ... }      // system-scope stacks (L1)
    SystemUpdate = { ... }

    Emitter <Name> { ... }              // inline
    Emitter <Name> from "<path>" { ... } // copied from a .dfe, then overridden
}
```

## `Settings`

Schema-driven: the names are read off the live asset, so a misspelling reports the real list
(DFX3020). Common ones:

```cpp
Settings = {
    EffectType  = "/Niagara/Default/FX_Default.FX_Default";
    WarmupTime  = 0.0;
    FixedBounds = box(-200, -200, -50, 200, 200, 300);
    FixedTickDelta     = true;      // substep at a fixed rate instead of once per frame
    FixedTickDeltaTime = 0.01667;   // ... of 60Hz. Changes simulation, not just smoothness
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

`ModulePaths` is DreamFX's own, not Niagara's: it adds search roots for resolving module short names
(L4). The engine defaults (`/Niagara/Modules`, `/Niagara/DynamicInputs`, `/Niagara/Functions`) stay on
the list, so declaring your own folder adds to them rather than replacing them.

## `Properties` — user parameters

```cpp
Properties = {
    int              SparkCount = 24                     [ Group="Burst"; SortPriority=10 ];
    float            SparkSpeed = 450.0;
    Color            TintA      = (1.0, 0.72, 0.25, 1.0);
    Vector           HitNormal  = (0, 0, 1)              [ Description="Impact normal from blueprint" ];
    Texture2D        NoiseTex   = "Plugin.MoonToon:Textures/T_Noise01";
    DI<SkeletalMesh> TargetMesh;
}
```

Each becomes `User.<Name>`, settable from blueprint with `SetNiagaraVariable*`. The name is a stable
key across rebuilds (plan 4.5), so renaming one breaks every blueprint that referenced it.

`Description` reaches the asset. `Group` and `SortPriority` do not — the external edit API's user
variable struct has no metadata fields for them, which the build says once as DFX5099. They stay in
the source as documentation.

Data interface parameters are declared only; v1 does not configure them from text and does not apply a
default (DFX5098). Feed them at runtime. The single exception is `curve { }`, which is really
configuring a curve data interface — see [values.md](values.md).

## Stacks

Six stacks, two at system scope and four per emitter (L1), plus a per-emitter event stack declared
with `OnEvent` and any number of simulation stage stacks declared with `Stage` (both below):

```
SystemSpawn      SystemUpdate           <- top level of the .dfs
EmitterSpawn     EmitterUpdate          <- inside an Emitter block
ParticleSpawn    ParticleUpdate
```

Writing order is module order. Two statement forms (L2):

```cpp
ParticleUpdate = {
    GravityForce(Gravity = (0, 0, -680));      // module call
    Particles.Moon.Seed = 0.5;                 // assignment
    Particles.Moon.Tint = User.TintA;          // ...folds into the same Set Parameters module
    SolveForcesAndVelocity();                  // ...and this call ends the run
}
```

**Consecutive assignments fold into one Set Parameters module**, and a module call breaks the run.
That rule is what makes the round trip symmetric: one Set Parameters module exports as one block of
assignments.

A stack you do not declare is left alone, and the build says what it kept (DFX5003). This matters for
`SystemUpdate`, which a new system gets a `SystemState` in — clearing every undeclared stack would
make each `.dfs` without an explicit `SystemUpdate` produce a system that never runs. To take a stack
over and empty it, declare it empty: `SystemUpdate = { }`.

An emitter's four stacks are not subject to this: DreamFX builds emitters with no default modules, so
they start empty either way.

### Module calls

```cpp
ModuleName(Input = Value, Input = Value);
Spawn/Initialization/V2/InitializeParticle(...);   // partial path, when the short name is ambiguous
ModuleName@1.2(...);                               // R7 version pin
disabled GravityForce(Gravity = (0, 0, -980));     // in the stack, not executed
```

Arguments are always named (DFX2008). Input names are normalised — Niagara's `Loop Duration` is
written `LoopDuration`.

`disabled` parks a module without deleting it: it stays in the stack, keeps its inputs, and does not
run. That is Niagara's own "keep it but turn it off" state, and keeping the inputs is the whole reason
to use it rather than commenting the line out. It prefixes a module call only — on an assignment it is
DFX2024, because an assignment is folded into the stack's shared Set Parameters module and disabling
that would drop every other assignment beside it.

**Static switches gate other inputs, and on a module source order is write order.** An input that only
exists once a switch is set has to be written after it:

```cpp
EmitterState(
    LifeCycleMode = Self,        // gates everything below
    LoopBehavior  = Once,
    LoopDuration  = 0.15
);
```

On a **dynamic input** the rule is relaxed: switches are hoisted and written first whatever order they
appear in, so this works either way round.

```cpp
VelocityStrength = RandomRangeFloat(
    Minimum        = User.Speed * 0.5,
    Maximum        = User.Speed,
    RandomnessMode = SimulationDefaults   // a switch, written before the two above
)
```

`dfx schema <Module> -Stack <Stack>` prints the signature as the build sees it, static switches
included.

`@1.2` records which version the source was written against and errors if the asset now exposes a
different one (DFX3009). It cannot *select* a version — the external edit API has no way to
(plan-v2 W3).

### Assignments

```cpp
Particles.Moon.Seed        = 0.5;          // first write declares the attribute (L2)
Color Particles.Color      = hlsl { ... }; // a type, when the value carries none
Emitter.MyCounter          = 0;
```

The target is namespace-qualified (DFX4025). The type comes from the value; an `hlsl` block, a dynamic
input and an inline expression all have none, so those need the annotation (DFX4022).

### `OnEvent` — reacting to another emitter's events

```cpp
Emitter Receiver
{
    ParticleSpawn = { … }

    OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = {
        ReceiveLocationEvent();        // ordinary stack statements; runs per received event
    }
}
```

The header configures the handler, the block is a stack like any other. `Source` names an emitter in
this system, `Event` the event it generates (a `GenerateLocationEvent()` module sends
`"LocationEvent"`), and the rest are the handler's spend controls, written only when they differ from
the defaults: `Mode` (`SpawnedParticles` spawns `SpawnNumber` new particles per event and runs the
block on them; `EveryParticle` runs it on the existing ones), `MaxEventsPerFrame`,
`UpdateAttributeInitialValues`, `RandomSpawnNumber` with `MinSpawnNumber`. `Source` and `Event` are
required (DFX2025).

One `OnEvent` block per emitter (DFX5031): DreamFX addresses the event stack through the same rails as
the other four, and those can reach exactly one handler. An emitter that should react to two event
streams becomes two emitters.

An event *generator* usually needs `RequiresPersistentIDs = true` in the source emitter's `Settings`,
because it reads `Particles.ID`; without it the build reports the engine's own stack issue (DFX6003).

### `Stage` — simulation stages

```cpp
Emitter Fluid
{
    Settings = { SimTarget = GPU; }

    ParticleUpdate = { … }

    Stage Settle = {                     // a bare stage: enabled, particle iteration, runs once
        ScaleColor();
    }

    Stage Project(DataInterface = "Emitter.PressureGrid", NumIterations = 10) = {
        …                                // ordinary stack statements; runs per grid cell, ten times
    }
}
```

A stage is a particle stack that runs after `ParticleUpdate`, on a GPU emitter (`SimTarget = GPU`;
a CPU emitter refuses stages at compile time). Declaration order is run order, and each block is a
stack like any other — module calls, folded assignments, `disabled`, all of it.

The header holds the stage's own properties, written only when they differ from a freshly added
stage: `DataInterface` names the grid parameter the stage iterates over (one thread per cell rather
than per particle) and already implies the iteration source; `Iteration` says it explicitly
(`Particles`, `DataInterface`, `DirectSet`) for the shapes that need it, such as a
data-interface iteration with nothing bound; `NumIterations` repeats the stage (default 1);
`Enabled = false` parks it. Unknown or misshapen arguments are DFX2026. Two `Stage` blocks with one
name is DFX5032 — stages are identified by name, and the build would otherwise quietly keep
whichever stack ran last.

The `DataInterface` parameter must exist on the emitter by the time the stage's modules have been
written — in practice the modules that use the grid also link it, which creates it. A count driven
by a *parameter* rather than a number is not representable; the export says so and the rebuilt
stage keeps the default count. A stage of a custom C++ stage class (anything that is not the
engine's generic stage) stays a gap with its own header line (DFX8016).

## `Defaults` — what a read produces when nothing wrote

> Working since 2026-08-09: the ordering fix (defaults applied after the stacks, with an implied
> Value pass) closed what [roundtrip-2026-08-08-3.md](../roundtrip-2026-08-08-3.md) measured as dead
> ends — the writes were creating the entry they then refused to fill. The decompiler exports a
> `Defaults` block when an emitter graph carries one that differs from what a fresh build produces.

```cpp
Emitter Sparks
{
    Defaults = {
        float Particles.MySize = 1.0;             // a value
        Vector Particles.Home  = Engine.Owner.Position;   // a binding
    }

    ParticleSpawn = { … }
}
```

An assignment says *this parameter is now this*. A default says *if nothing set it, reading it gives
this* — Niagara's `DefaultMode`, and the difference between a read that compiles and one that does
not. Entries are assignments with a declared type (DFX4028); the value must be a literal, an enum or
another parameter, because a default cannot compute per particle (DFX4029).

## Renderers

```cpp
SpriteRenderer Core
{
    Material     = "Plugin.MoonToon:Materials/FX/M_SparkSprite";
    Alignment    = VelocityAligned;
    FacingMode   = FaceCamera;
    SortMode     = ViewDepth;
    SubImageSize = (2, 2);

    Bind SpriteSize -> Particles.SpriteSize;
    Bind Color      -> Particles.Color;
}
```

Properties are schema-driven (L8): every renderer type gets its whole property set with no per-type
syntax, and an unknown name reports the real list. Only the type keyword is a closed set (DFX3004).

A property that holds a *list* of assets — `Meshes` and `OverrideMaterials` on a mesh renderer — is
written as an array of paths:

```cpp
MeshRenderer Body
{
    Meshes           = ["/Engine/BasicShapes/Cube"];
    OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
}
```

Each element is really a struct with the asset as one field inside it; which field is found by
reflection, so this works for renderer types that do not exist yet. The struct's *other* fields — a
mesh's per-element pivot, scale and LOD range — have no syntax. An export that would have dropped one
says so in the file header rather than flattening it away.

`Bind` is separate from property assignment because attribute bindings are not plain fields — the
binding struct caches a display name, a data-set name and source-mode flags that only its own
`SetValue` recomputes, so writing the serialised field would leave half a binding behind.

**Declaration order is renderer order**, and there is no other addressing scheme. Reordering two
renderer blocks repaints the effect.

Leaving `Material` out applies the engine default (DFX5004) rather than drawing nothing.

## Referencing a `.dfe`

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

See [dfe.md](dfe.md) for what the merge does and does not do.
