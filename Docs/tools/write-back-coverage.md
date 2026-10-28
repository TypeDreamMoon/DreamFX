# Write-back coverage and gaps: measured inventory

> Recorded on 2026-10-08 to answer **what DFX can and cannot write back**, and **which editor
> changes cannot be synchronized back to text**.
>
> Scope: read original assets and write source text, with the embedded-script extraction exception
> described in section 3.2. Numbers come from one continuous local run on a UE 5.8.2 source engine
> with DreamFX `359b3b3`, covering all 59 Niagara systems under `/AtlasFX`, `/Game/GameActor2D`
> and `/Game/AssetMaterial`. Commands and artifacts are listed in section 7. Anything not exercised
> is explicitly **unverified**. Later implementation results are dated separately in sections 6.8–6.10;
> this is a historical measurement record, not a claim that every initial gap remains in current code.
>
> The separate research note `write-back-from-dreamgui.md` covers **DreamGUI/DUI's approach and
> techniques DreamFX could reuse**. This document records **DFX's measured gaps**, without
> repeating that design discussion. The research note is not part of this documentation checkout.

## 0. Conclusions

**Successfully reading an asset does not prove it can be written back.**

- **Read**: 59/59 systems exported; only six assets reported 17 unexpressible features. The
  `coverage` command's own reported expressibility was **100%**.
- **Write**: rebuilding five samples from those 59 succeeded for **four**, while one could not
  even reach compilation (`NS_ScanScene`, 21 `DFX4007` errors).
- The four successfully built samples passed **L1/L2**, but independent **asset-reflection**
  comparison with `asset-diff` found **three assets with real differences**, including a confirmed stored-value
  divergence (section 3.5). Its visual/runtime consequence was not verified.
- The specific old failure “mesh renderer material-parameter attribute bindings disappear and
  render a white block” **did not reproduce in the measured export/rebuild path** (reflection
  evidence in section 2.1). The same kind of loss occurred elsewhere: **module-input literals**,
  invisible to `mirror-diff`.
- **`mirror-diff` (L1+L2) cannot alone establish lossless write-back.** Include `asset-diff`, which
  itself had two coverage gaps (section 3.9). At the time of the initial measurement, bidirectional
  editing still needed **an asset-level safety gate that sees state absent from text**, and **a
  supported read path for suppressed inputs**. The only existing switch, `-NoDefaults`, was
  documented as diagnostic output not intended for maintenance.

## 1. Coverage check (`coverage`)

```powershell
pwsh -File .skill/dfx.ps1 coverage -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
```

**59 systems scanned, 59 exported, zero failed.** Reported gaps: six assets, 17 occurrences.
Asset and emitter identifiers below retain their original spelling.

| # | Reported gap | Count | Assets |
|---|---|---|---|
| 1 | `emitter '位置' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光' (version 77B881A6-…)` | 3 | 白色刀光, 白色刀高光, TR刀光_System |
| 2 | `emitter '高光' inherits from '…/基础刀光.基础刀光'` | 3 | Same three |
| 3 | `emitter '暗部' inherits from '…/基础刀光.基础刀光'` | 3 | Same three |
| 4 | `emitter '刀光纹理' inherits from '…/基础刀光.基础刀光'` | 3 | Same three |
| 5 | `emitter '突刺' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/突刺.突刺' (version E752D719-…)` | 2 | Leng刀光, Leng纯色 |
| 6 | `emitter '突刺001' inherits from '…/突刺.突刺'` | 1 | Leng刀光 |
| 7 | `module 'UpdateScanSpriteSize' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |
| 8 | `module 'GetNormalizedDistance' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |

Three qualifications affect the conclusion:

1. `coverage` counts **`FDecompileResult::UnsupportedFeatures`**
   (`Source/DreamFXEditor/Private/Commandlet/DreamFXCommandlet.cpp:309-381`), the features the
   **decompiler itself acknowledges** it cannot express. **It does not test generation.**
   `NS_ScanScene` reports `ok (2 gap(s))` here, but its export cannot rebuild at all (section 3.3).
   This measures **read coverage**, not write-back coverage.
2. Inheritance gaps are the documented **DFX8014 flattening of the parent chain**: the link is
   lost, not the contents (`Docs/diagnostics/DFX8xxx.md:223-239`). The measurement confirms this:
   all four inherited emitter stacks in `TR刀光_System` survive, while `parent` becomes `none`
   (section 3.1). The table accounts for 15 inheritance occurrences, including the three thrust
   emitters; the earlier 12-occurrence subtotal covered the four slash emitters in three assets.
3. These 59 systems contained **zero `Stage` blocks, zero cases with more than one event handler,
   zero custom stage classes and zero `RibbonRenderer` blocks**. DFX8015/DFX8016 were never
   triggered. They are implemented gaps **unverified by this content**, not confirmed absent.

## 2. Decompilation evidence

```powershell
pwsh -File .skill/dfx.ps1 decompile-all -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
# 59 written, 0 failed, under C:\CrossingVoid\DFX\Decompiled\**
```

Static inventory of 59 `.dfs` exports; counts are **matching lines / matching files**:

| Feature | Count | Notes |
|---|---|---|
| `MeshRenderer` blocks | 90 / 51 | This project mainly uses mesh effects |
| `SpriteRenderer` blocks | 58 / 25 | |
| `RibbonRenderer` blocks | 0 / 0 | **Unverified** |
| `MaterialParameters =` | 12 / 12 | Two templates and ten Misaka mesh systems |
| `Meshes =` | 90 / 51 | **All JSON blobs; zero path arrays** |
| `OverrideMaterials =` | 80 / 47 | All blobs as well |
| `Material =` | 58 / 25 | All SpriteRenderer |
| `Bind … ->` | 98 / 50 | |
| `FacingMode =` | 153 / 58 | |
| `Alignment =` | 17 / 14 | |
| `SubImageSize =` | 0 / 0 | **Unused by this content; unverified** |
| `SortMode =` | 12 / 12 | |
| `LocalSpace =` | 128 / 54 | |
| `Determinism/AllocationMode/PreAllocationCount =` | 12 / 12 each | |
| `QualityLevelMask` | 117 / 36 | |
| `SimTarget =` / `FixedBounds =` / `InterpolatedSpawning =` / `CalculateBoundsMode =` | 4 / 1 each | Only `NS_ScanScene` |
| `WarmupTime =` | 59 / 59 | |
| `FixedTickDelta` / `FixedTickDeltaTime` | 59 / 59 | |
| `SystemState(` | 59 / 59 | |
| `OnEvent(` | 12 / 12 | Event stacks were exercised successfully |
| `DI<…>` | 12 / 12 | Including configuration JSON |
| `disabled ` modules | 41 / 13 | |
| `@version` | 430 / 56 | |
| `EffectType =` / `ModulePaths` / `RequiresPersistentIDs` | 0 / 0 | **Unverified** |
| `Stage ` blocks / `Defaults = {` / `as <node name>` pins | 0 / 0 | **Unverified** |
| Actual `hlsl { }` blocks | 0 / 0 | No degraded inline expressions in this content |

> Every export header contains `// Inline arithmetic is not recovered: expressions come back as
> equivalent hlsl { } blocks.` This is **a comment, not an expression**. A text search for
> `hlsl {` matches all 59 files and must not be counted as expression degradation.

### 2.1 Material-parameter attribute bindings: the old gap is closed for this path

The manually authored system `/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh`, with no
text source and representative of the old white-block failure, exports this renderer:

```c
// DFX/Decompiled/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh.dfs:90-100
MeshRenderer
{
    bOverrideMaterials = true;
    bSubImageBlend = false;
    MaterialParameters = "{\"attributeBindings\":[{\"materialParameterName\":\"Sheet\",\"niagaraVariable\":{\"name\":\"User.Atlas\",…},\"niagaraChildVariable\":{\"name\":\"ResolvedTexture\",…}}],…}";   // :94
    Meshes = "[{\"meshParameterBinding\":{…},\"mesh\":\"/Script/Engine.StaticMesh'/ZDBridge/FX/FXDefault.FXDefault'\",…}]";   // :95
    OverrideMaterials = "[{\"explicitMat\":\"/Script/Engine.Material'/AtlasFX/M_FXAtlasSheet.M_FXAtlasSheet'\",…}]";          // :96
    SortMode = ViewDepth;

    Bind CustomSorting -> Particles.NormalizedAge;   // :99
}
```

`Sheet ← User.Atlas.ResolvedTexture` is in the `MaterialParameters` blob. R4 decompilation
preserves structured property values without dedicated syntax **as JSON strings**:
`TryWriteJsonBlob` in `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1009-1044`,
called at `:1231-1237` and `:1249-1254`. Generation symmetrically inserts them **as JSON**
(`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:276-295`).

**Reflection evidence**, independent of text export: `asset-diff` reads properties directly with
`ExportText_InContainer` (`Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp:425-436`):

```text
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParameters = (AttributeBindings=((MaterialParameterName="Sheet",NiagaraVariable=(Name="User.Atlas",TypeDefHandle=(RegisteredTypeIn…
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 4095
```

The original and mirror match **character for character** in
`Saved/DreamFX/NS_Effects1_Mesh.{original,mirror}.facts`.

> The old failure does **not reproduce** on this engine/patch combination through this path.
> That specific gap can be closed, but the **class of loss** remains (section 3.4).

### 2.2 `Bind` and `MaterialParameters` are different mechanisms

- `Bind X -> Y;` handles fields of **`FNiagaraVariableAttributeBinding`**, such as
  `PositionBinding`, `ColorBinding` and `CustomSortingBinding`. Reading visits only
  `FStructProperty` values of exactly that struct type
  (`Source/DreamFXEditor/Private/Adapter/DreamFXNiagaraAdapter.cpp:2662-2675`). The decompiler
  deliberately skips property keys ending in `Binding`, since `Bind` lines represent them
  (`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1178-1183`).
- `MaterialParameters` is a **`TArray<FNiagaraMaterialAttributeBinding>`**, a different struct,
  using the JSON-blob path rather than `Bind` statements.

This matters for write-back: `MaterialParameters` has no strongly typed write path equivalent
to `Bind`. Its measured round trip succeeds (section 5), but validation depends on
`FJsonSerializer` and the engine's JSON-to-struct converter. Conversion errors are engine errors,
not DFX-specific diagnostics.

### 2.3 `Meshes` and `OverrideMaterials` did not export as the documented path arrays

`Docs/language/dfs.md:302-316` shows:

```c
Meshes            = ["/Engine/BasicShapes/Cube"];
OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
```

Measured output: **90 `Meshes` and 80 `OverrideMaterials` occurrences, all JSON blobs, zero path
arrays**. `TryWriteReferenceArray` (`DreamFXDecompiler.cpp:1055-1133`) requires both successful
`GetArrayElementReferenceField` and all other element fields equal to a default-constructed
element. Real elements carry export-text references such as `/Script/Engine.StaticMesh'…'` in
`mesh`/`explicitMat`; `TryReadReferenceObject` (`:920`) cannot read these, so output falls back to
the blob branch (`:1249-1254`).

**No data is lost**: blobs are lossless and all five available mirrors passed L1. However, the
documented authoring form differs from actual export output. Documentation and editor integrations
must not assume exports contain path arrays.

## 3. Unsupported write-back and loss inventory

> Status after the second pass on 2026-10-09: sections 3.1/3.2 remain declared gaps; 3.3's
> namespace asymmetry remains; 3.4's value write-back is addressed by `pull` (`-NoDefaults`
> supplies keys, the formal store reader supplies values); 3.5 is addressed as described in 6.9;
> 3.6/3.7/3.8 were unchanged. Sections 3.9/3.10/3.11 describe tool properties and measurement
> boundaries. Section 6.10 records later disposition item by item.

### 3.1 Parent chains (declared, DFX8014)

| Asset | Lost state | Consequence | Evidence |
|---|---|---|---|
| `SAO_Ausna/白色刀光`, `白色刀高光`, `SAO_Kirito/TR刀光_System` | Four emitter-parent links per system to `/Game/…/基础刀光` | `位置/高光/暗部/刀光纹理` stacks survive in full; future parent edits no longer propagate | Original `emitter 位置 parent = /Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光`; mirror `emitter 位置 parent = none` |
| `SAO_Kirito/Leng刀光` | Two links, `突刺` and `突刺001`, parent `…/突刺.突刺` | Same | Same pattern |
| `SAO_Kirito/Leng纯色` | One link | Same | Same pattern |

**The link, not the content, is lost.** `TR刀光_System`, with four reported gaps, receives this
L1 verdict: `bodies identical; the original records 4 gap(s) its mirror does not have`.

### 3.2 Unresolvable module scripts (declared)

In `/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene`, `GetNormalizedDistance` and
`UpdateScanSpriteSize` report `has no resolvable script asset (scratch pad or missing reference)`.
R3 in `decompile-all` extracts them as independent script assets:

```text
extracted embedded script 'GetNormalizedDistance'   -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_GetNormalizedDistance
extracted embedded script 'UpdateScanSpriteSize'    -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_UpdateScanSpriteSize
```

> **Side effect:** `decompile-all` does not only write text. This step writes **two `.uasset`
> files** under `/Game/Decompiled/…/Scripts/`. `Content/Decompiled/` is ignored by `.gitignore`.
> Any description of this command as read-only must state this exception.

### 3.3 An entire exported document cannot rebuild (undeclared)

`/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` was the **only sampled system
whose build failed directly**: 21 errors, all of the same kind.

```text
error DFX4007: Input 'Divide_Float.A' expects NiagaraFloat. 'NPC.NPC_Scan001.MaxScanRange'
  is neither a literal of that type nor a parameter reference -- parameter references start
  with a namespace such as User., Particles., Emitter., System. or Engine.
```

| Item | Detail |
|---|---|
| Source locations | `DFX/Decompiled/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene.dfs`: `:16 :65 :79 :80 :90 :145 :151 :173 :174 :184 :249 :255 :276 :277 :287 :352 :358 :378 :379 :389`; the original record describes 19 lines and 21 errors, with multiple errors on lines such as `:16` and `:80` |
| Rejected names | `NPC.NPC_Scan001.{MaxScanRange, GridCellSize, Location, NormalizedTime}`. The first segment is emitter name **`NPC`**, not `User/Particles/Emitter/System/Engine/Module/…` |
| Why reading emits it | For `EInputValueMode::Linked`, the decompiler emits the linked variable's **name unchanged**: `DreamFXDecompiler.cpp:655-656`, `case EInputValueMode::Linked: return ToNameToken(Value.LinkedVariable.GetName().ToString());` |
| Why writing rejects it | A **closed namespace allowlist** in `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:39-44` lists `Particles/Emitter/System/User/Engine/Module/Transient/StackContext/Local/Output/Parameters/DataInstance`. `IsNamespacedName` (`:204-227`) rejects `NPC`, eventually producing DFX4007 at `:652-655` |
| Consequence | No exported text for this system can rebuild; editor changes cannot complete the text round trip while the exported document fails compilation |

This is the strongest read/write mismatch in the initial run: **the reader emits a token the
same plugin's writer rejects, while `coverage` reports `ok`.**

### 3.4 Module-input literals absent from the source (undeclared)

This is the same class of loss as the old white-block issue, at a different location. The
decompiler's default-suppression rule (`DreamFXDecompiler.cpp:1730-1731`) is:

> R8: only inputs that differ from a pristine instance of the same module are printed.

**More values than expected were stored in the original's RapidIterationParameters but absent
from text.** Reflection-level fact-set comparison with `asset-diff -DumpFacts` found:

| Asset | Facts only in original | Module-input constants | System-update constants | Other |
|---|---|---|---|---|
| `/AtlasFX/Templates/NS_Atlas2D_Mesh` | **0** | 0 | 0 | 0 |
| `/AtlasFX/Templates/NS_Atlas2D_Sprite` | **0** | 0 | 0 | 0 |
| `/Game/…/Misaka/Material/Sk1/NS_Effects1_Mesh` | 2 | 2 | 0 | 0 |
| `/Game/AssetMaterial/FXs/破空/破空灰尘` | 57 | 37 | 4 | 16 |
| `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | 93 | 61 | 9 | 23 |

Examples from `Saved/DreamFX/破空灰尘.original.facts`, all absent from the mirror:

```text
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Rotation Angle (NiagaraFloat) = 0000F041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Size (Vector2f) = 0000204100002041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.AddVelocity.Velocity Speed (NiagaraFloat) = 0000C841
ri system-update Constants.Fountain.EmitterState.MaxDistance (NiagaraFloat) = 00409C45
```

Interpret these counts carefully:

- Suppression is **intended to be behaviorally equivalent**: module defaults should restore the
  omitted values on rebuild. These facts alone **do not prove incorrect rendering**.
- They are nevertheless **absent from this text**. For source-authoritative editing this means:
  1. **No supported read representation**: normal `decompile` output has neither the input value
     nor a marker saying a value was suppressed. Only `-NoDefaults` reveals it; at the time,
     `.skill/dfx.ps1:127-129` explicitly said “Diagnostic only -- the result is not meant to be maintained.”
  2. **No write-back address**: line-based write-back and a dirty set need the input's key, which
     is not present in the source at all.
- It is **fragile across module updates**: if module defaults change, the mirror follows them,
  while the original retains its stored values.

### 3.5 Confirmed stored-value divergence: `MeshYaw` −90 → 0 (undeclared)

`/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` has the smallest of the three measured
asset differences: `136 fact(s) vs 136`, **two pairs**, representing the same input in spawn and
update scripts.

```text
ORIG  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 0000B4C2
MIRR  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 00000000
```

`0xC2B40000` is **−90.0**; `0x00000000` is **0.0**. These are different values for the same variable.
Every link in the following chain was measured:

1. The source **omits** `MeshYaw`. `NS_Effects1_Mesh.dfs:87` contains only
   `/AtlasFX/Modules/Sprite_Atlas_Size(DepthScale = User.DepthScale);`.
2. The original **stores −90**, as shown above.
3. `-NoDefaults`, which prints all inputs, nevertheless exports **0**:

   ```c
   // Saved/dfx-writeback-study/NS_Effects1_Mesh.NoDefaults.dfs
   /AtlasFX/Modules/Sprite_Atlas_Size(
       MeshSize = (92.800003, 64.0),
       MinSize = (92.800003, 64.0),
       DepthScale = User.DepthScale,
       UniformScale = 1.0,
       MeshYaw = 0.0        // The asset's rapid-iteration store contains -90.
   );
   ```

   The same export of `/AtlasFX/Templates/NS_Atlas2D_Mesh` gives identical `MeshYaw = 0.0`.
   The read path therefore cannot distinguish “really zero” from “stored as −90.”
4. Across both templates and the Misaka system, **only this input** differs in value;
   `MeshSize`, `MinSize` and `UniformScale` agree on both sides.

**Confirmed:** source does not completely describe the original module-input state; the reader's
`MeshYaw` differs from the rapid-iteration store, and the mirror differs from the original.
Without `-NoCompile`, `asset-diff` **forces both sides to recompile and waits**
(`DreamFXCommandlet.cpp:816-827`), so this is not merely a stale, uncompiled value.

**Unverified:** whether Niagara runtime consumes the pin or rapid-iteration value, and therefore
whether the mirror actually renders differently. That needs L3 or visual comparison.

Related context, not proof: an AtlasFX source comment in
`Plugins/AtlasFX/Source/AtlasFXEditor/Private/AtlasFXPaperZDPreview.cpp:63` attributes an earlier
invisible preview to mesh-template `MeshYaw = -90`, which rotated the plane edge-on to the camera
(also a thin line in game); the template was changed back to zero.

This was the only initial clue connecting a definite source/asset value difference with a possible
visible change, and merited investigation first.

### 3.6 Renderer `MaterialParamValidMask` 15 → 0 (undeclared)

In emitter `位置`, one of four mesh renderers in
`/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System`:

```text
ORIG  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 15
MIRR  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 0
```

The same asset also loses a group of dynamic-material-parameter RI constants:

```text
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 2 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 3 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 4 (NiagaraFloat) = 00000000
```

The source **does contain** `DynamicMaterialParameters@1.1(…)` at `TR刀光_System.dfs:528`.
The module exists; the missing state is the renderer's valid mask and corresponding RI constants.

**Unverified:** whether `MaterialParamValidMask` is a cache recalculated during load/compile.
It is not `CPF_Transient` (otherwise `AppendPropertyFacts`, `DreamFXAssetFacts.cpp:85-89`, would
skip it), so it is serialized asset state. Whether recompilation restores it was not separately
tested. Its consequence remained unverified, alongside section 3.5.

### 3.7 Curve/DI node-name drift (undeclared)

The `compiled … di '…'` fact family differs in `破空灰尘` and `TR刀光_System`:

```text
ORIG   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.Scale Alpha.FloatCurve'    … calls: SampleCurve
ORIG   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve005.FloatCurve' … calls: SampleCurve
MIRR   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve.FloatCurve'  … calls: SampleCurve
MIRR   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve004.FloatCurve' … calls: SampleCurve
MIRR   compiled 刀光纹理   ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve006.FloatCurve' … calls: SampleCurve
```

Both sides call `SampleCurve`, suggesting similar behavior, but this is not a behavioral proof.
The **curve-owning module node names** drift (`Scale Alpha` → `FloatFromCurve`,
`FloatCurve001` → `FloatFromCurve006`). `as <name>` in `dfs.md:120-126` addresses stable node
identity, yet **none of the 59 exports contains an `as` pin**.

**Unverified:** whether this can break `Output.<node>.<value>` links. No resulting compile error
appeared in the available L2 checks, but the absence of `as` is established.

### 3.8 `EffectType`: asymmetric read/write tables (code finding, not exercised)

The comment at `DreamFXDecompiler.cpp:1266-1274` requires the decompiler table to stay aligned
with generator `EmitterSettings`: the two are halves of one contract, and a setting present on
only one side is silently lost. Inspection found:

| Scope | Generator | Reader |
|---|---|---|
| Emitter | `DreamFXGenerator.cpp:408-427`, **12** entries | `DreamFXDecompiler.cpp:1281-1298`, **12**, identical |
| Emitter entries | SimTarget / LocalSpace / Determinism / RandomSeed / AllocationMode / PreAllocationCount / InterpolatedSpawning / CalculateBoundsMode / FixedBounds / RequiresPersistentIDs / Enabled / QualityLevelMask | Same entries |
| System | `DreamFXGenerator.cpp:396-406`, **5**: `EffectType`, `WarmupTime`, `FixedBounds`, `FixedTickDelta`, `FixedTickDeltaTime` | `DreamFXDecompiler.cpp:1300-1313`, **4**: **no `EffectType`** |

At this revision, **`EffectType` can be written but not read back**. Export/rebuild can silently
omit it for systems that use it. This content had zero `EffectType =` occurrences, so the failure
was **not exercised**. The asymmetry itself was real and could be corrected with one table entry.

### 3.9 Verification-tool blind spots

| Blind spot | Explanation |
|---|---|
| **L1 compares export against export** | Both sides use the same lossy exporter, so read-path loss is structurally invisible. `DreamFXAssetFacts.h:9-18` explicitly describes this. Sections 3.4–3.7 all contained real differences despite L1 PASS |
| **L2 only establishes compilability** | The compiler does not reject every wrong or omitted value |
| **`asset-diff` initially omits system properties** | `DescribeSystem` (`DreamFXAssetFacts.cpp:246-437`) invokes `AppendPropertyFacts` for emitter data (`:406`), renderers (`:430`), stages (`:396`) and DI (`:486`), but never the `UNiagaraSystem` itself. Lost `WarmupTime`, `FixedBounds`, `bFixedTickDelta`, `EffectType` or `Determinism` therefore produces no system-property fact difference |
| **`asset-diff` does not fully compare the parent chain** | `VersionedParent` is in `EmitterDataSkip` (`DreamFXAssetFacts.cpp:258`). Existing `parent` facts come from a separate adapter read; `VersionedParentAtLastMerge` is not compared at all |
| **`mirror-diff` counts never-built mirrors as failure** | `return Failed + Missing + CompileFailed;` (`DreamFXCommandlet.cpp:746`). Five passed, zero failed, 54 never built gives **exit 54**, `dfx: FAILED`; partial-tree CI use can misinterpret it |

### 3.10 Known normalization noise

These differences may come from the comparator rather than lost authoring state:

1. **`<self>` prefixes**: original
   `emitter Fountain renderer 0:… Meshes = ((Mesh="/Script/Engine.StaticMesh'<self>_Mesh.破空灰尘_Mesh'"))`
   versus mirror `…'StaticMesh'/Game/AssetMaterial/FXs/破空/破空灰尘_Mesh.破空灰尘_Mesh'`.
   `SelfPackage` differs because the mirror lives under `/Game/Decompiled/…`; normalization
   produces different strings for the **same absolute reference**.
2. **`EditorData` / `EditorParameters`**: names such as `…破空灰尘:Fountain.NiagaraEmitterEditorData~`
   versus `…破空灰尘:Fountain~.NiagaraEmitterEditorData~` reflect editor-only object naming.
3. **`bBindingExistsOnSource=True→False` inside renderer `*Binding` fields** is compiler-derived
   cache noise. **Do not extend this dismissal to `MaterialParamValidMask`**: the section 3.6
   difference needs independent investigation.

### 3.11 Explicitly unverified features

`Stage` blocks; more than one event handler or event generators; custom C++ stage classes;
`RibbonRenderer`; `SubImageSize`; custom pivot/scale/LOD in `MeshRenderer.Meshes` elements;
`EffectType`; `ModulePaths`; `RequiresPersistentIDs`; `as <node name>`; `Defaults = {}`;
actual inline `hlsl { }`; `.dfe` `from` references; and per-device-profile platform-set overrides
(`FNiagaraPlatformSet` beyond `QualityLevelMask`). Absence from this content is not proof of support.

## 4. Supported write-back inventory

A feature counts as **measured write-back support** only when it appears in the export,
passes L1/L2, **and** survives reflection-level facts. The first two alone establish only
text-level success, with reflection coverage still qualified below.

| Category | Result | Evidence |
|---|---|---|
| **Renderer `MaterialParameters`, material-to-attribute binding** | Measured support | `NS_Effects1_Mesh.dfs:94`; identical `MaterialParameters = (AttributeBindings=(…"Sheet"…"User.Atlas"…"ResolvedTexture"…))` and `MaterialParamValidMask = 4095` facts (2.1) |
| **Renderer `Meshes` / `OverrideMaterials`** | Supported as JSON blobs | `NS_Effects1_Mesh.dfs:95-96`; matching facts; all 137/137 `NS_Atlas2D_Mesh` facts identical |
| **Renderer `Bind X -> Y`** | Text and facts agree | 98 lines in 50 files; `NS_Atlas2D_Mesh.dfs:100`; mirror re-export matches line by line |
| **Sprite renderer `Material`** | Supported | `NS_Atlas2D_Sprite.dfs:91`; all 131/131 facts identical |
| **Renderer `FacingMode` / `Alignment` / `SortMode` / `bOverrideMaterials` / `bSubImageBlend` / `SortOrderHint`** | Individually compared facts | `FacingMode`: 153 lines / 58 files; `TR刀光_System` renderer facts match except `*Binding` caches and `MaterialParamValidMask` |
| **Emitter settings, 12 entries** | Tables aligned and exercised | Section 3.8; `LocalSpace` in 54 files, `Determinism` / `AllocationMode` / `PreAllocationCount` in 12 each, `QualityLevelMask` in 36, `RandomSeed` in 12 |
| **System `WarmupTime` / `FixedBounds` / `FixedTickDelta(+Time)`** | Text-level success; initial fact collector omits system properties | `WarmupTime` and `FixedTickDelta`: 59/59 files; see 3.9 |
| **Module presence and stack order** | Supported | `TR刀光_System`: 166 view models, 43 `AddModule` calls; all five available mirrors L1 `bodies identical` |
| **Nondefault module-input values** | Supported | `破空灰尘` L1 PASS; `AddVelocity(Velocity = (-200.0, 0.0, 0.0))` (`:60`) agrees with fact `…Velocity (Vector3f) = 000048C3…` |
| **Dynamic inputs / inline expressions** | Exercised through `DynamicInputToSource`; actual `hlsl {}` absent | `FloatFromCurve(… curve { … })` and `Divide_Float(A = …, B = …)` retained, L1 PASS |
| **`disabled` modules** | Text-level success | 41 lines / 13 files |
| **`@version` pins** | Supported | 430 occurrences / 56 files; `ParticleState@1.1`, `DynamicMaterialParameters@1.1`, `ScaleColor@1.0` |
| **`OnEvent`, one handler** | Text-level success | 12 files, zero event gaps |
| **User parameters and DI JSON configuration** | Supported | `NS_Effects1_Mesh.dfs:13`, `DI<SpriteAtlas> Atlas = "{…Flipbook…FrameRects…ResolvedTexture…}"`; matching `user …` facts |
| **Collapsed Set Parameters assignments / `Defaults`** | Assignments pass at text level; `Defaults = {}` unverified | Three `AddSetParametersModule` calls in `TR刀光_System`; zero Defaults blocks |
| **`.dfe` `from` references** | Unverified; no `.dfe` use here | Documented as one-way syntax sugar; exports are self-contained (`Docs/language/dfe.md:40-48`) |

## 5. Fixed-point samples

Five systems were sampled because bulk rebuilding can stall the editor: the documented example
has 24 systems creating 237 compile jobs. Each source was rebuilt with `-Force` in a separate
editor process.

| # | Asset | Selection reason | Build | L1 | L2 | `asset-diff` | First difference |
|---|---|---|---|---|---|---|---|
| S1 | `/AtlasFX/Templates/NS_Atlas2D_Mesh` | Mesh renderer and material bindings; known-good baseline | 0 errors | **PASS** | **PASS**, UpToDate | **SAME**, 137/137 facts | None |
| S2 | `/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` | The manually authored mesh system from the white-block investigation, no text source | 0 errors | **PASS** | **PASS**, UpToDate | **DIFF**, 136/136, 2↔2 | `Sprite_Atlas_Size.MeshYaw`: original `0000B4C2` (−90), mirror `00000000` (0) |
| S3 | `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | Manually authored; four inherited emitters and mesh renderers | 0 errors | **PASS**, `bodies identical; the original records 4 gap(s) its mirror does not have` | **PASS**, UpToDate | **DIFF**, 751→654, 93↔22 | First: `compiled 刀光纹理 … di 'Emitter.FloatFromCurve.FloatCurve001'` vs `'…FloatFromCurve006.FloatCurve'` (3.7). Also original `emitter 位置 parent = /Game/…/基础刀光.基础刀光` vs `none` (declared, 3.1), 61 module constants (3.4), mask 15→0 (3.6) |
| S4 | `/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` | The only system with extracted embedded scripts | **21 errors, 0 built** | —, mirror never built | — | **MISSING** | All `DFX4007`; first at `NS_ScanScene.dfs(16,30)`, `'NPC.NPC_Scan001.MaxScanRange'` |
| S5 | `/Game/AssetMaterial/FXs/破空/破空灰尘` | Representative of `/Game/AssetMaterial` | 0 errors | **PASS** | **PASS**, UpToDate | **DIFF**, 444→403, 57↔16 | 37 module-input and four system-update constants only in original |

The aggregate scan reported **L1: 5 passed, 0 failed, 54 never built; L2: 5 compiled clean,
0 failed**. `dfx` returned `FAILED (exit 54)` because `Missing` contributes to the exit code
(3.9). `asset-diff` reported **2 same, 3 different, 54 missing**. These aggregate counts cover
the available mirrors in the scan and are distinct from the five newly selected build attempts.

The central result is that `mirror-diff` passes all five available mirrors while `asset-diff`
finds differences in three assets.

Additional S2 evidence: re-exporting its mirror and using `Compare-Object` against the original
export gives **103 lines versus 103**, differing only in the first-line provenance comment.
Even `MaterialParameters`, `Meshes` and `OverrideMaterials` blobs match byte for byte. Its problem
is **not the text round trip**, but **the text's omission of `MeshYaw`**.

## 6. Implementation recommendations and follow-up

> The separate DreamGUI research note proposed this order: (1) build safety gate, (2) `pull`,
> (3) line-level write-back plus dirty tracking, (4) coordinated compile suppression. This section
> adds measured evidence and specific changes, revising the basis for the first two items.

### 6.1 Safety gate: use `DescribeSystemFacts`, not exported text

The earlier suggestion was to reuse the `DFX8010` gate in `build`. The measurements support
the goal but **not that implementation basis**: DFX8010 relies on `Result.UnsupportedFeatures`,
yet the three differing assets reported none of the losses in 3.4–3.7. S2 and S5 report zero;
S3 reports only the four declared parent links. A text-level gate cannot see these losses.

`DescribeSystemFacts` in `Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp` is already an
independent reflection-based asset probe. After loading the target and before committing writes:

1. Record its current facts.
2. Determine what the source will produce.
3. List facts that the text cannot reproduce and refuse destructive replacement unless explicitly
   forced with `-Force`.

This reuses an existing tool at one integration point to cover the S2/S3/S5 classes of silent loss.

### 6.2 Add system properties to `asset-diff`

`DescribeSystem` (`DreamFXAssetFacts.cpp:246`) never calls `AppendPropertyFacts` for the
`UNiagaraSystem` object itself. Reflecting that object adds an independent witness for
`EffectType`, `WarmupTime`, `FixedBounds`, `bFixedTickDelta` and `bDeterminism`. Removing
`VersionedParent` (`:258`) from the skip set was also proposed so parent-chain comparison would
not rely on a separate adapter path.

### 6.3 Fix `<Emitter>.<Node>.<Input>` read/write asymmetry

`IsNamespacedName` (`DreamFXValueLowering.cpp:204-227`, allowlist at `:39-44`) rejects references
whose first segment is an emitter name, although the decompiler emits them (`:655-656`). Options:

- **Accept real context**: `Bind` already accepts emitter aliases (DFX4026 comment in
  `DreamFXGenerator.cpp:1490-1502`), and `Output.<node>.<value>` is already allowed. Recognizing
  emitter/node names from this system is more accurate than expanding a fixed keyword list.
- **Declare unsupported output**: if it cannot be expressed, record `UnsupportedFeatures`
  instead of emitting a writer-rejected token, so `coverage` stops describing it as `ok`.

Either approach must make the read-coverage report acknowledge current rebuild limitations.

### 6.4 Read path: export every input explicitly

`pull` and dirty tracking require **an input's current value even when it equals its default**.
Initially only `-NoDefaults` did this, while its documented contract excluded maintained source
(`.skill/dfx.ps1:127-129`). The implementation already exists as
`FDecompileOptions::bIncludeDefaultedInputs` (`DreamFXCommandlet.cpp:265, 1100`).

The initial recommendation was to promote this to the **read mode for write-back**, and define
the difference between normal and `-NoDefaults` exports as a **suppressed set**. Dirty write-back
would leave that set alone and update only keys the user actually changed. Otherwise the first
flush could erase values described in 3.4/3.5. Section 6.9 records the more direct implemented approach.

### 6.5 Investigate `MeshYaw` first

This is the one definite source/asset value mismatch with related evidence suggesting a visual
effect. First compare original and `/Game/Decompiled/…/NS_Effects1_Mesh` visually or through L3.
If rendering differs, add a read-path assertion comparing the input's reported value with its
rapid-iteration stored value, and report a gap when they disagree. Pin/store conflicts merit
their own diagnostic rather than silent export.

### 6.6 Correct gate interpretation

- State explicitly that `coverage` measures **read coverage**, or add a rebuildability column.
  One of these 59 systems reads successfully but cannot rebuild.
- `mirror-diff` includes `Missing` in its exit code. Either distinguish missing mirrors with a
  dedicated result, build the whole tree in CI, or provide an “already built mirrors only” mode.
- **Add `asset-diff` to the fixed-point gate.** Byte-identical no-op write-back alone is insufficient:
  three measured assets have reproducible text but unequal asset state. The initial acceptance
  proposal was both byte-identical L1 and equal asset fact sets; section 6.10 refines that criterion.

### 6.7 Documentation corrections suggested by measurements

| Location | Existing description | Measured qualification |
|---|---|---|
| `Docs/diagnostics/DFX5xxx.md:186-202`, DFX5093 | `MaterialParam` is reserved/unimplemented in v1; fix unavailable | `MaterialParam X = Y;` really is unimplemented, but **`MaterialParameters = "…json…";` works** and is emitted by the decompiler. Document the usable alternative |
| `Docs/language/dfs.md:302-316` | `Meshes = ["/Engine/BasicShapes/Cube"]` | All 90 exported occurrences were JSON blobs, zero path arrays. Integrations/parsers must support blobs |

### 6.8 Implemented: build safety gate

> Branch `feat/build-safety-gate`, diagnostic `DFX8018`. It implements section 6.1 with
> `DescribeSystemFacts` and adds the system-property coverage from 6.2. These are follow-up
> measurements; the initial measurements above remain historical evidence.

**Second-pass semantics: detect drift in state the source does not control.**

The first version (`66db8ce`) blocked every fact not retained exactly after rebuilding. That
immediately blocked ordinary authoring: changing a declared `UniformScale` from 1.0 to 2.0
looked like loss of `0000803F`, and deleting a module was blocked. The second version (`b3ca74b`)
compares each structure and consults source declarations first:

- **Before saving**: collect facts before the first asset mutation, then again after compilation
  and provenance stamping but **before `SaveSystem`**. Use reflection-level multiset difference,
  not just the first mismatch.
- **Rule 1 — compare only structures present on both sides.** An emitter, module node or renderer
  present only on one side is an intentional source addition/deletion and is ignored. Deleting
  an entire module therefore succeeds.
- **Rule 2 — a missing fact inside a retained structure is blocked.** The source could not
  represent that state; saving would destroy it, as with a lost material binding. One later
  exception (6.10): if the **same value remains at the same address after removing its script
  label**, the missing instance was a duplicate in another script store. Allow the collapse and
  report verbose `merged |`; names alone are insufficient.
- **Rule 3 — a changed fact inside a retained structure is blocked only when undeclared.**
  Consult parsed declarations, not guessed names. Settings use top-level keys from the planned
  JSON, exactly what the generator writes. Module inputs use AST argument names and Niagara
  variable names registered in `ApplyStack` against the **actual engine-assigned node name**.
  Only there is the name after `as <name>` known; guessing “module asset name plus counter” is
  explicitly rejected.
- **Rule 4 — uncertainty blocks, with distinct wording.** Details end in
  `[deterministic drift (the text does not name this input)]` when declarations were inspected,
  or `[suspected drift (cannot tell)]` when no declaration record exists for that structure.
- **Self-contained fact families**: `di`, `compiled`, `user`, event handlers, Stage and emitters
  without data lack registerable source addresses. Each whole fact is treated as its structure,
  so rule 1 interprets differences as whole-record additions/deletions. This is an explicit limit:
  DI/compiled facts lack node names, and Stage internal properties do not map one-to-one to
  `Stage(...)` argument spellings.
- `-Force` still saves, emitting the same details as **warnings** with the same markers.
- A **new target asset** is not gated. Neither are `verify` and `-NoSave`, which do not save.
- Compare **stable** facts: strip `bBindingExistsOnSource`, `bIsCachedParticleValue` and empty
  `compiled … writes:` lists caused by PostLoad discarding cached VM data. These changes affect
  only the gate; `asset-diff` output and the earlier recorded counts remain unchanged.
- **New collection surface**: system-object properties now appear as `system WarmupTime = …`.
  The skip list excludes compiler products, editor bookkeeping, identity and R3-extracted scratch pads.
- **Only explicit `-Force` bypasses the gate**, independently of ordinary `bForce` provenance
  bypass. Adopt, watcher rebuilds and the DreamGUI bridge set the latter only. Normal declared
  edits pass automatically; genuine loss is blocked.
- Artifacts on every trigger: `Saved/DreamFX/BuildSafety/<asset>.{before,after,lost}.facts`.
  After forcing, the asset no longer contains the previous state, so these files preserve the evidence.

Measurements used the same compiled plugin, one editor process per command. Logs:
`Saved/DreamFX/gate-probe/runs5|runs6/`.

| # | Command/change | Result |
|---|---|---|
| 1 | `dfx build <mirror export plus one comment>`, no `-Force` | `1 built`, zero errors/warnings. With `-LogCmds=LogDreamFX Verbose -FullStdOutLogOutput`: `build safety: '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' still holds all 186 fact(s) after the rebuild.` |
| 2 | Declared `MeshYaw = -90 → -45`, `UniformScale = 2.0 → 3.0` | **Clean**, exit 0, no build safety lines; intended value edits no longer blocked |
| 3 | Delete entire `Sprite_Atlas_Size(...)` call | **Clean**, exit 0; adding it back also succeeds |
| 4 | Keep the call, remove only two arguments | **Four blocked facts**, all deterministic: `… MeshYaw : 000034C2 -> 00000000`, `… UniformScale : 00004040 -> 0000803F` |
| 4b | Same with `-Force` | Saved, same four details at warning level, exit 0 |
| 5 | Declared system `WarmupTime = 5.0 → 1.0` | **Clean**, exit 0 |
| 6 | `NS_Effects1_Mesh` export retargeted to the **original** by removing `Decompiled/` from `Name=` | **Two blocked facts**, spawn/update `… MeshYaw : 0000B4C2 -> 00000000`, deterministic; original `.uasset` SHA256 and mtime unchanged |
| 7 | Same for `破空灰尘` | **Eight blocked facts**: four `… ScaleColor.Scale RGBA : 0000803F… -> (missing)` plus four `… EmitterState.Min/MaxDistance : 00409C45/00000000 -> (missing)`. Whole-fact disappearance has no drift marker; original unchanged |
| 8 | Copy of maintained template `DFX/Templates/NS_Atlas2D_Mesh.dfs` | **One blocked fact**: `emitter Atlas2D_Mesh renderer 0:… MaterialParameters` becomes empty `(AttributeBindings=,…)`, deterministic — the white-block failure |

**Count correction:** section 3.4's “37+4 original-only facts” compares an **original with its
mirror**. **Rebuilding in place**, cleaning stale parameters then refilling from source, loses
only the eight facts in row 7. The other 29 module constants equal defaults and are restored as
identical facts. “The source omits them” is true; “an in-place rebuild loses all 37” is not.

**A deliberate boundary authors may disagree with:** removing an argument while keeping its
module still blocks under rule 3, because the source no longer specifies that input (row 4).
Deleting the entire module passes because the source explicitly removes the whole structure.
If “reset this field to its module default” should also count as intent, the relaxation belongs
in the single `Declared.Names` decision.

**Row 8 exposed a useful secondary finding:** maintained template source
`DFX/Templates/NS_Atlas2D_Mesh.dfs` lacks the `MaterialParameters = "{…}"` blob present in the
mirror export. Rebuilding it using the documented template workflow clears the Sheet binding
and recreates the white-block problem. The gate catches it; that workflow now requires explicit
`-Force`, followed by the `AtlasFXSetup` step already requested in the template header.
`ci.ps1` uses `build -All -Force`, so CI still proceeds with loss reported as warnings.

**Associated fix required by the bundled corpus:** collecting system facts exposed
`RoundTrip/ValueModes.dfs`, which declares `FixedBounds = box(-50…50)` but rebuilt as default ±100.
The writer set the box without enabling `bFixedBounds`, while the reader emitted it only when
enabled. Other fixtures all used default ±100 and had hidden this. A companion bool on the system
`FixedBounds` mapping (`FSettingMapping::CompanionBool`) fixed it. Consequence: the only project
sources declaring system bounds, `DFX/Templates/NS_Atlas2D_Mesh.dfs` and `NS_Atlas2D_Sprite.dfs`
(±300), will actually enable fixed bounds on their next rebuild. This matches their declared
intent but is a behavior change. The repaired corpus passed **55/55**.

**Write audit:** only two disposable probes were created, `/Game/DreamFXProbe/NS_GateProbe`
and `/Game/DreamFXProbe/NS_GateSysProbe` (removed after validation), plus rebuilt mirrors.
The three targeted originals, `NS_Effects1_Mesh`, `破空灰尘` and
`/AtlasFX/Templates/NS_Atlas2D_Mesh`, retained their hashes because refusal happened before save.
Byte backups are in `Saved/DreamFX/gate-probe/asset-backup/`.

### 6.9 Implemented: `pull` (asset → source)

> Branch `feat/pull`; `dfx pull`; diagnostics `DFX7106`–`DFX7116`.
> [The pull reference](pull.md) contains full semantics, limits and measurements. This section
> relates them to the earlier findings.

- **Section 6.4 was partly adopted and partly replaced.** `-NoDefaults` still puts **keys** in
  the source; `pull` cannot update an undeclared input. **Values** now come directly from
  reflection over the rapid-iteration store, aligned with `asset-diff` and the gate, rather than
  from pins. No separately computed suppressed set is needed: undeclared source inputs are
  simply left alone (`DFX7106`).
- **Section 3.5's `MeshYaw` can now be corrected in source.** `pull -Apply` changes exactly one
  line, `0.0 -> -90.0`; `build` then reports one built, zero errors and no build safety messages,
  since rule 3 allows the declared value. Both `asset-diff` sides become `0000B4C2`. The L3/visual
  question remains **unverified**: this fixes whether the source contains the value, not which
  runtime representation is consumed.
- **The eight facts from 6.8 row 7 are reported and skipped**, demonstrating that ordinary pull
  does not add structure. For `破空灰尘`: 19 compared, zero to write, 94 undeclared, zero unaddressed
  stacks, unchanged SHA256. **Counting distinction:** 94 is deduplicated by name; the earlier
  37/57 numbers count **per-script facts**, including separate spawn/update copies of one name.
  They are different quantities and must not be interchanged.
- **Adapter contract discovered by pull:** `FNiagaraAdapter::GetEmitterInfo` **appends** to its
  output. Reusing one `FEmitterInfo` for two emitters leaks the first emitter's stacks into the
  second. `pull` creates a fresh instance per emitter and documents this. The shared adapter
  itself was not changed; that requires a separate assessment.

### 6.10 Implemented: wider coverage, dirty tracking and structural edits

> Branch `feat/write-back`, three commits starting from `feat/pull`; new diagnostics
> `DFX7112`–`DFX7115`. Details and measured cases are in [pull](pull.md), sections 4b/4c/4d/5b.

Disposition of the earlier gaps:

- **3.4, omitted module literals:** `decompile -NoDefaults` still supplies input **keys**; ordinary
  pull adds no arguments. The formal **value** path directly reads the rapid-iteration store
  for source-declared addresses. The separate suppressed-set proposal remains unnecessary.
- **3.5, `MeshYaw`:** already addressed for text write-back in 6.9; unchanged this pass.
- **3.6/3.7/3.8, `MaterialParamValidMask`, DI node names and `EffectType`:** untouched this pass.
  These concern renderer/compile state, node identity and a missing reader field, respectively,
  rather than a value write-back operation.

**The initial support table understated later coverage.** Renderer properties, settings and
assignments now have assertion-backed write paths. `Settings` uses the generator's own mapping
table in reverse; renderer value spelling is shared by decompiler and `pull` through
`WriteBack/DreamFXSourceValue.h`. Extracting that shared implementation exposed a regression:
leftover `break` statements from the previous `switch` caused only the first property to export,
failing five RoundTrip fixtures. Independent `DescribeSystemFacts` assertions caught what a
symmetrical export comparison could miss.

**The asset-level acceptance criterion was implemented more precisely:** after rebuilding the
**same asset in place**, no fact may disappear while its address remains. This targets the same
state-loss category as DFX8018 rather than requiring two separately created assets to have
identical facts. Removed whole structures and collapsed duplicate constants are reported
separately. The default-value restoration described in 6.8 explains why missing source entries
do not all become lost facts on an in-place rebuild.

**A measured difference in criteria, resolved by `feat/collapsed-copy-drift`.** `SetInput`, the
same external-edit API used by editor sliders, writes an EmitterUpdate constant into **two**
stores, `system-spawn` and `system-update`, while a DreamFX rebuild materializes one. The previous
pass recorded this as an unresolved discrepancy. Probe artifacts and complete logs are in
`Saved/DreamFX/CollapseProbe/`; the real-system export was
`/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh`, editing `EmitterState`'s `Loop Duration`.

1. **It did block, with the predicted wording.** After a slider edit, pulling in the same session
   then rebuilding produced:

   ```text
   build safety: '…' held 187 fact(s), the rebuild produces 195; of the 1 fact(s) it no longer holds exactly, 1 are drift inside a structure the rebuild kept, 9 gained.
   lost | ri system-spawn Constants.Atlas2D_Mesh.EmitterState.Loop Duration (NiagaraFloat) : 0000A040 -> one copy fewer
   error DFX8018: … was not saved …
   ```

   Raw log: `CollapseProbe/A3-refusal-before-fix.log`.
2. **The trigger window is narrower than initially suspected.** The duplicate lives only in the
   session that wrote it. Even the same session loses its `ri system-spawn` copy after
   `SaveSystem`: measured `187 facts / 0 system-spawn` before editing, `188 / 1` afterward,
   `189 / 0` after save. A fresh process also reads only one. Thus **slider → Ctrl+S → rebuild**
   does not trigger it (CLI A1/A2 builds passed). The failing path is **edit a value in the editor,
   make text match the store in that same session (pull or manual edit), then rebuild without
   saving the asset**. That is precisely the bidirectional workflow under discussion.
3. **Editor entry points do not bypass the gate.** `FGenerateOptions::bForce` skips provenance;
   `bForceLossyRebuild` allows loss. Watcher, DreamGUI bridge and Adopt set only the former,
   as explained in `DreamFXGenerator.h`. A `.dfs` save triggering a watcher rebuild in that
   session can therefore be refused, with no CLI `-Force` switch at that entry point.
4. **Fix: a rule-2 exception keyed by value plus script-independent address.**
   `FBuildSafetyGate::Compare` checks which `(address,value)` pairs survive. The address is
   `Constants.<emitter>.<node>.<input>`; the script label only says **where the value is stored**.
   For the measured EmitterUpdate constant, editing adds a same-byte system-spawn copy of a
   system-update value, and rebuilding retains one. Same address and value still present means
   a representational duplicate collapsed, not state loss. Allow it but emit verbose
   `merged | …` and the summary
   `a further N cop(y/ies) of a constant another script's store still carries collapsed into one -- not drift`.
   **Only equal values qualify**. Different values or a truly absent value still use rules 2/3/4,
   with unchanged `(missing)`, deterministic and suspected markers. The sole relaxation is the
   `AfterValuesByKey` lookup, not a blanket skip of names containing `system-spawn`.
5. **Corpus assertions:** new `DreamFX.Corpus.WriteBack.CollapsedCopy` tests four fact pairs
   directly against `Compare`, asserting decisions rather than reimplementing them: two equal
   copies become one → zero blocked, one collapsed; unequal copies with the old value gone →
   one deterministic block; the same unequal case with the input declared → zero blocked under
   unchanged rule 3; retained address but entire subject absent → one `(missing)` block.
   `ModuleInput` now asserts, rather than merely reports, `0 lost / 1 collapsed` on rebuilding:

   ```text
   ModuleInput: rebuilt in place … 172 fact(s) before, 171 after, 0 lost, 1 duplicate copy/copies collapsed, 0 structure(s) the text removed
   ```

6. **The symmetric pull case needs no semantic change.** `FindStored` in
   `WriteBack/DreamFXPull.cpp` deduplicates by name: equal copies become one source value;
   unequal copies are refused because pull cannot determine which the engine uses (pull section 6).
   Equal values therefore yield one source literal, one rebuilt store value, then an idempotent
   pull (`DFX7107 … already holds every value … No bytes written`, unchanged SHA256 and mtime).
   Unequal values are not pulled; build writes the source's intended value to its own store.
   The operations do not repeatedly overwrite each other's choices. **Pull semantics did not change.**

**End-to-end measurements:** the disposable `/Game/DreamFXProbe/NS_PullProbe` was removed after
verification. The seven cases are in pull section 5b. The structural rebuild succeeded without
any safety-gate diagnostics and did not create `Saved/DreamFX/BuildSafety/`; its only warning in
the detailed log was pre-existing `DFX7102` for unbounded SpawnRate. No-op `pull -Apply` preserved
both SHA256 and mtime. When text changed and the asset did not, `DFX7115` preserved **the text's
newer value** without touching the file.

## 7. Reproduction

### Environment

- Project `C:\CrossingVoid`; engine `D:\UnrealEngine-5.8.2` with the DreamFX engine patch.
- Plugin `C:\CrossingVoid\Plugins\DreamFX` at `359b3b3`, branch `pr/engine-patch-and-writeback`.
- **Ensure no UnrealEditor is running before reproducing.** None was running at the start of this study.

### Commands, in execution order

```powershell
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# 1) Coverage: 59 systems, 59 exports, 0 failures, 17 gaps.
pwsh -File $dfx coverage -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 2) Full export: 59 written, 0 failed, to C:\CrossingVoid\DFX\Decompiled\**.
pwsh -File $dfx decompile-all -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 3) Fixed-point sample: one build per source; write only Decompiled/ mirrors, never originals.
#    See Saved\dfx-writeback-study\run-builds.ps1.
pwsh -File C:\CrossingVoid\Saved\dfx-writeback-study\run-builds.ps1
#    S1 NS_Atlas2D_Mesh OK; S2 NS_Effects1_Mesh OK; S3 TR刀光_System OK;
#    S4 NS_ScanScene failed with 21 errors; S5 破空灰尘 OK.

# 4) L1/L2 fixed-point check.
pwsh -File $dfx mirror-diff -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 5) Independent reflection comparison, persisting facts.
pwsh -File $dfx asset-diff -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -DumpFacts -Engine $eng

# 6) Print suppressed inputs, the key step in section 3.5.
pwsh -File $dfx decompile /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -NoDefaults -Out <out>.dfs -Engine $eng
```

### Artifact locations

| Content | Location |
|---|---|
| Exports, project-side, ignored via `DFX/Decompiled/` | `C:\CrossingVoid\DFX\Decompiled\**`, 59 `.dfs` files |
| Export copies and complete raw output | `C:\CrossingVoid\Saved\dfx-writeback-study\`: `decompiled\`, `raw-*.txt`, `build-S*.txt` |
| Original/mirror fact files, five pairs | `C:\CrossingVoid\Saved\DreamFX\*.{original,mirror}.facts` |
| Ignored mirror assets | `C:\CrossingVoid\Content\Decompiled\**`, `C:\CrossingVoid\Plugins\AtlasFX\Content\Decompiled\**` |
| `-NoDefaults` comparison exports | `Saved\dfx-writeback-study\NS_Effects1_Mesh.NoDefaults.dfs`, `NS_Atlas2D_Mesh.NoDefaults.dfs` |

### Change audit for the original study

- **No original assets changed.** Builds wrote only mirrors in `Decompiled/`. The only extra
  non-text export side effect was R3 embedded-script extraction to
  `/Game/Decompiled/…/Scripts/NS_ScanScene_*.uasset`, also under `Decompiled/`.
- The project already had modified `Plugins/AtlasFX/Content/Templates/NS_Atlas2D_*.uasset`
  files (mtime 17:47) and `Content/GameActor2D/Misaka/Material/DefAtk/FX_Defatk.uasset`
  (mtime 18:07), preceding the study's 20:20 start. These were pre-existing changes.
- The plugin's `git status` was clean before this report was written.
- `D:\UnrealEngine-5.8.2` was unchanged.
