---
title: "回写覆盖范围与缺口：实测清单"
description: "回写覆盖范围与缺口：实测清单"
---

<a id="write-back-coverage-and-gaps-measured-inventory"></a>

# 回写覆盖范围与缺口：实测清单

> 记录于 2026-10-08，用于回答 **DFX 能回写什么、不能回写什么**，以及**哪些编辑器
> 更改无法同步回文本**。
>
> 范围：读取原始资产并写出源文本，例外是第 3.2 节描述的嵌入式脚本提取。数字来自一次
> 在 UE 5.8.2 源码版引擎、搭载 DreamFX `359b3b3` 的本地连续运行，
> 覆盖 `/AtlasFX`、`/Game/GameActor2D` 和 `/Game/AssetMaterial` 下的全部 59 个 Niagara 系统。
> 命令与产物列于第 7 节。凡未实际运行的内容均明确标记为**未验证**。后续实现结果
> 分别注明日期于第 6.8–6.10 节；本文件是历史测量记录，并不声称每一个初始缺口
> 都仍存在于当前代码中。
>
> 另一份研究笔记 `write-back-from-dreamgui.md` 涵盖 **DreamGUI/DUI 的方法及 DreamFX 可复用的
> 技术**。本文件记录 **DFX 的实测缺口**，不重复那部分设计讨论。该研究笔记不属于
> 本文档检出内容。

<a id="0-conclusions"></a>

## 0. 结论

**成功读取资产并不能证明它可以被回写。**

- **读取**：59/59 个系统成功导出；仅六个资产报告了 17 处无法表达的feature。
  `coverage` 命令自身报告的可表达率为 **100%**。
- **写入**：从这 59 个系统中重建五个样本，成功了**四个**，其中一个
  甚至无法进入编译（`NS_ScanScene`，21 个 `DFX4007` 错误）。
- 四个成功构建的样本通过了 **L1/L2**，但使用 `asset-diff` 独立进行的**资产反射**
  比对发现**三个资产存在真实差异**，包括一处已确认的存储值
  分歧（第 3.5 节）。其视觉/运行时后果未经验证。
- 旧的特定故障“网格渲染器材质参数属性绑定消失、
  渲染成白色方块”**在实测的导出/重建路径中未能复现**（第 2.1 节的反射
  证据）。同类的丢失发生在别处：**模块输入字面量**，
  对 `mirror-diff` 不可见。
- **`mirror-diff`（L1+L2）本身无法确立无损回写。** 还需纳入 `asset-diff`，其
  本身存在两处覆盖缺口（第 3.9 节）。在初始测量时，双向
  编辑仍需要**一个能识别文本中缺失状态的资产级安全门**，以及**一条
  针对被抑制输入的可支持读取路径**。当时唯一存在的开关 `-NoDefaults`，
  被记录为诊断输出，不用于维护用途。

<a id="1-coverage-check-coverage"></a>

## 1. 覆盖检查（`coverage`）

```powershell
pwsh -File .skill/dfx.ps1 coverage -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
```

**扫描 59 个系统，导出 59 个，零失败。** 报告的缺口：六个资产，17 处。
下方的资产和发射器标识符保留其原始拼写。

| # | 报告的缺口 | 数量 | 资产 |
|---|---|---|---|
| 1 | `emitter '位置' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光' (version 77B881A6-…)` | 3 | 白色刀光, 白色刀高光, TR刀光_System |
| 2 | `emitter '高光' inherits from '…/基础刀光.基础刀光'` | 3 | 同样三个 |
| 3 | `emitter '暗部' inherits from '…/基础刀光.基础刀光'` | 3 | 同样三个 |
| 4 | `emitter '刀光纹理' inherits from '…/基础刀光.基础刀光'` | 3 | 同样三个 |
| 5 | `emitter '突刺' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/突刺.突刺' (version E752D719-…)` | 2 | Leng刀光, Leng纯色 |
| 6 | `emitter '突刺001' inherits from '…/突刺.突刺'` | 1 | Leng刀光 |
| 7 | `module 'UpdateScanSpriteSize' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |
| 8 | `module 'GetNormalizedDistance' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |

有三点限定会影响该结论：

1. `coverage` 计数的是 **`FDecompileResult::UnsupportedFeatures`**
   （`Source/DreamFXEditor/Private/Commandlet/DreamFXCommandlet.cpp:309-381`），即**反编译器自身承认**无法表达的
   feature。**它并不测试生成。**
   `NS_ScanScene` 在此报告 `ok (2 gap(s))`，但其导出完全无法重建（第 3.3 节）。
   此处衡量的是**读取覆盖范围**，而非回写覆盖范围。
2. 继承缺口是已记录的**对父链的 DFX8014 展平**：丢失的是链接，
   而非内容（`Docs/diagnostics/DFX8xxx.md:223-239`）。测量证实了这一点：
   `TR刀光_System` 中全部四个继承的发射器栈都得以保留，而 `parent` 变成了 `none`
   （第 3.1 节）。表中计入 15 处继承，包括三个推力
   发射器；早先的 12 处小计涵盖了三个资产中的四个刀光发射器。
3. 这 59 个系统包含**零个 `Stage` 块、零个含多个事件处理器的情况、
   零个自定义阶段类以及零个 `RibbonRenderer` 块**。DFX8015/DFX8016 从未被
   触发。它们是**未经此内容验证**的实现缺口，而非已确认不存在。

<a id="2-decompilation-evidence"></a>

## 2. 反编译证据

```powershell
pwsh -File .skill/dfx.ps1 decompile-all -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
# 59 written, 0 failed, under C:\CrossingVoid\DFX\Decompiled\**
```

对 59 个 `.dfs` 导出的静态清点；计数为**匹配行数 / 匹配文件数**：

| feature | 数量 | 说明 |
|---|---|---|
| `MeshRenderer` 块 | 90 / 51 | 本项目主要使用网格特效 |
| `SpriteRenderer` 块 | 58 / 25 | |
| `RibbonRenderer` 块 | 0 / 0 | **未验证** |
| `MaterialParameters =` | 12 / 12 | 两个模板和十个 Misaka 网格系统 |
| `Meshes =` | 90 / 51 | **全部为 JSON blob；零个路径数组** |
| `OverrideMaterials =` | 80 / 47 | 同样全部为 blob |
| `Material =` | 58 / 25 | 全部为 SpriteRenderer |
| `Bind … ->` | 98 / 50 | |
| `FacingMode =` | 153 / 58 | |
| `Alignment =` | 17 / 14 | |
| `SubImageSize =` | 0 / 0 | **此内容未使用；未验证** |
| `SortMode =` | 12 / 12 | |
| `LocalSpace =` | 128 / 54 | |
| `Determinism/AllocationMode/PreAllocationCount =` | 各 12 / 12 | |
| `QualityLevelMask` | 117 / 36 | |
| `SimTarget =` / `FixedBounds =` / `InterpolatedSpawning =` / `CalculateBoundsMode =` | 各 4 / 1 | 仅 `NS_ScanScene` |
| `WarmupTime =` | 59 / 59 | |
| `FixedTickDelta` / `FixedTickDeltaTime` | 59 / 59 | |
| `SystemState(` | 59 / 59 | |
| `OnEvent(` | 12 / 12 | 事件栈已成功运行 |
| `DI<…>` | 12 / 12 | 包括配置 JSON |
| `disabled ` 模块 | 41 / 13 | |
| `@version` | 430 / 56 | |
| `EffectType =` / `ModulePaths` / `RequiresPersistentIDs` | 0 / 0 | **未验证** |
| `Stage ` 块 / `Defaults = {` / `as <node name>` 引脚 | 0 / 0 | **未验证** |
| 实际 `hlsl { }` 块 | 0 / 0 | 此内容中没有降级的内联表达式 |

> 每个导出头都包含 `// Inline arithmetic is not recovered: expressions come back as
> equivalent hlsl { } blocks.` 这**是注释，不是表达式**。文本搜索
> `hlsl {` 会匹配全部 59 个文件，不得计为表达式降级。

<a id="21-material-parameter-attribute-bindings-the-old-gap-is-closed-for-this-path"></a>

### 2.1 材质参数属性绑定：此路径下旧的缺口已闭合

手动编写的系统 `/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh`，没有
文本源，且是旧白色方块故障的典型代表，它导出了这个渲染器：

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

`Sheet ← User.Atlas.ResolvedTexture` 位于 `MaterialParameters` blob 中。R4 反编译
以 **JSON 字符串**的形式保留了结构化属性值，无需专门语法：
`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1009-1044` 中的 `TryWriteJsonBlob`，
在 `:1231-1237` 和 `:1249-1254` 处调用。生成时对称地将它们**作为 JSON** 插入
（`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:276-295`）。

**反射证据**，独立于文本导出：`asset-diff` 使用
`ExportText_InContainer` 直接读取属性（`Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp:425-436`）：

```text
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParameters = (AttributeBindings=((MaterialParameterName="Sheet",NiagaraVariable=(Name="User.Atlas",TypeDefHandle=(RegisteredTypeIn…
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 4095
```

原始与镜像在 `Saved/DreamFX/NS_Effects1_Mesh.{original,mirror}.facts` 中**逐字符**匹配。

> 旧故障在此引擎/补丁组合上通过此路径**不会复现**。
> 那个特定缺口可以闭合，但**那一类丢失**仍然存在（第 3.4 节）。

<a id="22-bind-and-materialparameters-are-different-mechanisms"></a>

### 2.2 `Bind` 与 `MaterialParameters` 是不同机制

- `Bind X -> Y;` 处理 **`FNiagaraVariableAttributeBinding`** 的字段，例如
  `PositionBinding`、`ColorBinding` 和 `CustomSortingBinding`。读取时仅访问
  恰好为该结构体类型的 `FStructProperty` 值
  （`Source/DreamFXEditor/Private/Adapter/DreamFXNiagaraAdapter.cpp:2662-2675`）。反编译器
  会故意跳过以 `Binding` 结尾的属性键，因为 `Bind` 行表示它们
  （`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1178-1183`）。
- `MaterialParameters` 是一个 **`TArray<FNiagaraMaterialAttributeBinding>`**，一个不同的结构体，
  使用 JSON blob 路径而非 `Bind` 语句。

这对回写很重要：`MaterialParameters` 没有与 `Bind` 等价的强类型写入路径。其经实测的往返测试成功（第 5 节），但验证依赖于
`FJsonSerializer` 以及引擎的 JSON 到结构体转换器。转换错误是引擎错误，
而非 DFX 特有的诊断。

<a id="23-meshes-and-overridematerials-did-not-export-as-the-documented-path-arrays"></a>

### 2.3 `Meshes` 和 `OverrideMaterials` 未按文档所述的路径数组导出

`Docs/language/dfs.md:302-316` 显示：

```c
Meshes            = ["/Engine/BasicShapes/Cube"];
OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
```

实测输出：**90 个 `Meshes` 和 80 个 `OverrideMaterials` 出现，全部为 JSON blob，零个路径
数组**。`TryWriteReferenceArray`（`DreamFXDecompiler.cpp:1055-1133`）要求成功执行
`GetArrayElementReferenceField` 且所有其他元素字段都等于默认构造的
元素。真实元素在
`mesh`/`explicitMat` 中带有导出文本引用，例如 `/Script/Engine.StaticMesh'…'`；`TryReadReferenceObject`（`:920`）无法读取这些，因此输出回退到
blob 分支（`:1249-1254`）。

**没有数据丢失**：blob 是无损的，并且所有五个可用的镜像都通过了 L1。然而，
文档所述的编写形式与实际导出输出不同。文档和编辑器集成
不得假设导出内容包含路径数组。

<a id="3-unsupported-write-back-and-loss-inventory"></a>

## 3. 不支持的回写与丢失清单

> 2026-10-09 第二轮处理后的状态：3.1/3.2 节仍为已声明的缺口；3.3 的
> 命名空间不对称仍然存在；3.4 的值回写已由 `pull` 解决（`-NoDefaults`
> 提供键，正式存储读取器提供值）；3.5 已按 6.9 中所述解决；
> 3.6/3.7/3.8 未改变。3.9/3.10/3.11 节描述工具属性和测量
> 边界。6.10 节逐项记录后续处置。

<a id="31-parent-chains-declared-dfx8014"></a>

### 3.1 父级链（已声明，DFX8014）

| 资产 | 丢失的状态 | 后果 | 证据 |
|---|---|---|---|
| `SAO_Ausna/白色刀光`、`白色刀高光`、`SAO_Kirito/TR刀光_System` | 每个系统有四个发射器父级链接指向 `/Game/…/基础刀光` | `位置/高光/暗部/刀光纹理` 堆栈完整保留；未来的父级编辑不再传播 | 原始 `emitter 位置 parent = /Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光`；镜像 `emitter 位置 parent = none` |
| `SAO_Kirito/Leng刀光` | 两个链接，`突刺` 和 `突刺001`，父级 `…/突刺.突刺` | 相同 | 相同模式 |
| `SAO_Kirito/Leng纯色` | 一个链接 | 相同 | 相同模式 |

**丢失的是链接，而不是内容。** `TR刀光_System`，有四个已报告的缺口，收到此
L1 判定：`bodies identical; the original records 4 gap(s) its mirror does not have`。

<a id="32-unresolvable-module-scripts-declared"></a>

### 3.2 无法解析的模块脚本（已声明）

在 `/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` 中，`GetNormalizedDistance` 和
`UpdateScanSpriteSize` 报告了 `has no resolvable script asset (scratch pad or missing reference)`。
`decompile-all` 中的 R3 将它们提取为独立的脚本资产：

```text
extracted embedded script 'GetNormalizedDistance'   -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_GetNormalizedDistance
extracted embedded script 'UpdateScanSpriteSize'    -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_UpdateScanSpriteSize
```

> **副作用：** `decompile-all` 不只写入文本。此步骤在 `/Game/Decompiled/…/Scripts/` 下写入 **两个 `.uasset`
> 文件**。`Content/Decompiled/` 被 `.gitignore` 忽略。
> 任何将此命令描述为只读的说明都必须注明这一例外。

<a id="33-an-entire-exported-document-cannot-rebuild-undeclared"></a>

### 3.3 整个导出的文档无法重建（未声明）

`/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` 是**唯一一个构建直接失败的采样系统**：21 个错误，全部为同一类型。

```text
error DFX4007: Input 'Divide_Float.A' expects NiagaraFloat. 'NPC.NPC_Scan001.MaxScanRange'
  is neither a literal of that type nor a parameter reference -- parameter references start
  with a namespace such as User., Particles., Emitter., System. or Engine.
```

| 项 | 详情 |
|---|---|
| 源位置 | `DFX/Decompiled/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene.dfs`：`:16 :65 :79 :80 :90 :145 :151 :173 :174 :184 :249 :255 :276 :277 :287 :352 :358 :378 :379 :389`；原始记录描述了 19 行和 21 个错误，在诸如 `:16` 和 `:80` 等行上存在多个错误 |
| 被拒绝的名称 | `NPC.NPC_Scan001.{MaxScanRange, GridCellSize, Location, NormalizedTime}`。第一段是发射器名称 **`NPC`**，而非 `User/Particles/Emitter/System/Engine/Module/…` |
| 为何读取会发出它 | 对于 `EInputValueMode::Linked`，反编译器发出所链接变量的**名称不变**：`DreamFXDecompiler.cpp:655-656`、`case EInputValueMode::Linked: return ToNameToken(Value.LinkedVariable.GetName().ToString());` |
| 为何写入会拒绝它 | `Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:39-44` 中的**封闭命名空间允许列表**列出了 `Particles/Emitter/System/User/Engine/Module/Transient/StackContext/Local/Output/Parameters/DataInstance`。`IsNamespacedName`（`:204-227`）拒绝 `NPC`，最终在 `:652-655` 产生 DFX4007 |
| 后果 | 此系统的任何导出文本都无法重建；当导出的文档编译失败时，编辑器更改无法完成文本往返 |

这是初始运行中最强的读/写不匹配：**读取器发出一个令牌，而同一插件的写入器拒绝它，同时 `coverage` 报告 `ok`。**

<a id="34-module-input-literals-absent-from-the-source-undeclared"></a>

### 3.4 源中缺失的模块输入字面量（未声明）

这与旧的白块问题属于同一类丢失，只是位置不同。反编译器的默认抑制规则（`DreamFXDecompiler.cpp:1730-1731`）是：

> R8：只有与同一模块的原始实例不同的输入才会被打印。

**比预期更多的值存储在原始版本的 RapidIterationParameters 中，但文本中缺失。** 使用 `asset-diff -DumpFacts` 进行的反射级事实集比较发现：

| 资产 | 仅存在于原始版本的事实 | 模块输入常量 | 系统更新常量 | 其他 |
|---|---|---|---|---|
| `/AtlasFX/Templates/NS_Atlas2D_Mesh` | **0** | 0 | 0 | 0 |
| `/AtlasFX/Templates/NS_Atlas2D_Sprite` | **0** | 0 | 0 | 0 |
| `/Game/…/Misaka/Material/Sk1/NS_Effects1_Mesh` | 2 | 2 | 0 | 0 |
| `/Game/AssetMaterial/FXs/破空/破空灰尘` | 57 | 37 | 4 | 16 |
| `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | 93 | 61 | 9 | 23 |

来自 `Saved/DreamFX/破空灰尘.original.facts` 的示例，全部在镜像中缺失：

```text
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Rotation Angle (NiagaraFloat) = 0000F041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Size (Vector2f) = 0000204100002041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.AddVelocity.Velocity Speed (NiagaraFloat) = 0000C841
ri system-update Constants.Fountain.EmitterState.MaxDistance (NiagaraFloat) = 00409C45
```

请仔细解读这些计数：

- 抑制**旨在行为上等价**：模块默认值应在重建时恢复
  被省略的值。仅凭这些事实**不能证明渲染不正确**。
- 然而它们**在此文本中缺失**。对于以源为权威的编辑，这意味着：
  1. **没有受支持的读取表示**：正常的 `decompile` 输出既没有输入值，
     也没有标记表明某个值被抑制。只有 `-NoDefaults` 会揭示它；当时，
     `.skill/dfx.ps1:127-129` 明确表示“仅用于诊断——结果不打算被维护。”
  2. **没有回写地址**：基于行的回写和脏集需要输入的键，而该键在源中完全不存在。
- 它**在模块更新时很脆弱**：如果模块默认值发生变化，镜像会跟随它们，
  而原始版本保留其存储的值。

<a id="35-confirmed-stored-value-divergence-meshyaw-90--0-undeclared"></a>

### 3.5 已确认的存储值差异：`MeshYaw` −90 → 0（未声明）

`/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` 在三个实测资产差异中最小：`136 fact(s) vs 136`，**两对**，表示在生成和
更新脚本中的同一输入。

```text
ORIG  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 0000B4C2
MIRR  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 00000000
```

`0xC2B40000` 是 **−90.0**；`0x00000000` 是 **0.0**。这些是同一变量的不同值。
以下链条中的每一个环节都经过测量：

1. 源**省略了** `MeshYaw`。`NS_Effects1_Mesh.dfs:87` 仅包含
   `/AtlasFX/Modules/Sprite_Atlas_Size(DepthScale = User.DepthScale);`。
2. 原始版本**存储 −90**，如上所示。
3. `-NoDefaults` 打印所有输入，却仍然导出 **0**：

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

   `/AtlasFX/Templates/NS_Atlas2D_Mesh` 的相同导出给出了完全相同的 `MeshYaw = 0.0`。
   因此读取路径无法区分“真的为零”和“存储为 −90”。
4. 在两个模板和 Misaka 系统中，**只有此输入**的值不同；
   `MeshSize`、`MinSize` 和 `UniformScale` 在两侧一致。

**已确认：**源端并未完整描述原始模块输入状态；读取端的
`MeshYaw` 与快速迭代存储不同，且镜像与原始版本不同。
没有 `-NoCompile` 时，`asset-diff` **会强制双方重新编译并等待**
（`DreamFXCommandlet.cpp:816-827`），因此这不仅仅是一个过期的、未编译的值。

**未验证：**Niagara 运行时是消费引脚值还是快速迭代值，因此
镜像在渲染上是否真的不同。这需要 L3 或视觉对比。

相关背景（并非证据）：`Plugins/AtlasFX/Source/AtlasFXEditor/Private/AtlasFXPaperZDPreview.cpp:63` 中一条 AtlasFX 源注释
将早先的不可见预览归因于网格模板 `MeshYaw = -90`，该模板将平面旋转为
侧对相机（在游戏中同样表现为一条细线）；该模板已被改回零。

这是唯一一条将确定的源/资产值差异与可能可见的变化联系起来的
初始线索，因此值得优先调查。

<a id="36-renderer-materialparamvalidmask-15--0-undeclared"></a>

### 3.6 渲染器 `MaterialParamValidMask` 15 → 0（未声明）

在发射器 `位置` 中，`/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` 里四个网格渲染器之一：

```text
ORIG  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 15
MIRR  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 0
```

同一资产还丢失了一组动态材质参数 RI 常量：

```text
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 2 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 3 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 4 (NiagaraFloat) = 00000000
```

源端**确实包含** `DynamicMaterialParameters@1.1(…)`，位于 `TR刀光_System.dfs:528`。
该模块存在；丢失的状态是渲染器的有效掩码及相应 RI 常量。

**未验证：**`MaterialParamValidMask` 是否是在加载/编译期间重新计算的缓存。
它不是 `CPF_Transient`（否则 `AppendPropertyFacts`、`DreamFXAssetFacts.cpp:85-89` 会
跳过它），因此它是序列化的资产状态。重新编译是否会恢复它未经单独
测试。其后果与第 3.5 节一样仍未验证。

<a id="37-curvedi-node-name-drift-undeclared"></a>

### 3.7 曲线/DI 节点名称漂移（未声明）

`compiled … di '…'` 事实族在 `破空灰尘` 和 `TR刀光_System` 中不同：

```text
ORIG   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.Scale Alpha.FloatCurve'    … calls: SampleCurve
ORIG   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve005.FloatCurve' … calls: SampleCurve
MIRR   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve.FloatCurve'  … calls: SampleCurve
MIRR   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve004.FloatCurve' … calls: SampleCurve
MIRR   compiled 刀光纹理   ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve006.FloatCurve' … calls: SampleCurve
```

双方都调用 `SampleCurve`，表明行为相似，但这并非行为层面的证明。
**拥有曲线的模块节点名称**发生漂移（`Scale Alpha` → `FloatFromCurve`，
`FloatCurve001` → `FloatFromCurve006`）。`dfs.md:120-126` 中的 `as <name>` 用于处理稳定的节点
标识，但**59 个导出中没有一个包含 `as` 引脚**。

**未验证：**这是否会破坏 `Output.<node>.<value>` 链接。在可用的 L2 检查中
未出现由此导致的编译错误，但 `as` 的缺失已然确立。

<a id="38-effecttype-asymmetric-readwrite-tables-code-finding-not-exercised"></a>

### 3.8 `EffectType`：非对称读/写表（代码发现，未实际触发）

`DreamFXDecompiler.cpp:1266-1274` 处的注释要求反编译器表与生成器 `EmitterSettings`
保持对齐：两者是同一契约的两半，仅存在于
一侧的设置会被静默丢失。检查发现：

| 范围 | 生成器 | 读取器 |
|---|---|---|
| 发射器 | `DreamFXGenerator.cpp:408-427`，**12** 项 | `DreamFXDecompiler.cpp:1281-1298`，**12** 项，完全相同 |
| 发射器条目 | SimTarget / LocalSpace / Determinism / RandomSeed / AllocationMode / PreAllocationCount / InterpolatedSpawning / CalculateBoundsMode / FixedBounds / RequiresPersistentIDs / Enabled / QualityLevelMask | 相同条目 |
| 系统 | `DreamFXGenerator.cpp:396-406`，**5**：`EffectType`、`WarmupTime`、`FixedBounds`、`FixedTickDelta`、`FixedTickDeltaTime` | `DreamFXDecompiler.cpp:1300-1313`，**4**：**没有 `EffectType`** |

在此修订版中，**`EffectType` 可以写入但无法读回**。导出/重建可能会对
使用它的系统静默省略该值。此内容中 `EffectType =` 出现次数为零，因此该失败
**未被实际触发**。这种不对称本身是真实存在的，只需一个表项即可修正。

<a id="39-verification-tool-blind-spots"></a>

### 3.9 验证工具的盲区

| 盲区 | 说明 |
|---|---|
| **L1 将导出与导出比较** | 双方使用同一个有损导出器，因此读取路径的丢失在结构上不可见。`DreamFXAssetFacts.h:9-18` 明确描述了这一点。第 3.4–3.7 节均包含真实差异，尽管 L1 通过 |
| **L2 仅确定可编译性** | 编译器并不会拒绝每一个错误或省略的值 |
| **`asset-diff` 最初省略系统属性** | `DescribeSystem`（`DreamFXAssetFacts.cpp:246-437`）为发射器数据（`:406`）、渲染器（`:430`）、阶段（`:396`）和 DI（`:486`）调用 `AppendPropertyFacts`，但从不调用 `UNiagaraSystem` 本身。因此丢失的 `WarmupTime`、`FixedBounds`、`bFixedTickDelta`、`EffectType` 或 `Determinism` 不会产生任何系统属性事实差异 |
| **`asset-diff` 未完整比较父链** | `VersionedParent` 位于 `EmitterDataSkip`（`DreamFXAssetFacts.cpp:258`）中。既有的 `parent` 事实来自单独的适配器读取；`VersionedParentAtLastMerge` 完全未被比较 |
| **`mirror-diff` 将从未构建的镜像计为失败** | `return Failed + Missing + CompileFailed;`（`DreamFXCommandlet.cpp:746`）。五个通过，零个失败，54 个从未构建，得到 **退出码 54**，`dfx: FAILED`；部分树 CI 使用可能会误读它 |

<a id="310-known-normalization-noise"></a>

### 3.10 已知的归一化噪声

这些差异可能来自比较器而非丢失的创作状态：

1. **`<self>` 前缀**：原始
   `emitter Fountain renderer 0:… Meshes = ((Mesh="/Script/Engine.StaticMesh'<self>_Mesh.破空灰尘_Mesh'"))`
   对比镜像 `…'StaticMesh'/Game/AssetMaterial/FXs/破空/破空灰尘_Mesh.破空灰尘_Mesh'`。
   `SelfPackage` 不同是因为镜像位于 `/Game/Decompiled/…` 之下；归一化
   会为**同一绝对引用**产生不同的字符串。
2. **`EditorData` / `EditorParameters`**：诸如 `…破空灰尘:Fountain.NiagaraEmitterEditorData~`
   对比 `…破空灰尘:Fountain~.NiagaraEmitterEditorData~` 的名称反映的是仅编辑器内的对象命名。
3. **渲染器 `*Binding` 字段内的 `bBindingExistsOnSource=True→False`** 是编译器派生的
   缓存噪声。**不要将这种排除延伸到 `MaterialParamValidMask`**：第 3.6 节
   的差异需要独立调查。

<a id="311-explicitly-unverified-features"></a>

### 3.11 明确未验证的功能

`Stage` 块；多个事件处理器或事件生成器；自定义 C++ 阶段类；
`RibbonRenderer`；`SubImageSize`；`MeshRenderer.Meshes` 元素中的自定义枢轴/缩放/LOD；
`EffectType`；`ModulePaths`；`RequiresPersistentIDs`；`as <node name>`；`Defaults = {}`；
实际内联 `hlsl { }`；`.dfe` `from` 引用；以及按设备配置文件的平台集覆盖
（`FNiagaraPlatformSet` 超出 `QualityLevelMask`）。未出现在本内容中不构成受支持的证明。

<a id="4-supported-write-back-inventory"></a>

## 4. 受支持的写回清单

只有当某功能出现在导出中、通过 L1/L2 **并且**在反射级事实上存续时，才算作
**已测得的写回支持**。仅满足前两项只能确立文本层面的成功，反射覆盖范围
仍受限于下文所述条件。

| 类别 | 结果 | 证据 |
|---|---|---|
| **渲染器 `MaterialParameters`、材质到属性绑定** | 实测支持 | `NS_Effects1_Mesh.dfs:94`；完全一致的 `MaterialParameters = (AttributeBindings=(…"Sheet"…"User.Atlas"…"ResolvedTexture"…))` 与 `MaterialParamValidMask = 4095` 事实（2.1） |
| **渲染器 `Meshes` / `OverrideMaterials`** | 作为 JSON blob 受支持 | `NS_Effects1_Mesh.dfs:95-96`；事实匹配；全部 137/137 条 `NS_Atlas2D_Mesh` 事实完全一致 |
| **渲染器 `Bind X -> Y`** | 文本与事实一致 | 50 个文件中 98 行；`NS_Atlas2D_Mesh.dfs:100`；镜像再导出逐行匹配 |
| **Sprite 渲染器 `Material`** | 受支持 | `NS_Atlas2D_Sprite.dfs:91`；全部 131/131 条事实完全一致 |
| **渲染器 `FacingMode` / `Alignment` / `SortMode` / `bOverrideMaterials` / `bSubImageBlend` / `SortOrderHint`** | 逐项对比的事实 | `FacingMode`：153 行 / 58 个文件；`TR刀光_System` 渲染器事实匹配，除了 `*Binding` 缓存与 `MaterialParamValidMask` |
| **发射器设置，12 个条目** | 表格对齐并已验证 | 第 3.8 节；`LocalSpace` 出现在 54 个文件中，`Determinism` / `AllocationMode` / `PreAllocationCount` 各出现在 12 个文件中，`QualityLevelMask` 出现在 36 个文件中，`RandomSeed` 出现在 12 个文件中 |
| **系统 `WarmupTime` / `FixedBounds` / `FixedTickDelta(+Time)`** | 文本层面成功；初始事实收集器遗漏系统属性 | `WarmupTime` 与 `FixedTickDelta`：59/59 个文件；见 3.9 |
| **模块存在性与栈顺序** | 受支持 | `TR刀光_System`：166 个视图模型，43 次 `AddModule` 调用；全部五个可用镜像 L1 `bodies identical` |
| **非默认模块输入值** | 受支持 | `破空灰尘` L1 通过；`AddVelocity(Velocity = (-200.0, 0.0, 0.0))`（`:60`）与事实 `…Velocity (Vector3f) = 000048C3…` 一致 |
| **动态输入 / 内联表达式** | 通过 `DynamicInputToSource` 验证；实际 `hlsl {}` 缺失 | `FloatFromCurve(… curve { … })` 与 `Divide_Float(A = …, B = …)` 保留，L1 通过 |
| **`disabled` 模块** | 文本层面成功 | 41 行 / 13 个文件 |
| **`@version` 引脚** | 受支持 | 430 次出现 / 56 个文件；`ParticleState@1.1`、`DynamicMaterialParameters@1.1`、`ScaleColor@1.0` |
| **`OnEvent`，一个处理程序** | 文本层面成功 | 12 个文件，零事件缺口 |
| **用户参数与 DI JSON 配置** | 受支持 | `NS_Effects1_Mesh.dfs:13`、`DI<SpriteAtlas> Atlas = "{…Flipbook…FrameRects…ResolvedTexture…}"`；匹配的 `user …` 事实 |
| **折叠的 Set Parameters 赋值 / `Defaults`** | 赋值在文本层面通过；`Defaults = {}` 未验证 | `TR刀光_System` 中有三次 `AddSetParametersModule` 调用；零个 Defaults 块 |
| **`.dfe` `from` 引用** | 未验证；此处未使用 `.dfe` | 记录为单向语法糖；导出是自包含的（`Docs/language/dfe.md:40-48`） |

<a id="5-fixed-point-samples"></a>

## 5. 定点样本

由于批量重建可能导致编辑器停滞，因此对五个系统进行了抽样：文档示例
包含 24 个系统，创建了 237 个编译任务。每个源均在单独的编辑器进程中
使用 `-Force` 重建。

| # | 资产 | 选择原因 | 构建 | L1 | L2 | `asset-diff` | 首个差异 |
|---|---|---|---|---|---|---|---|
| S1 | `/AtlasFX/Templates/NS_Atlas2D_Mesh` | 网格渲染器与材质绑定；已知良好的基线 | 0 错误 | **通过** | **通过**，UpToDate | **相同**，137/137 条事实 | 无 |
| S2 | `/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` | 来自白块调查的手动编写的网格系统，无文本源 | 0 错误 | **通过** | **通过**，UpToDate | **不同**，136/136，2↔2 | `Sprite_Atlas_Size.MeshYaw`：原始 `0000B4C2`（−90），镜像 `00000000`（0） |
| S3 | `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | 手动编写；四个继承的发射器和网格渲染器 | 0 错误 | **通过**，`bodies identical; the original records 4 gap(s) its mirror does not have` | **通过**，UpToDate | **不同**，751→654，93↔22 | 首个：`compiled 刀光纹理 … di 'Emitter.FloatFromCurve.FloatCurve001'` 与 `'…FloatFromCurve006.FloatCurve'`（3.7）。还有原始 `emitter 位置 parent = /Game/…/基础刀光.基础刀光` 与 `none`（声明，3.1），61 个模块常量（3.4），遮罩 15→0（3.6） |
| S4 | `/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` | 唯一包含提取出的嵌入式脚本的系统 | **21 个错误，0 个构建** | —，镜像从未构建 | — | **缺失** | 全部 `DFX4007`；首个位于 `NS_ScanScene.dfs(16,30)`，`'NPC.NPC_Scan001.MaxScanRange'` |
| S5 | `/Game/AssetMaterial/FXs/破空/破空灰尘` | `/Game/AssetMaterial` 的代表 | 0 错误 | **通过** | **通过**，UpToDate | **不同**，444→403，57↔16 | 仅存在于原始中的 37 个模块输入和四个系统更新常量 |

聚合扫描报告 **L1：5 个通过，0 个失败，54 个从未构建；L2：5 个编译干净，
0 个失败**。`dfx` 返回 `FAILED (exit 54)`，因为 `Missing` 对退出码有贡献
（3.9）。`asset-diff` 报告 **2 个相同，3 个不同，54 个缺失**。这些聚合计数涵盖
扫描中的可用镜像，且与五个新选择的构建尝试不同。

核心结果是 `mirror-diff` 通过全部五个可用镜像，而 `asset-diff`
在三个资产中发现了差异。

附加的 S2 证据：重新导出其镜像并对原始导出使用 `Compare-Object`
得到 **103 行对 103 行**，仅在第一行的来源注释上不同。
甚至 `MaterialParameters`、`Meshes` 和 `OverrideMaterials` blob 也逐字节匹配。其问题
**不是文本往返**，而是**文本遗漏了 `MeshYaw`**。

<a id="6-implementation-recommendations-and-follow-up"></a>

## 6. 实现建议与后续工作

> 单独的 DreamGUI 研究笔记提出了此顺序：（1）构建安全门，（2）`pull`，
> （3）行级回写加脏跟踪，（4）协调编译抑制。本节
> 增加了实测证据和具体变更，修订了前两项的依据。

<a id="61-safety-gate-use-describesystemfacts-not-exported-text"></a>

### 6.1 安全门：使用 `DescribeSystemFacts`，而不是导出的文本

先前的建议是复用 `build` 中的 `DFX8010` 门。测量支持
该目标，但**不支持该实现依据**：DFX8010 依赖 `Result.UnsupportedFeatures`，
然而三个存在差异的资产均未报告 3.4–3.7 中的任何损失。S2 和 S5 报告为零；
S3 仅报告四个声明的父链接。文本层面的门无法看到这些损失。

`Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp` 中的 `DescribeSystemFacts` 已经是一个
独立的基于反射的资产探测。在加载目标之后、提交写入之前：

1. 记录其当前事实。
2. 确定源将产生什么。
3. 列出文本无法复现的事实，并拒绝破坏性替换，除非使用 `-Force` 明确
   强制。

这在一个集成点复用了现有工具，以覆盖 S2/S3/S5 类别的静默损失。

<a id="62-add-system-properties-to-asset-diff"></a>

### 6.2 向 `asset-diff` 添加系统属性

`DescribeSystem`（`DreamFXAssetFacts.cpp:246`）从未为
`UNiagaraSystem` 对象本身调用 `AppendPropertyFacts`。反射该对象为
`EffectType`、`WarmupTime`、`FixedBounds`、`bFixedTickDelta` 和 `bDeterminism` 添加了一个独立见证。
还提议从跳过集合中移除
`VersionedParent`（`:258`），以便父链比较不会
依赖单独的适配器路径。

<a id="63-fix-emitternodeinput-readwrite-asymmetry"></a>

### 6.3 修复 `<Emitter>.<Node>.<Input>` 的读/写不对称

`IsNamespacedName`（`DreamFXValueLowering.cpp:204-227`，允许列表位于 `:39-44`）拒绝首段为发射器名称的
引用，尽管反编译器会发出它们（`:655-656`）。选项：

- **接受真实上下文**：`Bind` 已经接受发射器别名（`DreamFXGenerator.cpp:1490-1502` 中的 DFX4026 注释），并且 `Output.<node>.<value>` 已被允许。从该系统识别发射器/节点名称比扩展固定的关键字列表更准确。
- **声明不支持的输出**：如果无法表达，请记录 `UnsupportedFeatures`，而不是发出被写入器拒绝的标记，这样 `coverage` 就不会再将其描述为 `ok`。

无论采用哪种方法，读取覆盖率报告都必须承认当前重建的限制。

<a id="64-read-path-export-every-input-explicitly"></a>

### 6.4 读取路径：显式导出每个输入

`pull` 和脏跟踪要求**输入即使等于其默认值也要提供当前值**。最初只有 `-NoDefaults` 这样做，而它已记录的契约排除了受维护的源（`.skill/dfx.ps1:127-129`）。该实现已存在，形式为 `FDecompileOptions::bIncludeDefaultedInputs`（`DreamFXCommandlet.cpp:265, 1100`）。

最初的建议是将其提升为**回写的读取模式**，并将普通导出与 `-NoDefaults` 导出之间的差异定义为一个**抑制集合**。脏回写将保持该集合不变，只更新用户实际更改的键。否则首次刷新可能会抹去 3.4/3.5 中描述的值。第 6.9 节记录了更直接的已实现方法。

<a id="65-investigate-meshyaw-first"></a>

### 6.5 首先调查 `MeshYaw`

这是唯一一处确定的源/资产值不匹配，且有相关证据表明存在视觉影响。首先通过视觉方式或 L3 比较原始版本和 `/Game/Decompiled/…/NS_Effects1_Mesh`。如果渲染结果不同，请添加一个读取路径断言，比较输入报告的值与其快速迭代存储的值，并在两者不一致时报告缺口。固定/存储冲突值得拥有自己的诊断，而不是静默导出。

<a id="66-correct-gate-interpretation"></a>

### 6.6 正确解读门禁

- 明确说明 `coverage` 衡量的是**读取覆盖率**，或者添加一列可重建性。这 59 个系统中有 1 个可以成功读取但无法重建。
- `mirror-diff` 的退出码中包含了 `Missing`。要么用专门的结果区分缺失的镜像，要么在 CI 中构建整棵树，或者提供一种“仅针对已构建镜像”的模式。
- **将 `asset-diff` 添加到定点门禁。**仅靠字节完全一致的 no-op 回写是不够的：三个实测资产具有可复现的文本，但资产状态不相等。最初的验收方案是同时满足字节完全一致的 L1 和相等的资产生成事实集；第 6.10 节细化了该标准。

<a id="67-documentation-corrections-suggested-by-measurements"></a>

### 6.7 根据测量结果建议的文档更正

| 位置 | 现有描述 | 实测限定 |
|---|---|---|
| `Docs/diagnostics/DFX5xxx.md:186-202`, DFX5093 | `MaterialParam` 在 v1 中保留/未实现；修复不可用 | `MaterialParam X = Y;` 确实未实现，但 **`MaterialParameters = "…json…";` 可以工作**，并且由反编译器发出。请记录可用的替代方案 |
| `Docs/language/dfs.md:302-316` | `Meshes = ["/Engine/BasicShapes/Cube"]` | 全部 90 个导出出现均为 JSON 块，零个路径数组。集成/解析器必须支持块 |

<a id="68-implemented-build-safety-gate"></a>

### 6.8 已实现：构建安全门禁

> 分支 `feat/build-safety-gate`，诊断 `DFX8018`。它通过 `DescribeSystemFacts` 实现了第 6.1 节，并添加了来自 6.2 的系统属性覆盖率。这些是后续测量；上面的初始测量仍为历史证据。

**第二遍语义：检测源无法控制的状态漂移。**

第一个版本（`66db8ce`）会阻止重建后未能精确保留的每一个事实。这立即阻止了普通创作：将声明的 `UniformScale` 从 1.0 改为 2.0 看起来像是丢失了 `0000803F`，而删除模块也会被阻止。第二个版本（`b3ca74b`）会比较每个结构并首先参考源声明：

- **保存之前**：在第一次资产变更之前收集事实，然后在编译和溯源标记之后、**`SaveSystem` 之前**再次收集。使用反射级别的多重集差异，而不仅仅是第一个不匹配项。
- **规则 1——只比较两侧都存在的结构。**仅存在于一侧的发射器、模块节点或渲染器是有意的源添加/删除，因此被忽略。因此删除整个模块可以成功。
- **规则 2——保留下来的结构中缺失的事实会被阻止。**源无法表示该状态；保存会破坏它，就像丢失材质绑定一样。稍后的一个例外（6.10）：如果**移除其脚本标签后同一个值仍保留在同一地址**，则缺失的实例是另一个脚本存储中的重复项。允许合并并报告详细的 `merged |`；仅凭名称是不够的。
- **规则 3——保留下来的结构中发生变化的事实，只有在未声明时才会被阻止。**请参考解析后的声明，而不是猜测的名称。设置使用计划 JSON 中的顶层键，正是生成器所写入的内容。模块输入使用在 `ApplyStack` 中注册的 AST 参数名和 Niagara 变量名，与**实际的引擎分配节点名称**进行匹配。只有在那里，`as <name>` 之后的名称才是已知的；猜测“模块资产名加计数器”的做法被明确拒绝。
- **规则 4——不确定时阻止，并使用不同的措辞。**当已检查声明时，详细信息以 `[deterministic drift (the text does not name this input)]` 结尾；当该结构不存在声明记录时，以 `[suspected drift (cannot tell)]` 结尾。
- **自包含的事实族**：`di`、`compiled`、`user`、事件处理器、Stage 以及没有数据的发射器，都缺少可注册的源地址。每个完整事实被视为其结构，因此规则 1 将差异解释为整条记录的添加/删除。这是一个明确的限制：DI/编译后的事实缺少节点名称，而 Stage 内部属性与 `Stage(...)` 参数拼写并非一一对应。
- `-Force` 仍然保存，以**警告**形式发出相同的详细信息，并使用相同的标记。
- **新的目标资产**不受门禁限制。`verify` 和 `-NoSave` 也不受限制，因为它们不保存。
- 比较**稳定**的事实：剥离 `bBindingExistsOnSource`、`bIsCachedParticleValue` 以及由 PostLoad 丢弃缓存 VM 数据导致的空 `compiled … writes:` 列表。这些变化只影响门禁；`asset-diff` 输出和先前记录的计数保持不变。
- **新的收集面**：系统对象属性现在表现为 `system WarmupTime = …`。跳过列表排除了编译器产物、编辑器簿记、身份标识以及 R3 提取的暂存区。
- **只有显式的 `-Force` 会绕过门禁**，独立于普通的 `bForce` 溯源绕过。采纳、监视器重建和 DreamGUI 桥接只设置后者。正常的已声明编辑会自动通过；真正的丢失会被阻止。
- 每次触发时的产物：`Saved/DreamFX/BuildSafety/<asset>.{before,after,lost}.facts`。强制之后，资产不再包含先前的状态，因此这些文件保留了证据。

测量使用相同的已编译插件，每个命令一个编辑器进程。日志：`Saved/DreamFX/gate-probe/runs5|runs6/`。

| # | 命令/变更 | 结果 |
|---|---|---|
| 1 | `dfx build <mirror export plus one comment>`，无 `-Force` | `1 built`，零错误/警告。使用 `-LogCmds=LogDreamFX Verbose -FullStdOutLogOutput` 时：`build safety: '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' still holds all 186 fact(s) after the rebuild.` |
| 2 | 声明了 `MeshYaw = -90 → -45`，`UniformScale = 2.0 → 3.0` | **干净**，退出码 0，没有构建安全行；预期的值编辑不再被阻止 |
| 3 | 删除整个 `Sprite_Atlas_Size(...)` 调用 | **干净**，退出码 0；再添加回去也成功 |
| 4 | 保留该调用，仅移除两个参数 | **四个被阻止的事实**，全部为确定性的：`… MeshYaw : 000034C2 -> 00000000`，`… UniformScale : 00004040 -> 0000803F` |
| 4b | 与 `-Force` 相同 | 已保存，同样的四个细节处于警告级别，退出码 0 |
| 5 | 声明了系统 `WarmupTime = 5.0 → 1.0` | **干净**，退出码 0 |
| 6 | 通过从 `Name=` 中移除 `Decompiled/`，将 `NS_Effects1_Mesh` 导出重定向到**原始** | **两个被阻止的事实**，spawn/update `… MeshYaw : 0000B4C2 -> 00000000`，确定性的；原始 `.uasset` SHA256 和 mtime 不变 |
| 7 | 对 `破空灰尘` 做同样处理 | **八个被阻止的事实**：四个 `… ScaleColor.Scale RGBA : 0000803F… -> (missing)` 加上四个 `… EmitterState.Min/MaxDistance : 00409C45/00000000 -> (missing)`。整个事实消失没有漂移标记；原始文件不变 |
| 8 | 复制所维护的模板 `DFX/Templates/NS_Atlas2D_Mesh.dfs` | **一个被阻止的事实**：`emitter Atlas2D_Mesh renderer 0:… MaterialParameters` 变为空 `(AttributeBindings=,…)`，确定性的——即白块失败 |

**计数更正：**第 3.4 节的“37+4 个仅原始事实”是将**原始文件与其镜像**进行比较。**原地重建**——先清理陈旧参数再从源重新填充——仅丢失第 7 行中的八个事实。其余 29 个模块常量等于默认值，并作为相同的事实被恢复。“源省略了它们”是对的；“原地重建会丢失全部 37 个”则不对。

**一个作者们可能有异议的刻意边界：**在保留其模块的同时移除一个参数，仍会按规则 3 被阻止，因为源不再指定该输入（第 4 行）。删除整个模块则通过，因为源明确移除了整个结构。如果“将此字段重置为其模块默认值”也应算作意图，那么放宽应放在单个 `Declared.Names` 决策中。

**第 8 行揭示了一个有用的次要发现：**所维护的模板源 `DFX/Templates/NS_Atlas2D_Mesh.dfs` 缺少镜像导出中存在的 `MaterialParameters = "{…}"` blob。使用文档化的模板工作流重建它会清除 Sheet 绑定并重新产生白块问题。门禁会捕获它；该工作流现在需要显式 `-Force`，随后执行模板头部已请求的 `AtlasFXSetup` 步骤。`ci.ps1` 使用 `build -All -Force`，因此 CI 仍会继续，并将损失报告为警告。

**捆绑语料库所需的关联修复：**收集系统事实暴露了 `RoundTrip/ValueModes.dfs`，它声明了 `FixedBounds = box(-50…50)`，但重建为默认 ±100。写入方设置了该框，却没有启用 `bFixedBounds`，而读取方仅在启用时才输出它。其他夹具都使用默认 ±100，因此掩盖了这一点。在系统 `FixedBounds` 映射上添加一个伴随布尔值（`FSettingMapping::CompanionBool`）修复了它。后果：唯一声明了系统边界的项目源 `DFX/Templates/NS_Atlas2D_Mesh.dfs` 和 `NS_Atlas2D_Sprite.dfs`（±300）将在下次重建时实际启用固定边界。这符合其声明的意图，但属于行为变更。修复后的语料库通过了 **55/55**。

**写入审计：**仅创建了两个一次性探针，`/Game/DreamFXProbe/NS_GateProbe` 和 `/Game/DreamFXProbe/NS_GateSysProbe`（验证后已移除），以及重建的镜像。三个目标原始文件 `NS_Effects1_Mesh`、`破空灰尘` 和 `/AtlasFX/Templates/NS_Atlas2D_Mesh` 保留了其哈希值，因为拒绝发生在保存之前。字节备份位于 `Saved/DreamFX/gate-probe/asset-backup/`。

<a id="69-implemented-pull-asset--source"></a>

### 6.9 已实现：`pull`（资产 → 源）

> 分支 `feat/pull`；`dfx pull`；诊断 `DFX7106`–`DFX7116`。
> [拉取参考](pull.md)包含完整语义、限制和测量结果。本节
> 将其与先前的发现关联起来。

- **第 6.4 节被部分采纳、部分替换。** `-NoDefaults` 仍然将**键**放入
  源中；`pull` 无法更新未声明的输入。**值**现在直接来自
  对快速迭代存储的反射，与 `asset-diff` 和门禁对齐，而不是
  来自引脚。不再需要单独计算的抑制集合：未声明的源输入
  只是被原样保留（`DFX7106`）。
- **第 3.5 节的 `MeshYaw` 现在可以在源中更正。** `pull -Apply` 恰好更改一行，
  `0.0 -> -90.0`；随后 `build` 报告一个已构建、零错误且没有构建安全消息，
  因为规则 3 允许该声明值。两个 `asset-diff` 侧都变为 `0000B4C2`。L3/视觉
  问题仍然**未验证**：这修复的是源是否包含该值，而不是消费哪种
  运行时表示。
- **来自 6.8 第 7 行的八个事实被报告并跳过**，表明普通拉取
  不会添加结构。对于 `破空灰尘`：比较了 19 个，零个待写入，94 个未声明，零个未处理的
  栈，SHA256 不变。**计数区别：**94 是按名称去重的；先前的
  37/57 数字按**每个脚本的事实**计数，包括同一名称的单独 spawn/update 副本。
  它们是不同的量，不可互换。
- **通过拉取发现的适配器契约：** `FNiagaraAdapter::GetEmitterInfo` 会**追加**到其
  输出。为一个 `FEmitterInfo` 复用给两个发射器会将第一个发射器的栈泄漏到
  第二个中。`pull` 为每个发射器创建一个新实例并记录了这一点。共享适配器
  本身未被更改；那需要单独评估。

<a id="610-implemented-wider-coverage-dirty-tracking-and-structural-edits"></a>

### 6.10 已实现：更广的覆盖率、脏跟踪和结构编辑

> 分支 `feat/write-back`，从 `feat/pull` 开始的三个提交；新诊断
> `DFX7112`–`DFX7115`。细节和实测用例见[拉取](pull.md)第 4b/4c/4d/5b 节。

先前缺口的处置：

- **3.4，被省略的模块字面量：** `decompile -NoDefaults` 仍然提供输入**键**；普通
  拉取不添加参数。正式的**值**路径直接读取快速迭代存储
  中的源声明地址。单独的抑制集合提案仍然不必要。
- **3.5，`MeshYaw`：**已在 6.9 中针对文本写回处理；本次未变。
- **3.6/3.7/3.8，`MaterialParamValidMask`、DI 节点名称和 `EffectType`：**本次未触碰。
  它们分别涉及渲染器/编译状态、节点标识和一个缺失的读取方字段，
  而不是值写回操作。

**最初的支持表低估了后续覆盖率。**渲染器属性、设置和
分配现在都有断言支持的写入路径。`Settings` 反向使用生成器自己的映射
表；渲染器值的拼写由反编译器和 `pull` 通过
`WriteBack/DreamFXSourceValue.h` 共享。提取该共享实现暴露出一个回归：
来自先前 `switch` 的遗留 `break` 语句导致只有第一个属性被导出，
使五个 RoundTrip 夹具失败。独立的 `DescribeSystemFacts` 断言捕获了
对称导出比较可能遗漏的问题。

**资产级验收标准被更精确地实现：**在**原地重建同一资产**后，
任何事实都不得在其地址仍然存在时消失。这针对的是与 DFX8018 相同的
状态丢失类别，而不是要求两个单独创建的资产具有
相同的事实。被移除的整个结构和被折叠的重复常量会分别报告。6.8 中描述的默认值
恢复解释了为什么缺失的源条目在原地重建时不会全部变成丢失的事实。

**在判定标准上存在一项经实测确认的差异，并由 `feat/collapsed-copy-drift` 解决。** `SetInput`——即编辑器滑块所用的同一外部编辑 API——会把一个 EmitterUpdate 常量写入**两个**存储：`system-spawn` 与 `system-update`，而 DreamFX 重建只会物化其中一个。上一轮将此记录为一项未解决的差异。探针工件与完整日志位于 `Saved/DreamFX/CollapseProbe/`；真实系统导出为 `/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh`，编辑的是 `EmitterState` 的 `Loop Duration`。

1. **它确实会阻断，且措辞与预测一致。** 在滑块编辑之后，于同一会话中拉取再重建，会产生：

   ```text
   build safety: '…' held 187 fact(s), the rebuild produces 195; of the 1 fact(s) it no longer holds exactly, 1 are drift inside a structure the rebuild kept, 9 gained.
   lost | ri system-spawn Constants.Atlas2D_Mesh.EmitterState.Loop Duration (NiagaraFloat) : 0000A040 -> one copy fewer
   error DFX8018: … was not saved …
   ```

   原始日志：`CollapseProbe/A3-refusal-before-fix.log`。
2. **触发窗口比最初怀疑的更窄。** 该重复项只存在于写入它的那个会话中。即便是同一会话，也会在 `SaveSystem` 之后丢失其 `ri system-spawn` 副本：编辑前实测为 `187 facts / 0 system-spawn`，编辑后为 `188 / 1`，保存后为 `189 / 0`。新进程同样只会读取到一个。因此 **滑块 → Ctrl+S → 重建** 不会触发它（CLI A1/A2 构建均通过）。失败的路径是 **在编辑器中编辑某个值，使文本在同一会话中与存储中的值匹配（拉取或手动编辑），然后在不保存资产的情况下重建**。这正是当前讨论的双向工作流。
3. **编辑器入口点不会绕过该门控。** `FGenerateOptions::bForce` 跳过溯源；`bForceLossyRebuild` 允许丢失。Watcher、DreamGUI 桥接与 Adopt 只设置前者，如 `DreamFXGenerator.h` 所述。因此，在该会话中，一次由 `.dfs` 保存触发的 watcher 重建可能会被拒绝，且该入口点没有 CLI `-Force` 开关。
4. **修复方案：一条以值加脚本无关地址为键的规则 2 例外。** `FBuildSafetyGate::Compare` 会检查哪些 `(address,value)` 对得以保留。地址是 `Constants.<emitter>.<node>.<input>`；脚本标签只说明**该值存储在何处**。对于实测的 EmitterUpdate 常量，编辑会添加一个与 system-update 值同字节的 system-spawn 副本，而重建后只保留一个。地址与值均相同且仍然存在，意味着一个表示层面的重复项被合并，而非状态丢失。允许这种情况，但需输出详细的 `merged | …` 以及摘要 `a further N cop(y/ies) of a constant another script's store still carries collapsed into one -- not drift`。**只有相等的值才符合条件**。不同的值或真正缺失的值仍适用规则 2/3/4，并保持 `(missing)`、确定性与疑似标记不变。唯一的放宽是 `AfterValuesByKey` 查找，而非一概跳过名称中包含 `system-spawn` 的情况。
5. **语料库断言：** 新增的 `DreamFX.Corpus.WriteBack.CollapsedCopy` 直接针对 `Compare` 测试四组事实对，断言其判定结果，而非重新实现这些判定：两份相等的副本合并为一份 → 零阻断、一次合并；副本不相等且旧值已消失 → 一次确定性阻断；同样的不相等情况但输入被声明 → 在规则 3 不变的前提下零阻断；地址保留但整个主体缺失 → 一次 `(missing)` 阻断。`ModuleInput` 现在对重建时的 `0 lost / 1 collapsed` 进行断言，而不仅仅是报告：

   ```text
   ModuleInput: rebuilt in place … 172 fact(s) before, 171 after, 0 lost, 1 duplicate copy/copies collapsed, 0 structure(s) the text removed
   ```

6. **对称的拉取情形无需语义变更。** `WriteBack/DreamFXPull.cpp` 中的 `FindStored` 按名称去重：相等的副本合并为一个源值；不相等的副本会被拒绝，因为拉取无法确定引擎使用的是哪一个（拉取第 6 节）。因此相等的值会得到一个源字面量、一个重建后的存储值，随后是一次幂等拉取（`DFX7107 … already holds every value … No bytes written`，SHA256 与 mtime 不变）。不相等的值不会被拉取；构建会把源中预期的值写入其自己的存储。这些操作不会反复覆盖彼此的选择。**拉取语义未改变。**

**端到端实测：** 一次性使用的 `/Game/DreamFXProbe/NS_PullProbe` 已在验证后移除。这七种情形见拉取第 5b 节。结构重建成功，且没有任何安全门控诊断，也未产生 `Saved/DreamFX/BuildSafety/`；其在详细日志中的唯一警告是针对无界 SpawnRate 的既有 `DFX7102`。空操作 `pull -Apply` 保留了 SHA256 与 mtime。当文本已更改而资产未更改时，`DFX7115` 保留了**文本中较新的值**，且未触及文件。

<a id="7-reproduction"></a>

## 7. 复现

<a id="environment"></a>

### 环境

- 项目 `C:\CrossingVoid`；引擎 `D:\UnrealEngine-5.8.2`，并带有 DreamFX 引擎补丁。
- 插件 `C:\CrossingVoid\Plugins\DreamFX` 位于 `359b3b3`，分支为 `pr/engine-patch-and-writeback`。
- **复现前确保没有 UnrealEditor 正在运行。** 本研究开始时没有正在运行的实例。

<a id="commands-in-execution-order"></a>

### 命令，按执行顺序

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

<a id="artifact-locations"></a>

### 工件位置

| 内容 | 位置 |
|---|---|
| 导出文件，项目侧，通过 `DFX/Decompiled/` 忽略 | `C:\CrossingVoid\DFX\Decompiled\**`，共 59 个 `.dfs` 文件 |
| 导出副本与完整原始输出 | `C:\CrossingVoid\Saved\dfx-writeback-study\`：`decompiled\`、`raw-*.txt`、`build-S*.txt` |
| 原始/镜像事实文件，共五对 | `C:\CrossingVoid\Saved\DreamFX\*.{original,mirror}.facts` |
| 被忽略的镜像资产 | `C:\CrossingVoid\Content\Decompiled\**`、`C:\CrossingVoid\Plugins\AtlasFX\Content\Decompiled\**` |
| `-NoDefaults` 对比导出 | `Saved\dfx-writeback-study\NS_Effects1_Mesh.NoDefaults.dfs`、`NS_Atlas2D_Mesh.NoDefaults.dfs` |

<a id="change-audit-for-the-original-study"></a>

### 原始研究的变更审计

- **未更改任何原始资产。** 构建只写入了 `Decompiled/` 中的镜像。唯一的额外非文本导出副作用是 R3 嵌入式脚本提取到 `/Game/Decompiled/…/Scripts/NS_ScanScene_*.uasset`，同样位于 `Decompiled/` 之下。
- 该项目中已存在被修改的 `Plugins/AtlasFX/Content/Templates/NS_Atlas2D_*.uasset` 文件（mtime 17:47）以及 `Content/GameActor2D/Misaka/Material/DefAtk/FX_Defatk.uasset`（mtime 18:07），早于本研究 20:20 的开始时间。这些是先前已存在的更改。
- 在撰写本报告之前，该插件的 `git status` 是干净的。
- `D:\UnrealEngine-5.8.2` 未发生更改。
