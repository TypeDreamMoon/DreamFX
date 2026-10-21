# `DynamicMesh` — procedural geometry written by the simulation

A `DynamicMesh` block in a `.dfs` declares a mesh whose vertices a GPU simulation computes every frame:
its topology (grids of quads, in patches), its material and vertex layout, and one HLSL program per
patch that says where each vertex is and what it carries. The build lowers the block into ordinary
Niagara pieces — a `DI<DynamicMesh>` user parameter, one GPU emitter per patch, two generated modules,
and one emitter that draws the mesh — and takes care of the engine's dynamic-mesh defects on the way.

It exists so that a shape is *computed*, not baked: no per-frame meshes, no flipbook of vertex data, and
nothing that only holds up from the one camera it was traced from. A sheet becomes a curved shell, a fan
becomes a cone, a streak becomes a tube — and the same program runs from any angle.

```cpp
System(Name="Samples/NS_Shells", Root="Game")
{
    Properties = {
        float Radius = 120.0;
    }

    DynamicMesh Shell
    {
        Settings = {
            Material  = "/Game/FX/M_Shell.M_Shell";
            TexCoords = 2;                                  // UV0..UV1
            Normals   = Auto;
            Bounds    = box(-400, -400, -400, 400, 400, 400);
            Duration  = 4.0;
        }

        Inputs = {
            float Radius = User.Radius;
            float Swirl  = hlsl { 2.0 + sin(Engine.Time) };
        }

        Patch Body = Tube(65, 24, Copies = 3)
        {
            float Twist = Theta + V * Swirl + K * 2.094;
            float R = Radius * (0.4 + 0.6 * sin(V * 3.14159)) * (1.0 + 0.15 * sin(T * 3.0 + V * 12.0));
            Position = float3(cos(Twist) * R, sin(Twist) * R, V * 600.0);
            Color    = float4(1.0, 0.4 + 0.6 * V, 0.2, 1.0 - V);
            UV1      = float2(K / NK, Rand);
        }
    }
}
```

## Shape of the block

```cpp
DynamicMesh <Name>
{
    Settings = { ... }          // the mesh: material, vertex layout, bounds, lifetime
    Inputs   = { ... }          // values the vertex programs read
    Renderer = { ... }          // extra properties of the mesh renderer that draws it
    Draw     = { ... }          // extra statements on the particle that draws it
    Patch <Name> = <Topology>(<counts>, <options>) { <HLSL> }   // one or more, in draw order
}
```

Only `Patch` is required. The block sits at the top level of a system, next to `Emitter` blocks; its
emitters are placed where the block is written, so it takes part in emitter order like any emitter.

`<Name>` becomes three things, all of which must be free: the user parameter `User.<Name>` (the mesh), the
emitter `<Name>` (draws it) and one emitter `<Name>_<Patch>` per patch (computes it).

### `Settings`

| Key | Value | Default |
| --- | --- | --- |
| `Material` | material asset path, as anywhere else in a `.dfs` | none — the engine's default material |
| `TexCoords` | UV channels per vertex, `0`–`8`; the body writes `UV0` … `UV<n-1>` | `1` |
| `Colors` | `true` / `false`: allocate the vertex colour channel | `true` |
| `Normals` | `None`, `Auto` or `Explicit` (below) | `None` |
| `Bounds` | `box(...)` in the mesh's local space — culling bounds and the emitters' fixed bounds | `box(-1000, -1000, -1000, 1000, 1000, 1000)` |
| `Duration` | seconds the mesh exists, then it is gone | as long as the system runs |
| `Visible` | any float value (literal, link, `hlsl { }`, dynamic input); the mesh is drawn while it is > 0.5 | always drawn |

`Normals`:

- `None` — no tangent basis is allocated. Unlit translucent effects rarely need one.
- `Auto` — the build differentiates the patch body: it evaluates the program a quarter of a grid step
  away in U and in V (four evaluations per vertex, all samples on the inside of the grid so poles and
  edges come out right) and writes the normal `cross(dP/dU, dP/dV)` and the tangent `dP/dU`. The body
  runs four times, so with `Auto` it may read particle attributes but not write them (DFX3065).
- `Explicit` — the body writes `Normal` (and optionally `Tangent`); the topology's analytic normal is the
  starting value. The tangent is orthogonalised against the normal before it is written.

### `Inputs`

```cpp
Inputs = {
    DI<Texture> Shape = User.ShapeTex;                       // a data interface: Shape.LoadTexture2D(...)
    float       Frame = hlsl { floor(Particles.Age * 30.0) };
    float       Gain  = FloatFromCurve(FloatCurve = curve { 0 -> 1; 1 -> 0; }, CurveIndex = Particles.NormalizedAge);
}
```

Each entry is a typed name with a value, and the value is anything a module input accepts — literal,
link, dynamic input, inline expression, `hlsl { }`. Inside the patch bodies the name is a plain variable.
Inputs are evaluated per vertex, in the computing emitter (`Particles.*` there is the vertex's particle).

Plain `User.`, `System.`, `Emitter.` and `Engine.` parameters can also be read directly in a body, as in
any `.dfm` body; `Inputs` is what a data interface or a computed value needs. A data interface has to come
through `Inputs` — a body can only call functions on a pin.

### `Patch`

```cpp
Patch Petals = Grid(33, 9, Copies = 12, Material = "/Game/FX/M_Petal.M_Petal", Flip = true)
{
    ... HLSL statements ...
}
```

A patch is a grid of `Columns × Rows` vertices, repeated `Copies` times, with one program that computes
every vertex. Patches are appended to the mesh in the order they are written; so are copies within a
patch.

| Topology | Grid | Helpers the body gets | Default `Position` | Front face |
| --- | --- | --- | --- | --- |
| `Grid(Columns, Rows)` | open sheet | — | 100 × 100 square in XY | +Z |
| `Strip(Columns)` | `Grid(Columns, 2)` | — | 100 × 10 band in XY | +Z |
| `Ring(Columns, Rows)` | U around, V radial | `Theta`, `Dir` | annulus, radius 50 → 100, in XY | −Z |
| `Tube(Columns, Rows)` | U around, V along +Z | `Theta`, `Dir` | radius 50, 100 tall | outward |
| `Sphere(Columns, Rows)` | U around, V from −Z to +Z | `Theta`, `Phi`, `Dir` | radius 100 | outward |

Counts are **vertex** counts. A closed shape repeats its seam: the first and last column of a `Tube` sit
on the same spot (with `U` 0 and 1), which is what lets UVs run 0 → 1 without a seam. `Tube(65, …)` has
64 segments around.

`Theta = 2π·U`; `Dir = (cos θ, sin θ, 0)` for a ring or tube; on a sphere `Phi = π·(1 − V)` is the
angle from +Z and `Dir` the unit direction. All topologies are the same grid underneath — the kind only
decides the helpers and the defaults, so any shape can be written on any of them.

The front face is the side `cross(dP/dU, dP/dV)` points to, for every topology; the table lists where that
lands for the defaults. This was measured, not derived: rendered with a one-sided material, an unflipped
`Grid` is visible from +Z and culled from −Z, and `Normals = Auto` lights a sphere exactly like the same
sphere with explicit outward normals. `Flip = true` turns a patch's triangles (and its `Auto` normal)
around. A two-sided material does not care.

Options: `Copies` (≥ 1, default 1), `Material` (this patch's material instead of the mesh's), `Flip`.

#### What a body sees

| Name | Type | |
| --- | --- | --- |
| `U`, `V` | float | grid coordinates, 0 → 1 inclusive |
| `I`, `J` | float | column and row, `0 … NU−1` and `0 … NV−1` (integer-valued) |
| `NU`, `NV` | float | the grid's column and row counts |
| `K`, `NK` | float | which copy, and how many there are |
| `Rand` | float | a stable random number in [0, 1) per copy (and per patch) |
| `T` | float | seconds since the mesh was spawned |
| `Index` | float | the vertex's index in the whole mesh |
| `Theta`, `Phi`, `Dir` | | topology helpers (table above) |
| your `Inputs` | | by name |

and writes:

| Name | Type | Starts as |
| --- | --- | --- |
| `Position` | float3 | the topology's default shape |
| `Color` | float4 | `(1, 1, 1, 1)`; stored as 8 bits per channel, so 0 → 1 |
| `UV0` … `UV<n-1>` | float2 | `UV0 = (U, V)`, the rest `(0, 0)` — full float precision, the place for HDR or per-vertex data |
| `Normal`, `Tangent` | float3 | the topology's analytic basis (written only with `Normals = Explicit`) |

The outputs are already declared: assign them (`Position = …;`), do not declare them again. Every name in
both tables is reserved, as is anything starting with `DM_`. The body is HLSL with the same rules as a
`.dfm` body ([dfm.md](dfm.md)): `Particles.*` attributes can be read and written (except under
`Normals = Auto`), data interface `out` arguments are plain uninitialised locals, and there are no runtime
loops — a module body also compiles for the CPU VM, whose HLSL front end rejects them (DFX6006).

### `Renderer` and `Draw`

The mesh is drawn by one CPU particle at the system's origin, through a mesh renderer bound to the mesh.

```cpp
Renderer = { SortOrderHint = 45; }        // any mesh renderer property (L8)
Draw = {                                  // appended to that particle's ParticleUpdate
    DynamicMaterialParameters(WriteParameterIndex0 = Vector4, Index0Vector4 = (1.0, 0.0, 0.0, 0.0));
    Particles.Scale = (1.0, 1.0, 1.5);
}
```

`Draw` is an ordinary stack: anything that addresses the drawing particle goes there — its colour
(`Particles.Color`, the material's Particle Color), dynamic material parameters, a scale, an orientation, a
position offset. Moving, rotating or scaling the whole mesh this way costs nothing per vertex.

## What it lowers to

For `DynamicMesh Shell` with patches `Body` and `Cap` in `System(Name="FX/NS_Shells", Root="Game")`:

| Piece | What |
| --- | --- |
| `User.Shell` | `DI<DynamicMesh>`: one section per material (in order of first use), vertex/triangle counts, `TexCoords`, colour and tangent flags, `DefaultBounds = Bounds` |
| `/Game/FX/NS_Shells_DynamicMesh/DM_GridTopology` | generated module, shared by every patch of the system: stores the particle's vertex index, writes the two triangles of its quad once, at spawn |
| `/Game/FX/NS_Shells_DynamicMesh/DM_Shell_Body` (`…_Cap`) | generated module per patch: the prologue (indices, `U`/`V`, helpers, defaults), your body, the normal estimate and the vertex writes |
| emitter `Shell_Body` (`Shell_Cap`) | GPU, local space, `Columns × Rows × Copies` particles in one burst, one per vertex; `ParticleSpawn` runs the topology module then the vertex module, `ParticleUpdate` runs the vertex module every frame |
| emitter `Shell` | CPU, one particle, a `MeshRenderer` whose mesh is bound to `User.Shell`; `Visible` writes the particle's `VisibilityTag` (0 drawn, 1 not), which the renderer filters on without any help from the material |
| `/Game/FX/NS_Shells_DynamicMesh/DM_Visibility` | generated module, only when a block sets `Visible` |

The generated modules are ordinary `.dfm` text built by the ordinary module generator, so everything in
[dfm.md](dfm.md) holds for them. A build that saves writes their source to
`Saved/DreamFX/DynamicMesh/<root>/<system path>_DynamicMesh/DM_*.dfm`, so the exact code a patch turned
into can be read. Their provenance stamp names the `.dfs`, so *Open Source* on one opens the block that
made it. A compile error in a body is reported at the patch's `{` (DFX6006), with Niagara's message and
the path of that text.

The system's provenance hash covers the generated text and a lowering version, so a block that would now
lower differently -- including after an engine upgrade that changes the texture coordinate probe below --
rebuilds even though the `.dfs` did not change.

Triangle order is draw order inside a section, and it is fixed: patches in source order, then copies, then
rows (V), then columns (U). Translucent layers that must stack belong in separate patches or copies,
written back to front.

### Engine defects handled here

- **Texture coordinates (UE 5.8).** `SetVertexTexCoord` addresses `Vertex·2 + TexCoord·2` instead of
  `(Vertex·NumTexCoords + TexCoord)·2`, on both the GPU and the CPU path, while the renderer reads the
  interleaved layout. With more than one UV channel the generator writes UVs at vertex `Index·TexCoords`
  and declares `NumVertices` as the real count times `TexCoords`, so the writes land where the renderer
  reads and pass the bounds check. The probe reads the engine's dynamic-mesh shader template, so an engine
  that fixes the bug gets plain writes (and a rebuild, because the generated text changes).
- **First frame.** A particle does not run its update stack on the frame it spawns, so the vertex module
  also runs at spawn (with `T = 0`): the mesh is complete on its first frame.

## Migrating a generator that writes modules by hand

`SaberAlterFX/Script/SaberAlter/gen_polar_dyn.py` is what this block replaces. Per phase it wrote two
modules (spawn: triangles, update: vertices), a Writer and a Draw emitter, the `DI<DynamicMesh>` JSON with
`NumVertices` multiplied by the UV channel count, and step-sum lookups to find a vertex's layer, side,
angle and radial index from its particle index. All of that is the lowering now. What stays the project's
own is the data (the shape and colour textures) and the math that turns it into positions -- the body.

```cpp
DynamicMesh PolarC
{
    Settings = {
        Material  = "/Game/SaberAlter/Materials/M_SA_PolarDyn.M_SA_PolarDyn";
        TexCoords = 4;
        Bounds    = box(-200, -9000, -9000, 200, 9000, 9000);
        Duration  = 2.95;
        Visible   = hlsl { step(100.5, floor(Particles.Age * 30.0 + 93.5)) * step(floor(Particles.Age * 30.0 + 93.5), 180.0) };
    }
    Inputs = {
        DI<Texture> Shape    = User.SA_PolarC_Shape;
        DI<Texture> ColorTex = User.SA_PolarC_Color;
        float       Frame    = hlsl { floor(Particles.Age * 30.0 + 93.5) };
    }
    Renderer = { SortOrderHint = 45; }
    Draw     = {   // M_SA_PolarDyn still multiplies its opacity by DynamicParameter.R
        DynamicMaterialParameters(WriteParameterIndex0 = Vector4, Index0Vector4 = (1.0, 0.0, 0.0, 0.0));
    }

    // Layer 0, 24 radial vertices; copy K is the crossfade side. Columns run radially, rows by angle,
    // which is the old vertex order (angle-major, radial fastest) and so the old triangle order.
    Patch Back = Grid(24, 721, Copies = 2)
    {
        float Lf = 0.0;  float Side = K;  float A = J;          // layer, side, angle index
        float Q  = clamp(floor((Frame - 100.5) / 3.0), 0.0, 26.0); // keys every 3 frames from f101
        float Qs = min(Q + Side, 26.0);
        float Tt = saturate((Frame - (101.0 + Q * 3.0)) / 3.0);
        float4 S0;  Shape.LoadTexture2D((int)A, (int)(Lf * 27.0 + Qs), 0, S0);
        float4 S1;  Shape.LoadTexture2D((int)A + 721, (int)(Lf * 27.0 + Qs), 0, S1);
        float4 C0;  ColorTex.LoadTexture2D((int)A, (int)(Qs * NU + I), 0, C0);
        float Rin = S0.x * 1024.0;
        float R   = lerp(Rin, max(S0.y * 1024.0, Rin + 1.0), U * U);  // the background's squared spacing
        float Th  = radians(-180.0 + A * 0.5);
        Position = float3(-1.0, -sin(Th) * R * 2.508733, cos(Th) * R * 2.508733);
        Color    = float4(C0.rgb, S0.z);
        UV0 = float2(A / 720.0, U * U);  UV1 = float2(S0.w, S1.x);  UV2 = float2(Side, Tt);  UV3 = float2(0.0, 0.0);
    }

    // Layers 1..18, 8 radial vertices; copy K = (layer - 1) * 2 + side, so they draw layer by layer.
    Patch Layers = Grid(8, 721, Copies = 36)
    {
        float Lf = 1.0 + floor(K * 0.5);  float Side = K - 2.0 * floor(K * 0.5);  float A = J;
        float CRow = 648.0 + (Lf - 1.0) * 216.0;
        ...   // as above, with R = lerp(Rin, Rout, U), the row CRow + Qs * NU + I, and per-layer Seed / Erode
    }
}
```

What changed in the move, and why each line is shorter:

- **Indices.** `U`, `V`, `I`, `J`, `K` replace the `Lf` / `VBase` / `Side` / `A` / `J` step sums; the old
  `V = J / max(NR - 1, 1)` is simply `U`. Two resolutions are two patches.
- **Keyframes.** Regularly spaced keys are a `floor` and a `saturate`, not 27 `step` terms. Irregular keys
  can stay as the generator's step sums, written into the body.
- **Per-layer constants** (`CRow`, `Seed`, `Erode`, `Pw`) are arithmetic of the layer where they follow a
  rule, and otherwise either step sums emitted by the generator or -- better -- a spare channel of the shape
  texture (`S1.yzw` are unused), which leaves the block hand-written and the generator writing only data.
- **Gone entirely:** both modules, both emitters, the JSON, `NumVertices × 4`, `Vertex * 4` on every UV
  write, `ExecIndex` bookkeeping, the `ModulePaths` edit, and the `BEGIN POLAR-*` splice markers.

Rebuilt in the test host with the phase-C textures, this block draws the same fan as the generated one,
and -- because the geometry is now code -- it can be made to hold from other angles by changing `Position`
alone. Replacing the X term with `-R * 2.508733 * (0.06 + 0.025 * Lf) - 25.0 * Lf` turns each traced layer
into a cone leaning towards the camera, front layers more: from the front it reads as before, from the side
it is a burst with depth instead of a card.

## Limits

- Topology is a grid of quads, fixed at build time: counts are literals, and a patch cannot change its
  triangle count while running. Collapse vertices (or zero `Color.a`) to hide parts.
- One draw order, set at build time (see above). No per-frame sorting of triangles.
- Vertex colour is 8-bit per channel; use a UV channel for anything wider.
- The mesh is in the system's local space. The engine's dynamic mesh does not cast shadows, collide, or
  appear in ray tracing.
- With more than one UV channel the vertex buffer is `TexCoords` times larger than it needs to be (the
  workaround above).
- At most 16,777,216 vertices per patch (index arithmetic is exact in float up to there), and the declared
  vertex count, times the UV workaround, must fit an `int32`.
- Generated modules are not deleted when a block or patch is removed or renamed: the old
  `<system>_DynamicMesh/DM_*` assets stay until deleted by hand (nothing references them).
- Mesh and patch names are plain identifiers (DFX3066); a back-quoted name cannot become an asset name.
- `DynamicMesh` is a system-level block; a `.dfe` cannot hold one. A child system inherits its parent's
  blocks and replaces one by declaring a block of the same name.
- Export and `pull` see the lowered emitters, not the block: an exported system comes back as the
  emitters and modules above, and `pull` leaves a `DynamicMesh` block's text alone.
