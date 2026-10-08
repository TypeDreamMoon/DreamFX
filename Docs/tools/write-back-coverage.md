# 写回覆盖 / 缺口：实测清单

> 2026-10-08 记录。起因：需要回答两个问题 —— **DFX 能写回什么、写不回什么**，以及
> **手工在编辑器里改完之后，哪些改动同步不回文本**。
>
> 口径：只读原资产、只写文本。所有数字来自本机一次连续实测
> （UE 5.8.2 源码引擎 + DreamFX `359b3b3`），内容为 `/AtlasFX`、
> `/Game/GameActor2D`、`/Game/AssetMaterial` 三个路径下的全部 59 个 Niagara 系统。
> 产物与命令见本文末「复现步骤」。凡是**没实测到**的一律标「未验证」。
>
> 与 [`write-back-from-dreamgui.md`](write-back-from-dreamgui.md) 的分工：那篇写的是
> **DreamGUI/DUI 怎么做的、DreamFX 该照搬什么**；这篇只写 **DFX 自身的实测缺口**，
> 不重复它的设计论证。

---

## 0. 一句话结论

**读得下来，不等于写得回去。**

- **读**：59/59 系统导出成功，只有 6 个资产报了 17 处「语言表达不了」——
  按 `coverage` 自己的口径是 **100% 可表达**。
- **写**：同样的 59 个里抽 5 个重建，**4 个成功、1 个连编译都进不去**
  （`NS_ScanScene`，21 个 DFX4007）。
- 4 个成功的镜像 **L1/L2 全 PASS**，但拿 `asset-diff` 在**资产反射层**再比一次，
  **3 个有真实差异**，其中 1 个是**已证实的行为分歧**（详见 §3.5）。
- 已知的「网格渲染器材质参数→属性绑定被清掉、渲染成白块」这个具体缺口
  **已经不存在了**（§2.1 有反射级证据）。但**同一类事故换了个位置**继续发生：
  这次丢的是**模块输入的字面量**，而 `mirror-diff` 对它完全失明。
- 所以：**`mirror-diff`（L1+L2）不能作为「写回无损」的判据**，必须并上
  `asset-diff`；而 `asset-diff` 自己还有两个盲区（§3.9）。
  离「双向写回」还差的是：**一个资产级、能看见「文本不会写的东西」的安全闸**，
  以及**能读出被抑制输入值的正式读路径**（今天只有 `-NoDefaults` 这个被
  文档标成「诊断用、不保证可维护」的开关）。

---

## 1. 覆盖检查（`coverage`）

```
pwsh -File .skill/dfx.ps1 coverage -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
```

**扫描 59 个系统，导出 59，失败 0。**
逐条 gap（`coverage` 自己列出的「不可表达项」，原文摘录，共 6 个资产 17 处）：

| # | gap（原文） | 计数 | 所属资产 |
|---|---|---|---|
| 1 | `emitter '位置' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光' (version 77B881A6-…)` | 3 | 白色刀光、白色刀高光、TR刀光_System |
| 2 | `emitter '高光' inherits from '…/基础刀光.基础刀光'` | 3 | 同上 |
| 3 | `emitter '暗部' inherits from '…/基础刀光.基础刀光'` | 3 | 同上 |
| 4 | `emitter '刀光纹理' inherits from '…/基础刀光.基础刀光'` | 3 | 同上 |
| 5 | `emitter '突刺' inherits from '/Game/GameActor2D/SAO_Kirito/ExAsset/突刺.突刺' (version E752D719-…)` | 2 | Leng刀光、Leng纯色 |
| 6 | `emitter '突刺001' inherits from '…/突刺.突刺'` | 1 | Leng刀光 |
| 7 | `module 'UpdateScanSpriteSize' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |
| 8 | `module 'GetNormalizedDistance' has no resolvable script asset (scratch pad or missing reference)` | 1 | NS_ScanScene |

**读这份输出时必须知道的三件事**（都影响结论）：

1. `coverage` 统计的是 **`FDecompileResult::UnsupportedFeatures`**
   （`Source/DreamFXEditor/Private/Commandlet/DreamFXCommandlet.cpp:309-381`），
   也就是**反编译器自己承认的**表达不了的东西。
   **它不测生成侧**：`NS_ScanScene` 在这份报告里是 `ok (2 gap(s))`，
   但它的导出文本**一个字都重建不出来**（§3.3）。所以 `coverage` 的数字是
   「读的覆盖率」，不是「写回的覆盖率」。
2. 12 处继承 gap 全都是 **DFX8014 那条已知的「展平继承链」**，
   文档里已经写清了「丢的是链接不是内容」（`Docs/diagnostics/DFX8xxx.md:223-239`）。
   实测证实了这一点：`TR刀光_System` 4 个继承发射器的栈在镜像里**完整存在**，
   只是 `parent` 变成 `none`（§3.1）。
3. 这 59 个系统里 **`Stage` 块 0 个、事件处理器 >1 个的 0 个、自定义 stage 类 0 个、
   `RibbonRenderer` 0 个**，所以 DFX8015 / DFX8016 这一轮**一次都没被触发**
   —— 它们是「代码里存在、本内容未验证」的缺口，不是「已确认没有」。

---

## 2. 反编译取证

```
pwsh -File .skill/dfx.ps1 decompile-all -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine D:\UnrealEngine-5.8.2
# → 59 written, 0 failed，落在 C:\CrossingVoid\DFX\Decompiled\**
```

对 59 份 `.dfs` 做静态盘点（数字是「命中行数 / 命中文件数」）：

| 特征 | 命中 | 说明 |
|---|---|---|
| `MeshRenderer` 块 | 90 / 51 | 本工程是网格特效为主 |
| `SpriteRenderer` 块 | 58 / 25 | |
| `RibbonRenderer` 块 | 0 / 0 | **未验证** |
| `MaterialParameters =` | 12 / 12 | 2 个模板 + 10 个 Misaka 网格系统 |
| `Meshes =` | 90 / 51 | **全部是 JSON blob 形式，0 处路径数组** |
| `OverrideMaterials =` | 80 / 47 | 同上，全部 blob |
| `Material =` | 58 / 25 | 全部是 SpriteRenderer |
| `Bind … ->` | 98 / 50 | |
| `FacingMode =` | 153 / 58 | |
| `Alignment =` | 17 / 14 | |
| `SubImageSize =` | 0 / 0 | **本内容未用到，未验证** |
| `SortMode =` | 12 / 12 | |
| `LocalSpace =` | 128 / 54 | |
| `Determinism/AllocationMode/PreAllocationCount =` | 各 12 / 12 | |
| `QualityLevelMask` | 117 / 36 | |
| `SimTarget =` / `FixedBounds =` / `InterpolatedSpawning =` / `CalculateBoundsMode =` | 各 4 / 1 | 只有 `NS_ScanScene` 用到 |
| `WarmupTime =` | 59 / 59 | |
| `FixedTickDelta` / `FixedTickDeltaTime` | 59 / 59 | |
| `SystemState(` | 59 / 59 | |
| `OnEvent(` | 12 / 12 | 事件栈是实测走通的 |
| `DI<…>` | 12 / 12 | 带配置 JSON |
| `disabled ` 模块 | 41 / 13 | |
| `@版本号` | 430 / 56 | |
| `EffectType =` / `ModulePaths` / `RequiresPersistentIDs` | 0 / 0 | **未验证** |
| `Stage ` 块 / `Defaults = {` / `as <节点名>` pin | 0 / 0 | **未验证** |
| 真正的 `hlsl { }` 块 | 0 / 0 | 本内容没有退化的内联表达式 |

> 注意：每份导出头部都有 `// Inline arithmetic is not recovered: expressions come back as
> equivalent hlsl { } blocks.` 这句注释，它是**注释不是内容** —— 按行 grep `hlsl {`
> 会 59/59 全中，别把它当成「表达式退化了」。

### 2.1 「材质参数→属性绑定」这个已知缺口：已关闭

`/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh`（**手工搭的、没有文本源**，
就是出过白块事故的那一类）导出后，渲染器块是：

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

`Sheet ← User.Atlas.ResolvedTexture` 就在 `MaterialParameters` 那个 blob 里。
实现依据：反编译器在 R4 把「有结构但没语法的属性值」**原样当 JSON 字符串带出去**
（`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1009-1044` 的
`TryWriteJsonBlob`，调用点 `:1231-1237` / `:1249-1254`），生成侧对称地把它
**当 JSON 拼接回去**（`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:276-295`）。

**反射级证据**（比文本对比强：`asset-diff` 不经过导出器，是直接
`ExportText_InContainer` 反射读属性，见
`Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp:425-436`）：

```
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParameters = (AttributeBindings=((MaterialParameterName="Sheet",NiagaraVariable=(Name="User.Atlas",TypeDefHandle=(RegisteredTypeIn…
emitter Atlas2D_Mesh renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 4095
```

原资产和镜像**两侧逐字符相同**（`Saved/DreamFX/NS_Effects1_Mesh.{original,mirror}.facts`）。

> ⇒ 旧事故在当前引擎 + 当前补丁上**不复现**。这条可以从「已知缺口」里划掉，
> 但**同一类**缺口没关掉 —— 见 §3.4。

### 2.2 `Bind` 和 `MaterialParameters` 是两套机制，写法不同

- `Bind X -> Y;` 只覆盖 **`FNiagaraVariableAttributeBinding` 类型的字段**
  （`PositionBinding`、`ColorBinding`、`CustomSortingBinding`…）。
  读取实现只遍历 `FStructProperty` 且结构体恰好是
  `FNiagaraVariableAttributeBinding` 的属性：
  `Source/DreamFXEditor/Private/Adapter/DreamFXNiagaraAdapter.cpp:2662-2675`。
  反编译器反过来**故意跳过**所有以 `Binding` 结尾的属性键，因为那些会由 `Bind` 行表达：
  `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1178-1183`。
- `MaterialParameters` 是 **`TArray<FNiagaraMaterialAttributeBinding>`**，另一种结构体，
  走的是上面的 JSON blob 通路，**不是** `Bind` 行。

这条区分对写回很关键：**`MaterialParameters` 没有 `Bind` 那样的强类型写路径**，
它靠字符串 JSON 往返。本次实测往返成功（§5），但它的"类型安全"全靠
`FJsonSerializer` + 引擎的 JSON→struct 转换器，出错时报的是引擎的错，不是 DFX 的。

### 2.3 `Meshes` / `OverrideMaterials` 从来不按文档写的路径数组形式导出

`Docs/language/dfs.md:302-316` 写的是：

```c
Meshes            = ["/Engine/BasicShapes/Cube"];
OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
```

实测：**90 处 `Meshes`、80 处 `OverrideMaterials` 全部是 JSON blob，0 处路径数组。**
原因在 `TryWriteReferenceArray`（`DreamFXDecompiler.cpp:1055-1133`）：
它要求 `GetArrayElementReferenceField` 成功**且**每个元素的其它字段都等于默认构造的元素；
真实内容里元素带着 `mesh`/`explicitMat` 的导出文本形式（`/Script/Engine.StaticMesh'…'`），
`TryReadReferenceObject`（`:920`）读不出来，于是整体落到 blob 分支（`:1249-1254`）。

**不是丢数据**（blob 是无损的，5/5 L1 PASS），但：文档描述的形式和工具实际给出的
形式是两回事，写文档或写编辑器集成时不要按路径数组去解析。

---

## 3. 不可写回 / 会丢清单

> **这一章的现状（2026-10-09 第二轮）**：§3.1/§3.2 是已声明缺口，未变；§3.3（读写命名空间不对称）
> 未变；§3.4 的「写不回」已由 `pull` 解决（键要靠 `-NoDefaults`，值走正式读路径），
> §3.5 已解决（§6.9），§3.6/§3.7/§3.8 未变，§3.9/§3.10/§3.11 是工具自身的性质，未变。
> 逐项消化情况记在 §6.10。

### 3.1 继承链（已声明，DFX8014）

| 资产 | 丢失内容 | 后果 | 证据 |
|---|---|---|---|
| `SAO_Ausna/白色刀光`、`白色刀高光`、`SAO_Kirito/TR刀光_System` | 各 4 个发射器与 `/Game/…/基础刀光` 的父子链接 | 镜像里 `位置/高光/暗部/刀光纹理` 的栈**完整**，但父资产以后改了不会传到镜像 | facts：`emitter 位置 parent = /Game/GameActor2D/SAO_Kirito/ExAsset/基础刀光.基础刀光`（原）vs `emitter 位置 parent = none`（镜像） |
| `SAO_Kirito/Leng刀光` | 2 个（`突刺`、`突刺001`，父 `…/突刺.突刺`） | 同上 | 同上 |
| `SAO_Kirito/Leng纯色` | 1 个 | 同上 | 同上 |

**已实测确认「丢的是链接不是内容」**：coverage 报 4 gap 的 `TR刀光_System`，
L1 的判语是 `bodies identical; the original records 4 gap(s) its mirror does not have`。

### 3.2 模块脚本解析不出来（已声明）

`/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` 的
`GetNormalizedDistance`、`UpdateScanSpriteSize` 两个模块
`has no resolvable script asset (scratch pad or missing reference)`。
`decompile-all` 按 R3 把它们抽成了独立脚本资产：

```
extracted embedded script 'GetNormalizedDistance'   -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_GetNormalizedDistance
extracted embedded script 'UpdateScanSpriteSize'    -> /Game/Decompiled/GameActor2D/Origin_Miku/SceneScan/Niagara/Scripts/NS_ScanScene_UpdateScanSpriteSize
```

> 副作用提醒：`decompile-all` 并不只写文本 —— 这一步**在 `/Game/Decompiled/…/Scripts/`
> 下写了两个 `.uasset`**（`Content/Decompiled/` 已被 `.gitignore` 忽略）。说
> 「decompile-all 只读」的时候要带上这个例外。

### 3.3 ★ 整篇文本重建不进去（**未声明**）

`/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` ——
**这是本次唯一一个 `build` 直接失败的抽样**，21 个错误、全部同一类：

```
error DFX4007: Input 'Divide_Float.A' expects NiagaraFloat. 'NPC.NPC_Scan001.MaxScanRange'
  is neither a literal of that type nor a parameter reference -- parameter references start
  with a namespace such as User., Particles., Emitter., System. or Engine.
```

| 项 | 内容 |
|---|---|
| 出错的文本行 | `DFX/Decompiled/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene.dfs` 的 `:16 :65 :79 :80 :90 :145 :151 :173 :174 :184 :249 :255 :276 :277 :287 :352 :358 :378 :379 :389`（19 行、21 个错误——`:16` 和 `:80` 等各含 2 个） |
| 出错的名字 | `NPC.NPC_Scan001.{MaxScanRange, GridCellSize, Location, NormalizedTime}` —— 首段是**发射器名** `NPC`，不是 `User/Particles/Emitter/System/Engine/Module/…` |
| 读侧为什么写它 | 这是一个 `EInputValueMode::Linked` 的输入，反编译器把链接变量的**名字原样**吐出：`DreamFXDecompiler.cpp:655-656` `case EInputValueMode::Linked: return ToNameToken(Value.LinkedVariable.GetName().ToString());` |
| 写侧为什么拒它 | 参数引用的命名空间是一个**封闭白名单**：`Source/DreamFXEditor/Private/Generation/DreamFXValueLowering.cpp:39-44`（`Particles/Emitter/System/User/Engine/Module/Transient/StackContext/Local/Output/Parameters/DataInstance`）＋ `IsNamespacedName`(`:204-227`)；`NPC` 不在里面，于是落到 `:652-655` 的 DFX4007 |
| 后果 | 这个系统**没有任何文本可写回**。它在编辑器里改的东西永远同步不回文本，因为文本根本编译不过 |

**这是本次最硬的一条：同一个插件里，读侧产出了一个写侧自己拒绝的 token，
而 `coverage` 对它报 `ok`。**

### 3.4 ★ 模块输入的字面量不进文本（**未声明**）

这是 §2.1 那个白块事故的**同型缺口，换了个位置**。

反编译器有一条默认抑制规则（`DreamFXDecompiler.cpp:1730-1731`）：

> `R8: only inputs that differ from a pristine instance of the same module are printed.`

实测：**原资产的 RapidIterationParameters 里存着、文本里没有的值，比想象的多。**
把镜像和原资产做反射级 fact 集合比较（`asset-diff -DumpFacts`）：

| 资产 | 只在原资产里、镜像没有的 fact 总数 | 其中**模块输入常量** | 其中 **system-update 常量** | 其中其它 |
|---|---|---|---|---|
| `/AtlasFX/Templates/NS_Atlas2D_Mesh` | **0** | 0 | 0 | 0 |
| `/AtlasFX/Templates/NS_Atlas2D_Sprite` | **0** | 0 | 0 | 0 |
| `/Game/…/Misaka/Material/Sk1/NS_Effects1_Mesh` | 2 | 2 | 0 | 0 |
| `/Game/AssetMaterial/FXs/破空/破空灰尘` | 57 | 37 | 4 | 16 |
| `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | 93 | 61 | 9 | 23 |

原文样例（`Saved/DreamFX/破空灰尘.original.facts`，镜像侧**一条都没有**）：

```
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Rotation Angle (NiagaraFloat) = 0000F041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.InitializeParticle.Sprite Size (Vector2f) = 0000204100002041
ri Fountain ParticleSpawnScriptInterpolated Constants.Fountain.AddVelocity.Velocity Speed (NiagaraFloat) = 0000C841
ri system-update Constants.Fountain.EmitterState.MaxDistance (NiagaraFloat) = 00409C45
```

**怎么读这组数字（重要，别过度解读）**：

- 这些值的抑制**按设计是行为等价的** —— 文本里没写，重建时模块的默认值会补上同样的数。
  所以**不能**说「镜像一定渲染错了」。
- 但它们**确实不在这份文本里**。对「文本是唯一真源」的写回来说，这直接意味着两件事：
  1. **读不到**：`decompile` 的正式输出里，这个输入既没有值、也没有「我这里有个被抑制的值」的标记。
     想拿到它只有 `-NoDefaults`，而那个开关被文档明确标成
     *"Diagnostic only -- the result is not meant to be maintained"*（`.skill/dfx.ps1:127-129`）。
  2. **写不回**：行级写回 + 脏集合（`write-back-from-dreamgui.md` §3 的第 3 条）要落地，
     脏集合必须能寻址到这些输入；今天它们连**键**都不在文本里。
- 另外它是**脆的**：模块资产将来改默认值，镜像会跟着变，而原资产不会。

### 3.5 ★ 一处**已证实的行为分歧**：`MeshYaw` −90 → 0（**未声明**）

`/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` 是 `asset-diff` 判 `DIFF` 的
三个资产里差得最小的：`136 fact(s) vs 136`，**只有 2 对差异**，而且两对是同一件事
（spawn / update 两个脚本各一条）：

```
ORIG  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 0000B4C2
MIRR  ri Atlas2D_Mesh ParticleUpdateScript Constants.Atlas2D_Mesh.Sprite_Atlas_Size.MeshYaw (NiagaraFloat) = 00000000
```

`0xC2B40000` = **−90.0**，`0x00000000` = **0.0**。**同一个变量、两个不同的值。**

链条如下（每一步都有实测）：

1. 文本里**没有** `MeshYaw`：`NS_Effects1_Mesh.dfs:87` 只有
   `/AtlasFX/Modules/Sprite_Atlas_Size(DepthScale = User.DepthScale);`
2. 但**原资产存着 −90**（上面的 fact）。
3. `-NoDefaults`（强行打印全部输入）导出的却是 **0**：

   ```c
   // Saved/dfx-writeback-study/NS_Effects1_Mesh.NoDefaults.dfs
   /AtlasFX/Modules/Sprite_Atlas_Size(
       MeshSize = (92.800003, 64.0),
       MinSize = (92.800003, 64.0),
       DepthScale = User.DepthScale,
       UniformScale = 1.0,
       MeshYaw = 0.0        // <-- 资产里的 rapid-iteration 存的是 -90
   );
   ```

   同一份 `-NoDefaults` 跑 `/AtlasFX/Templates/NS_Atlas2D_Mesh` 得到**一模一样**的
   `MeshYaw = 0.0` —— 也就是说**读路径区分不出**「真的是 0」和「存的是 −90」。
4. 两个模板 + 这个 Misaka 系统的 fact 对比里，**只有这一个输入**出现值分歧；
   `MeshSize/MinSize/UniformScale` 两侧完全相同。

**已证实**：文本不是原资产模块输入状态的完整描述；读侧的 `MeshYaw` 值与资产的
rapid-iteration store 不一致；镜像因此与原资产在这一项上不同。
（`asset-diff` 未加 `-NoCompile` 时会**强制重编译两侧并等待**
——`DreamFXCommandlet.cpp:816-827`——所以这不是「没编译造成的陈旧值」。）

**未验证**：Niagara 运行时实际取的是 pin 值还是 rapid-iteration 值，因此
**镜像是否真的画得不一样，未验证**。要证这一步需要 L3 / 目视。

旁证（不是证据，但同向）：AtlasFX 自己的源码注释记着这个数就是当年「看不见」的真因 ——
`Plugins/AtlasFX/Source/AtlasFXEditor/Private/AtlasFXPaperZDPreview.cpp:63`：
> 最初「看不见」的真因是网格模板里的 MeshYaw = -90 把面片转成了侧对镜头（**游戏里也一样是一条缝**），模板已改回 0

**这是本次唯一一条能把「文本写回」和「画面变化」连起来的线索，建议作者优先查它。**

### 3.6 ★ 渲染器 `MaterialParamValidMask` 15 → 0（**未声明**）

`/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` 的 `位置` 发射器（4 个网格渲染器之一）：

```
ORIG  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 15
MIRR  emitter 位置 renderer 0:NiagaraMeshRendererProperties MaterialParamValidMask = 0
```

同一个资产的 `ri` 侧同步缺了一整组动态材质参数常量：

```
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 2 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 3 (NiagaraFloat) = 00000000
ORIG  ri 位置 ParticleSpawnScriptInterpolated Constants.位置.DynamicMaterialParameters.Index 0 Param 4 (NiagaraFloat) = 00000000
```

文本里**有** `DynamicMaterialParameters@1.1(…)` 这个模块调用
（`TR刀光_System.dfs:528`），所以模块**在**，缺的是渲染器侧的 valid-mask 与
对应的 RI 常量。

**未验证**：`MaterialParamValidMask` 是不是「load/compile 时会重算的缓存」。
它不是 `CPF_Transient`（否则会被 `AppendPropertyFacts` 跳过，
`DreamFXAssetFacts.cpp:85-89`），所以它**是**被序列化的资产状态；
但它是否在重编译后自愈，没测。**后果未验证，建议与 §3.5 一起查。**

### 3.7 ★ 曲线 / DI 节点名漂移（**未声明**）

`破空灰尘` 与 `TR刀光_System` 的 `compiled … di '…'` 一族 fact 两侧对不上：

```
ORIG   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.Scale Alpha.FloatCurve'    … calls: SampleCurve
ORIG   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve005.FloatCurve' … calls: SampleCurve
MIRR   compiled Fountain  ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve.FloatCurve'  … calls: SampleCurve
MIRR   compiled Fountain001 ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve004.FloatCurve' … calls: SampleCurve
MIRR   compiled 刀光纹理   ParticleSpawnScriptInterpolated di 'Emitter.FloatFromCurve006.FloatCurve' … calls: SampleCurve
```

`SampleCurve` 两侧都调用，所以**行为大概率一致**；漂移的是
**承载曲线的模块节点名**（`Scale Alpha` → `FloatFromCurve`，
`FloatCurve001` → `FloatFromCurve006`）。这正是 `dfs.md:120-126` 里
`as <name>` 要解决的问题 —— 而本次 59 份导出里 **`as` pin 一处都没有**。

**未验证**：节点名漂移是否会让任何 `Output.<节点>.<值>` 链接悬空。本次没有看到
由此产生的编译错误（L2 全 PASS），但 `as` 的缺席是事实。

### 3.8 ★ `EffectType`：读写表不对称（代码级，本内容未触发）

`DreamFXDecompiler.cpp:1266-1274` 自己写着：

> Must stay in step with `EmitterSettings` on the generator side -- the two tables are the two
> halves of one contract, and a setting present in only one of them is a silent loss.

实测这两张表：

| | 生成侧 | 读侧 |
|---|---|---|
| 发射器 | `DreamFXGenerator.cpp:408-427`，**12** 项 | `DreamFXDecompiler.cpp:1281-1298`，**12** 项 |
| | SimTarget / LocalSpace / Determinism / RandomSeed / AllocationMode / PreAllocationCount / InterpolatedSpawning / CalculateBoundsMode / FixedBounds / RequiresPersistentIDs / Enabled / QualityLevelMask | **逐项一致** |
| 系统 | `DreamFXGenerator.cpp:396-406`，**5** 项：`EffectType` / `WarmupTime` / `FixedBounds` / `FixedTickDelta` / `FixedTickDeltaTime` | `DreamFXDecompiler.cpp:1300-1313`，**4** 项：**没有 `EffectType`** |

⇒ **`EffectType` 能写、读不出来**。任何设了 `EffectType` 的系统，
导出→重建会**静默**丢掉它。本内容 0 处 `EffectType =`，所以**没被触发**；
但这是一处真实的不对称，修起来是一个表项的成本。

### 3.9 验证工具自身的盲区（会掩盖上面这些问题）

| 盲区 | 说明 |
|---|---|
| **L1 是「导出 vs 导出」** | 两侧都是同一个有损导出器的输出，所以**读侧的丢失在 L1 里结构性不可见**。`DreamFXAssetFacts.h:9-18` 自己写着这句话。本次 §3.4/§3.5/§3.6/§3.7 全部是 L1 PASS 却真实存在的差异 |
| **L2 只证明「编得过」** | 值写错、写丢，编译器不报错 |
| **`asset-diff` 不比较系统级属性** | `DescribeSystem`（`DreamFXAssetFacts.cpp:246-437`）里 `AppendPropertyFacts` 只用在**发射器数据**(`:406`)、**渲染器**(`:430`)、**stage**(`:396`)、**DI**(`:486`) 上 —— **从未**对 `UNiagaraSystem` 本身调用。所以系统级的 `WarmupTime` / `FixedBounds` / `bFixedTickDelta` / `EffectType` / `Determinism` 丢了**也不会有 fact 差异** |
| **`asset-diff` 看不见父链** | `VersionedParent` 在 `EmitterDataSkip` 里（`DreamFXAssetFacts.cpp:258`）。本次 `parent` fact 之所以出现，是适配器另一条路读的；`VersionedParentAtLastMerge` 则完全没比 |
| **`mirror-diff` 的退出码把「没建过」算成失败** | `return Failed + Missing + CompileFailed;`（`DreamFXCommandlet.cpp:746`）。本次 5 passed / 0 failed / 54 never built → **exit 54**，`dfx: FAILED`。当成 CI 门禁会误报 |

### 3.10 已知会误报的差异（别去追）

`asset-diff` 有三处差异是**工具自身的归一化假象**，不是写回丢的：

1. **`<self>` 前缀**：`emitter Fountain renderer 0:… Meshes = ((Mesh="/Script/Engine.StaticMesh'<self>_Mesh.破空灰尘_Mesh'"))`（原）
   vs `…'StaticMesh'/Game/AssetMaterial/FXs/破空/破空灰尘_Mesh.破空灰尘_Mesh'`（镜像）。
   `SelfPackage` 两侧不同（镜像在 `/Game/Decompiled/…` 下），归一化结果自然不同 —— **同一个绝对路径**。
2. **`EditorData` / `EditorParameters`**：`…破空灰尘:Fountain.NiagaraEmitterEditorData~` vs
   `…破空灰尘:Fountain~.NiagaraEmitterEditorData~`，编辑器专用对象的命名归一化。
3. **渲染器 `*Binding` 上的 `bBindingExistsOnSource=True→False` 与 `MaterialParamValidMask`**：
   `bBindingExistsOnSource` 是编译期推导的缓存，逐条报差异属噪声
   （`MaterialParamValidMask` 不同 —— §3.6 那条要单独看，不能一起忽略）。

### 3.11 本内容未验证的项（明确列出，别当成「没问题」）

`Stage` 块、事件处理器 >1 个 / 事件生成器、自定义 C++ stage 类、`RibbonRenderer`、
`SubImageSize`、`MeshRenderer` 的 `Meshes` 元素自定义 pivot/scale/LOD、`EffectType`、
`ModulePaths`、`RequiresPersistentIDs`、`as <节点名>`、`Defaults = {}` 块、
真正的内联 `hlsl { }`、`.dfe` 的 `from` 引用、per-device-profile 的平台集覆盖
（`QualityLevelMask` 之外的 `FNiagaraPlatformSet`）。

---

## 4. 可写回清单（逐类 + 证据）

判定口径：**「导出里在」＋「L1/L2 PASS」＋「反射级 fact 里也在」** 三样齐全才算
「实测可写回」；只满足前两样标「文本级通过，反射级未逐项验证」。

| 类别 | 结论 | 证据 |
|---|---|---|
| **渲染器 · `MaterialParameters`（材质参数→属性绑定）** | ✅ 实测可写回 | `NS_Effects1_Mesh.dfs:94`；镜像 facts 中 `MaterialParameters = (AttributeBindings=(…"Sheet"…"User.Atlas"…"ResolvedTexture"…))` 与 `MaterialParamValidMask = 4095` 两侧相同（§2.1） |
| **渲染器 · `Meshes` / `OverrideMaterials`** | ✅（JSON blob 形式） | `NS_Effects1_Mesh.dfs:95-96`；facts 相同；`NS_Atlas2D_Mesh` 137/137 facts 全同 |
| **渲染器 · `Bind X -> Y`** | ✅（文本级 + facts 同） | 98 行/50 文件；`NS_Atlas2D_Mesh.dfs:100`；镜像 re-export 逐行相同 |
| **渲染器 · `Material`（Sprite）** | ✅ | `NS_Atlas2D_Sprite.dfs:91`；该资产 131/131 facts 全同 |
| **渲染器 · `FacingMode` / `Alignment` / `SortMode` / `bOverrideMaterials` / `bSubImageBlend` / `SortOrderHint`** | ✅（facts 逐项比对） | `FacingMode` 153 行/58 文件；`TR刀光_System` 渲染器 facts 除 `*Binding` 缓存与 `MaterialParamValidMask` 外全部一致 |
| **发射器设置（12 项）** | ✅ 表逐项对齐 + 实测 | 见 §3.8 对照表；内容侧 `LocalSpace` 54 文件、`Determinism`/`AllocationMode`/`PreAllocationCount` 各 12、`QualityLevelMask` 36、`RandomSeed` 12 |
| **系统设置（`WarmupTime`/`FixedBounds`/`FixedTickDelta(+Time)`）** | ✅ 文本级（facts 不比系统级属性，见 §3.9） | `WarmupTime =` 59/59、`FixedTickDelta` 59/59 |
| **模块栈 · 模块齐全 / 顺序** | ✅ | `TR刀光_System` 166 个 view model、43 次 `AddModule`；5/5 L1 `bodies identical` |
| **模块栈 · 输入值（**非默认的**）** | ✅ | `破空灰尘` L1 PASS；`AddVelocity(Velocity = (-200.0, 0.0, 0.0))` (`:60`) 与 fact `…Velocity (Vector3f) = 000048C3…` 一致 |
| **模块栈 · 动态输入 / 内联表达式** | ✅（本内容未出现真 `hlsl {}`，走的是 `DynamicInputToSource`） | `FloatFromCurve(… curve { … })`、`Divide_Float(A = …, B = …)` 都在文本里且 L1 PASS |
| **模块栈 · `disabled`** | ✅ 文本级 | 41 行 / 13 文件 |
| **模块栈 · `@版本` pin** | ✅ | 430 处 / 56 文件；`ParticleState@1.1`、`DynamicMaterialParameters@1.1`、`ScaleColor@1.0` |
| **`OnEvent` 事件栈（单个处理器）** | ✅ 文本级 | 12 文件含 `OnEvent(`，coverage 0 处事件 gap |
| **用户参数 + DI 配置 JSON** | ✅ | `NS_Effects1_Mesh.dfs:13` 的 `DI<SpriteAtlas> Atlas = "{…Flipbook…FrameRects…ResolvedTexture…}"`；`user …` facts 两侧一致 |
| **`Set Parameters` 折叠赋值 / `Defaults` 块** | ⚠️ 赋值文本级通过；`Defaults = {}` 本内容 0 处，**未验证** | 3 次 `AddSetParametersModule`（`TR刀光_System`） |
| **`.dfe` 的 `from` 引用** | ⚠️ **未验证**（本内容无 `.dfe` 使用） | 文档说这是单向语法糖，导出必然是自包含的（`Docs/language/dfe.md:40-48`） |

---

## 5. 固定点抽样结果

抽样 5 个（文档警告过批量重建会卡编辑器：24 系统 = 237 编译任务，所以只取 5）。
每个 `build` 一次编辑器启动，5 个源各自 `-Force` 重建。

| # | 资产 | 挑它的理由 | `build` | L1 | L2 | `asset-diff` | 第一处差异 |
|---|---|---|---|---|---|---|---|
| S1 | `/AtlasFX/Templates/NS_Atlas2D_Mesh` | 网格渲染器 + 材质参数绑定；已知 PASS 的基线 | ✅ 0 error | **PASS** | **PASS** (UpToDate) | **SAME** (137/137 facts) | 无 |
| S2 | `/Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh` | **白块事故本体**：手工搭的、无文本源的网格系统 | ✅ 0 error | **PASS** | **PASS** (UpToDate) | **DIFF** (136/136，2↔2) | `Sprite_Atlas_Size.MeshYaw`：原 `0000B4C2`(−90) vs 镜像 `00000000`(0) |
| S3 | `/Game/GameActor2D/SAO_Kirito/ExAsset/TR刀光_System` | 手工搭的；4 个发射器继承父资产；4 个网格渲染器 | ✅ 0 error | **PASS**（判语：`bodies identical; the original records 4 gap(s) its mirror does not have`） | **PASS** (UpToDate) | **DIFF** (751→654，93↔22) | 报告首条是 `compiled 刀光纹理 … di 'Emitter.FloatFromCurve.FloatCurve001'` vs 镜像 `'…FloatFromCurve006.FloatCurve'`（§3.7）；另有 `emitter 位置 parent` 原 `/Game/…/基础刀光.基础刀光` vs 镜像 `none`（已声明，§3.1）、61 条模块输入常量（§3.4）、`MaterialParamValidMask 15→0`（§3.6） |
| S4 | `/Game/GameActor2D/Origin_Miku/SceneScan/Niagara/NS_ScanScene` | 唯一一个有「抽出来的嵌入脚本」的系统 | ❌ **21 error(s)，0 built** | —（镜像never built） | — | —（MISSING） | 全部是 `error DFX4007`，首个在 `NS_ScanScene.dfs(16,30)`：`'NPC.NPC_Scan001.MaxScanRange'` |
| S5 | `/Game/AssetMaterial/FXs/破空/破空灰尘` | `/Game/AssetMaterial` 批次代表 | ✅ 0 error | **PASS** | **PASS** (UpToDate) | **DIFF** (444→403，57↔16) | 37 条模块输入常量 + 4 条 system-update 常量只存在于原资产 |

**汇总**：`mirror-diff` → **L1 5 passed / 0 failed / 54 never built**，
**L2 5 compiled clean / 0 failed**（`dfx` 报 `FAILED (exit 54)`，因为退出码含 `Missing`，见 §3.9）。
`asset-diff` → **2 same, 3 different, 54 missing**。

**这就是本篇报告的核心结论**：`mirror-diff` 说 5/5 通过，`asset-diff` 说其中 3 个不一样。

额外的逐行证据：把 S2 的镜像重新导出后与原导出逐行 `Compare-Object`，
**103 行 vs 103 行，唯一差异是第 1 行的 provenance 注释** ——
连 `MaterialParameters` / `Meshes` / `OverrideMaterials` 三行 blob 都逐字节相同。
所以 S2 的问题**不是文本往返**（那部分完美），而是**文本本身没带 `MeshYaw`**。

---

## 6. 对实现的建议（按投入产出排序）

> `write-back-from-dreamgui.md` §3 已经排过一版顺序（① build 侧安全闸 →
> ② `pull` → ③ 行级写回 + 脏集合 → ④ 配合编译抑制）。
> 本章**不重复**那套论证，只补**实测支持什么、实测要求改哪里**，
> 并按本次测量重排其中的 ① 和 ②。

### 6.1 安全闸：用 `DescribeSystemFacts`，不要用文本（最高优先）

`write-back-from-dreamgui.md` §3① 建议把 `DFX8010` 那道闸搬到 `build` 上。
**实测支持这个方向，但指出它是错的实现基础**：`DFX8010` 现有实现依赖
`Result.UnsupportedFeatures`，而本次 3 个「`asset-diff` DIFF」的资产
**`UnsupportedFeatures` 一个都没报**（S2 报 0、S5 报 0、S3 报的 4 条是已声明的父链）。
文本级的闸看不见任何一条 §3.4–§3.7 的丢失。

**具体做法**：`Source/DreamFXEditor/Private/Diff/DreamFXAssetFacts.cpp` 里的
`DescribeSystemFacts` 已经是现成的资产级探测器（反射遍历，不经过导出器）。
在 `build` 打开目标资产之后、写入之前：
1. 记下现在的 fact 集合；
2. 算出「这份文本将写入什么」；
3. 报告 fact 集合里**文本不会产生**的项，列出清单并拒绝覆盖，要盖得显式 `-Force`。

投入：一个复用已有工具的调用点。收益：把 S2/S3/S5 三类静默丢失一次性堵住。

### 6.2 给 `asset-diff` 补系统级属性（很小，建议顺手做）

`DescribeSystem`（`DreamFXAssetFacts.cpp:246`）从未对 `UNiagaraSystem` 本身
调用 `AppendPropertyFacts`。加一行对 `UNiagaraSystem::StaticStruct()` 的调用，
`EffectType` / `WarmupTime` / `FixedBounds` / `bFixedTickDelta` / `bDeterminism`
就有了独立见证。同时把 `VersionedParent`(`:258`) 从 skip 里挪出来，
父链丢失就不用靠适配器旁路了。

### 6.3 修读写不对称：`<Emitter>.<Node>.<Input>`（小，但它是「完全写不回」）

`IsNamespacedName`（`DreamFXValueLowering.cpp:204-227`）的白名单（`:39-44`）
不认首段是**发射器名**的引用，而反编译器就是这么写的
（`DreamFXDecompiler.cpp:655-656`）。两条路选一条：

- **放宽**：`Emitter.<name>` 形式的别名本来就被 `Bind` 接受
  （见 `DreamFXGenerator.cpp:1490-1502` 的 DFX4026 注释），
  `Output.<node>.<value>` 也已经在白名单里；把「本系统里的发射器名 / 节点名」
  也当合法首段，比加白名单项更贴近实际。
- **降级**：如果确实表达不了，读侧就必须把它记进 `UnsupportedFeatures`
  而不是吐出写侧不认的 token —— 否则 `coverage` 会继续对它报 `ok`。

无论走哪条，`coverage` 都不该再把这种资产报成可表达。

### 6.4 把「被抑制的输入」变成一等公民（为 ② `pull` 和 ③ 脏集合铺路）

`write-back-from-dreamgui.md` §3② 的 `pull` 和 §3③ 的脏集合都需要
「**读到某个输入的当前值，即使它等于默认值**」。今天这件事只有
`-NoDefaults` 能做，而它被契约性地排除在「可维护输出」之外
（`.skill/dfx.ps1:127-129`）。实现其实已经在：
`FDecompileOptions::bIncludeDefaultedInputs`
（`DreamFXCommandlet.cpp:265, 1100`）。

建议把那句「Diagnostic only」改掉，明确「**这是写回路径的读取模式**」，
并把「普通模式与 `-NoDefaults` 的差集」定义为**抑制集**：
脏集合写回时，抑制集里的项**一律不动**，只写给用户真正改过的键。
这一步不做，③ 的行级写回一定会在第一次 flush 时把 §3.4/§3.5 的东西冲掉。

### 6.5 先把 §3.5 的 `MeshYaw` 查清楚（诊断，不是实现）

它是本次唯一一条「文本 vs 资产」有**确定值差**、且旁证（AtlasFX 源码注释）
说这个数直接决定画面对不对的线索。查的顺序：
1. 目视 / L3 比一次 `NS_Effects1_Mesh` 原资产与镜像（`/Game/Decompiled/…/NS_Effects1_Mesh`）；
2. 若确有差异 → 在读路径上加一条断言：**读到的输入值 vs 该输入的 rapid-iteration 存值**，
   不一致就报 gap。这一类不一致（pin 与 store 打架）值得一个专门的诊断号。

### 6.6 门禁口径的修正（零成本，改文档/改退出码）

- `coverage` 的数字要明确写清是「**读**的覆盖率」。或者加一列「可重建性」——
  本次 59 个里已经有 1 个（`NS_ScanScene`）读完但重建不了。
- `mirror-diff` 的退出码含 `Missing`（`DreamFXCommandlet.cpp:746`），
  部分构建的树会被判 FAILED。要么把 `Missing` 拆成独立退出码，要么在 CI 里
  先把树建全，要么加一个「只统计已建镜像」的开关。
- **把 `asset-diff` 并进固定点门禁**。`write-back-from-dreamgui.md` §4 的验收标准
  现在只有「无操作写回逐字节不变」；实测证明这一条**不够**（本次 3 个资产
  在「文本逐字节可复现」的同时资产级并不相同）。建议验收标准改成两条：
  L1 逐字节 + `asset-diff` fact 集合相等。

### 6.7 文档订正（两处与实测不符）

| 位置 | 现状 | 实测 |
|---|---|---|
| `Docs/diagnostics/DFX5xxx.md:186-202`（DFX5093） | 「`MaterialParam` is reserved syntax and is not implemented in v1」/「Fix. Not available.」 | 保留语法（`MaterialParam X = Y;`）确实没实现，但**属性形式 `MaterialParameters = "…json…";` 是通的**，而且反编译器产出的就是这一种。建议在 DFX5093 里点明「有另一条能用的通路」 |
| `Docs/language/dfs.md:302-316` | `Meshes = ["/Engine/BasicShapes/Cube"]` | 反编译器 90/90 都产出 JSON blob，0 处路径数组。写编辑器集成 / 写解析器时按 blob 处理 |

### 6.8 已实现：build 侧安全闸

> 分支 `feat/build-safety-gate`，诊断 `DFX8017`。§6.1 按「用 `DescribeSystemFacts`、不要用文本」
> 落地，§6.2 的系统级属性补盲一并在内。**下面这一节是新增实测，不改变 §1–§7 原有内容。**

**语义（第二轮：只抓「文本管不到的地方发生的漂移」）**

> 第一版（`66db8ce`）是一句「重建后没完全持有的 fact 一律拦」，实测立刻打在正常写文本的人身上：
> 把文本里**已经写着**的 `UniformScale` 从 1.0 改成 2.0 会被报成「丢了 `0000803F`」，删一个模块也被拦。
> 第二版（`b3ca74b`）改成**一个结构一个结构地比，先问文本再指控**：

- **落盘之前**：第一次写资产之前记下 fact 集合，编译完成、provenance 盖章之后、`SaveSystem`
  **之前**再读一次（`DescribeSystemFacts`，反射级、不过导出器；多重集差，不是「第一处不同」）。
- **规则 1｜只比两侧都有的结构**：发射器／模块节点／渲染器只在一侧出现 ⇒ 那是文本增删的，
  **有意，忽略** —— 所以「删掉一个模块」干净通过（下表第 3 行）。
- **规则 2｜范围内事实消失 ⇒ 拦**：结构两侧都在，它下面某条 fact 却没了 ⇒ 文本没有表达它的能力，
  存盘就是销毁它（`MaterialParameters` 丢绑定就是这一形态）。
- **规则 3｜范围内事实值变了 ⇒ 只有文本没声明该输入时才拦**：判定取自解析结果，**不猜名字** ——
  设置类取计划要写的那份 JSON 的顶层键（写进去的就是声明的），模块输入取 AST 里那条模块调用的
  实参名 ＋ 该输入的 Niagara 变量名，二者在 `ApplyStack` 里对着**引擎真正分配的节点名**登记
  （`as <name>` 之后的名字只有那里存在；「模块资产名 + 计数」这种猜法是被明确拒绝的）。
- **规则 4｜判不了 ⇒ 按拦，但措辞分开**：明细行尾带
  `[deterministic drift (the text does not name this input)]`（查过声明，文本没写它）或
  `[suspected drift (cannot tell)]`（这个结构根本没有可查的声明记录）。
- **自成一体的族**：`di` / `compiled` / `user` / 事件处理器 / Stage /「emitter 无数据」这些 fact
  在文本里没有可登记的地址，按「整条即结构」处理 ⇒ 落在规则 1（差异＝文本增删整条）。这是有意写下的
  限制，理由在代码注释里：`di`/`compiled` fact 不带节点名，Stage 的内部属性与 `Stage(...)` 的实参
  没有一对一拼写。
- `-Force` ⇒ 照常写入，同一份明细改以 **warning** 级别进日志（标记照旧）。
- 目标资产**原本不存在** ⇒ 不拦；`verify` / `-NoSave` 不拦（不写盘）。
- 只比**稳定**事实：剥掉 `bBindingExistsOnSource` / `bIsCachedParticleValue`（§3.10 的编译期缓存）
  与 `compiled … writes:` 的空列表（PostLoad 丢弃缓存 VM 的产物）。两处**只改闸**，
  `asset-diff` 的输出与 §3/§5 引用的数字原样不动。
- **新增采集面**：`UNiagaraSystem` 自身属性进 fact 集合（§3.9 的盲区 / §6.2），形如
  `system WarmupTime = …`；skip 表只排除编译器产物、编辑器簿记、身份与 R3 抽出的 scratch pad。
- **出口只有 `-Force`**，且它与 `bForce` 是**两个开关**：Adopt、保存时重建（watcher）、DreamGUI
  bridge 只设后者 —— 按新语义，它们的正常文本改动会自动放行，只有真会丢东西的才拦。
- 产物：`Saved/DreamFX/BuildSafety/<资产>.{before,after,lost}.facts`（每次触发都写；
  `-Force` 之后资产已不含 before 状态，这份是唯一记录）。

**实测**（同一个编译产物；每条命令一次编辑器启动，日志在 `Saved/DreamFX/gate-probe/runs5|runs6/`）

| # | 命令 | 结果 |
|---|---|---|
| 1 | `dfx build <镜像导出＋一行注释>`，无 `-Force` | `1 built`，0 error / 0 warning：**无误报**。`-LogCmds=LogDreamFX Verbose -FullStdOutLogOutput` 下可见闸的判语：`build safety: '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' still holds all 186 fact(s) after the rebuild.` |
| 2 | 文本里已写明的 `MeshYaw = -90 → -45`、`UniformScale = 2.0 → 3.0` | **干净通过**，exit 0，日志里没有任何 `build safety` 行 —— 「改值也拦」已消除 |
| 3 | 整条 `Sprite_Atlas_Size(...)` 调用删掉 | **干净通过**，exit 0（再写回去也通过：新增不算丢）|
| 4 | 同一条调用**只删两个实参** | **拦下** 4 条，行尾均为 deterministic：`… MeshYaw : 000034C2 -> 00000000`、`… UniformScale : 00004040 -> 0000803F` |
| 4b | 同上 ＋ `-Force` | 写入成功，同 4 条改以 warning 列出（标记照旧），exit 0 |
| 5 | 系统探针：`WarmupTime = 5.0` → `1.0`（文本里声明了） | **干净通过**，exit 0 |
| 6 | `NS_Effects1_Mesh` 的导出（`Name=` 去掉 `Decompiled/`）指向**原资产** | **拦下** 2 条：`… MeshYaw : 0000B4C2 -> 00000000`（spawn/update 各一条），带 deterministic 标记；原 `.uasset` 的 SHA256 与 mtime 不变 |
| 7 | `破空灰尘` 同上 | **拦下** 8 条：4 × `… ScaleColor.Scale RGBA : 0000803F… -> (missing)` ＋ 4 × `… EmitterState.Min/MaxDistance : 00409C45/00000000 -> (missing)`（整条消失，不带标记）；原 `.uasset` 不变 |
| 8 | `DFX/Templates/NS_Atlas2D_Mesh.dfs` 的副本（手工维护的模板源） | **拦下** 1 条：`emitter Atlas2D_Mesh renderer 0:… MaterialParameters` → `(AttributeBindings=,…)` 全空，带 deterministic 标记 —— 就是白块事故 |

**读数注意（一处订正）**：§3.4 的「37+4 条只在原资产里」是**原资产 vs 镜像**的差集；
**原地重建**（`CleanUpStaleParameters` 之后按文本重填）实测只丢上表第 7 行的 8 条 ——
其余 29 条的值恰好等于模块默认值，重建时被默认值补回、fact 相同。所以「文本没带它们」是真的，
「原地重建会丢 37 条」不是：两者不是同一个比较。

**仍然会拦、且是作者可能反对的地方**：**把某个实参从文本里删掉**（模块还在）按规则 3 是「文本不再声明
这个输入」⇒ 仍然拦（上表第 4 行）。这与「删掉整个模块干净通过」并不矛盾：删模块是文本在说「这块我不要了」，
删实参是文本没说这个输入该怎么办，它的旧值就是文本管不到的东西 —— 但如果你希望「字段回落到模块默认值」
也算有意，放宽点仍是唯一的 `Declared.Names` 那一处判断。

**第 8 行是本次最有价值的副作用**：手工维护的 `DFX/Templates/NS_Atlas2D_Mesh.dfs`
**不含** `MaterialParameters = "{…}"` 那个 blob（镜像导出里有），所以按现在文档写的方式
重建模板 = 清掉 Sheet 绑定 = 白块事故。闸把它拦下了，代价是那条工作流现在要显式 `-Force`
（模板头注释本来就要求随后再跑一次 `AtlasFXSetup`）。`ci.ps1` 用的是 `build -All -Force`，
所以 CI 不受影响：那一路的损失以 warning 进日志。

**顺带修的一处（不然仓库自带的 corpus 会红）**：新增的系统级 fact 让 `dfx corpus` 立刻红了 ——
`RoundTrip/ValueModes.dfs` 声明 `FixedBounds = box(-50…50)`，重建回来的是引擎默认的 ±100。
根因不在闸：写侧只写箱子、从不开 `bFixedBounds`，读侧又只在开关为真时才打印箱子，
于是「声明了固定包围盒」这条设置看着生效、其实一直悬空（其它 fixture 全都声明 ±100 = 默认值，
这个洞就在 corpus 里藏住了）。修法是一行映射：系统 `FixedBounds` 带一个 companion bool
（`FSettingMapping::CompanionBool`）。**副作用**：本工程 DFX 源里声明了系统 `FixedBounds` 的
只有 `DFX/Templates/NS_Atlas2D_Mesh.dfs` 与 `NS_Atlas2D_Sprite.dfs`（±300），
它们下一次重建会真正启用固定包围盒 —— 这正是那两行的本意，但需要作者知情。
修后 `dfx corpus`：**55 passed / 0 failed**。

**写入的东西（自证）**：只创建了两个一次性探针资产（`/Game/DreamFXProbe/NS_GateProbe`、
`/Game/DreamFXProbe/NS_GateSysProbe`，验证后已删除）和各自的镜像重建；三个被「瞄准」的手工资产
（`NS_Effects1_Mesh`、`破空灰尘`、`/AtlasFX/Templates/NS_Atlas2D_Mesh`）hash 前后不变
（拦截发生在 save 之前），字节留档在 `Saved/DreamFX/gate-probe/asset-backup/`。

### 6.9 已实现：`pull`（资产 → 文本）

> 分支 `feat/pull`，命令 `dfx pull`，诊断 `DFX7105`–`DFX7111`。**完整语义、边界与实测在
> [`pull.md`](pull.md)**；本节只记它和本文前几节的关系，以及实测纠正/确认了哪些结论。

- **§6.4 的两次「建议改成」按原样落地了一半，另一半换了个做法。** 建议是把 `-NoDefaults` 从
  「诊断用」升格成「写回路径的读取模式」，并把「普通模式与 `-NoDefaults` 的差集」定义为抑制集。
  实现里：`-NoDefaults` 仍然是**把键写进文本**的那一半（`pull` 依赖它 —— 文本里没有的输入，
  `pull` 不会替你加），而**值**不再走 pin：`pull` 直接反射读 rapid-iteration store，
  口径与 `asset-diff`、安全闸完全一致。于是「抑制集」这件事不再需要单独定义：
  文本没声明的，`pull` 一律不碰（`DFX7106`）。
- **§3.5 的 `MeshYaw` 现在可以在文本里修好了**：`pull -Apply` 把 `-90` 写进导出文本
  （`0.0 -> -90.0`，整文件只有这一行变化），随后的 `build` 1 built / 0 error，
  日志里没有任何 `build safety` 行 —— 值变了但文本声明了它，安全闸规则 3 放行；
  `asset-diff` 里这一项两侧都成了 `0000B4C2`。**§3.5 末尾「镜像是否真的画得不一样」的 L3 问题不受影响，
  仍然未验证**；本命令解决的是「文本带不带这个值」，不是「运行时读哪个」。
- **§6.8 第 7 行的 8 条，现在会被 `pull` 逐条报出来并跳过**，这就是「不新增结构」的直接证据
  （`破空灰尘`：19 compared / 0 to write / 94 not declared / 0 stack(s) not addressed，
  文件 SHA256 前后相同）。**读数订正**：94 是「按名字去重」的数，和 §3.4/§6.8 的 37 / 57 不是同一口径 ——
  后者是**逐脚本的事实条数**（同一个名字在 `ParticleSpawnScriptInterpolated` 与 `ParticleUpdateScript`
  里各算一条），前者是一个名字一行。用哪个数字都行，但别把它们当成同一个量。
- **一条新的适配器契约**（`pull` 踩到的）：`FNiagaraAdapter::GetEmitterInfo` 的 out 参数是**追加**语义，
  同一个 `FEmitterInfo` 实例给两个 emitter 用，第二个 emitter 会拿到第一个的 stack。
  `pull` 每个 emitter 新建一个实例并写了注释；**适配器本身没动**（共享路径，改它要单独评估）。

### 6.10 已实现：写回的覆盖面、脏集合与结构编辑（第二轮）

> 分支 `feat/write-back`（3 个提交，从 `feat/pull` 起），诊断新增 `DFX7112`–`DFX7115`。
> 语义、边界与逐条实测在 [`pull.md`](pull.md) 的 §4b/§4c/§4d/§5b；本节只记它和本文前几节的关系，
> 以及这一轮实测**确认 / 推翻**了哪些结论。

- **§3 的缺口清单，这一轮消化掉三项、一项明确留下。**
  - §3.4「模块输入的字面量不进文本」：**读侧的键**仍然只能靠 `decompile -NoDefaults` 提供（`pull`
    不新增结构），但**值**这一侧现在是正式路径，而且比 §6.4 建议的更直接 —— 值直接反射读
    rapid-iteration store，键由文本声明。§6.4 那句「把抑制集定义成普通模式与 `-NoDefaults` 的差集」
    因此仍然不需要实现。
  - §3.5 `MeshYaw`：§6.9 已解决（文本能修），本轮不变。
  - §3.6 / §3.7 / §3.8（渲染器 `MaterialParamValidMask`、DI 节点名漂移、`EffectType` 读写不对称）：
    **本轮没碰**，它们不是写回能覆盖的形态（分别是编译期缓存、节点名漂移、读侧缺一项）。
- **§4 那张「可写回清单」里有两类被低估了。** 表格把「渲染器属性」「设置块」和「赋值」列在
  「文本级通过 / 未验证」那一档，本轮把它们做成了**有断言的写路径**：`Settings` 走生成器自己那张
  映射表（读写两侧同一张），渲染器属性的**值拼写**由反编译器与 `pull` 共用一份实现
  （`WriteBack/DreamFXSourceValue.h`）。共用这份实现当场抓到一处回归：委托改造漏删了原 `switch`
  每个 case 的 `break`，渲染器只写出第一个属性，5 个 RoundTrip fixture 变红 —— **这正是 §3.9
  「L1 是导出 vs 导出」那条盲区的反面**：这次是资产级断言（`DescribeSystemFacts`）把它捞出来的。
- **§6.6 提出的第二条验收标准（`asset-diff` fact 集合相等）本轮在语料里落地了，但换了个更准的口径。**
  不是「两个资产 fact 全等」，而是「**同一个**资产原地重建之后，没有任何 fact 的地址还在却消失了」——
  那正是安全闸 `DFX8017` 拦的东西，而且它比闸更严（闸按规则 1 忽略只在一侧出现的结构）。
  §6.8 记的「原地重建只丢 8 条」在这里得到解释：那些「丢」是值等于模块默认值的常量，重建时被默认值
  补回、fact 相同。
- **一处只有实测才会发现的口径差（新）。** 表达式 `SetInput`（编辑器的滑条走的是同一条外部编辑 API）
  会把 EmitterUpdate 模块的常量写进**两份** store（`system-spawn` 与 `system-update`），而 DreamFX
  的一次重建只materialise `system-update` 那份。于是重建之后 `ri system-spawn ...` 那条 fact 消失了，
  值本身还在。安全闸按「一个结构里 fact 消失即 drift」会把它算成 drift **并拒绝**（措辞是
  「one copy fewer」）；语料里的闭环断言把它单独归类为「重建把两份相同常量合成一份」，只报不判失败。
  两个口径的差别是**已知且仅此一处**，写在这里而不是抹平：它影响的是「编辑器调过的值 + 立刻重建」
  这条路是否需要 `-Force`，值得单独查（本轮没查）。
- **端到端实测（探针 `/Game/DreamFXProbe/NS_PullProbe`，验证后已删）**：见 pull.md §5b 的七行原始结论。
  关键三条：结构编辑后 `build` 是 `1 built / 0 error / 0 warning` 且 **`Saved/DreamFX/BuildSafety/`
  根本没被创建**（闸只在报事时才写那个目录）；无操作 `pull -Apply` 的 **SHA256 与 mtime 都不变**；
  文本改值、资产没动时 `DFX7115` 让**文本赢**且文件不被触碰。

---

## 7. 复现步骤

### 环境

- 工程 `C:\CrossingVoid`，引擎 `D:\UnrealEngine-5.8.2`（已打 DreamFX 引擎补丁）
- 插件 `C:\CrossingVoid\Plugins\DreamFX` @ `359b3b3`（`pr/engine-patch-and-writeback`）
- **跑之前先确认没有 UnrealEditor 在跑**（本次实测时本来就没有）

### 命令（按执行顺序）

```powershell
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# 1) 覆盖检查：59 系统 / 59 导出 / 0 失败；17 处 gap
pwsh -File $dfx coverage -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 2) 全文导出：59 written / 0 failed → C:\CrossingVoid\DFX\Decompiled\**
pwsh -File $dfx decompile-all -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 3) 固定点抽样：每个源一次 build（只写 Decompiled/ 镜像，不动原资产）
#    见 Saved\dfx-writeback-study\run-builds.ps1
pwsh -File C:\CrossingVoid\Saved\dfx-writeback-study\run-builds.ps1
#    S1 NS_Atlas2D_Mesh OK | S2 NS_Effects1_Mesh OK | S3 TR刀光_System OK
#    S4 NS_ScanScene 失败 21 error | S5 破空灰尘 OK

# 4) L1/L2 固定点检查
pwsh -File $dfx mirror-diff -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -Engine $eng

# 5) 反射级独立比对 + fact 落盘
pwsh -File $dfx asset-diff -Path '/AtlasFX+/Game/GameActor2D+/Game/AssetMaterial' -DumpFacts -Engine $eng

# 6) 把被抑制的输入打出来（§3.5 的关键一步）
pwsh -File $dfx decompile /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -NoDefaults -Out <out>.dfs -Engine $eng
```

### 产物位置

| 内容 | 路径 |
|---|---|
| 导出文本（工程侧，`.gitignore` 已忽略 `DFX/Decompiled/`） | `C:\CrossingVoid\DFX\Decompiled\**`（59 个 `.dfs`） |
| 导出文本副本 + 全部原始输出 | `C:\CrossingVoid\Saved\dfx-writeback-study\`（`decompiled\`、`raw-*.txt`、`build-S*.txt`） |
| fact 落盘（原 vs 镜像，各 5 对） | `C:\CrossingVoid\Saved\DreamFX\*.{original,mirror}.facts` |
| 镜像资产（`.gitignore` 已忽略） | `C:\CrossingVoid\Content\Decompiled\**`、`C:\CrossingVoid\Plugins\AtlasFX\Content\Decompiled\**` |
| `-NoDefaults` 对照导出 | `Saved\dfx-writeback-study\NS_Effects1_Mesh.NoDefaults.dfs`、`NS_Atlas2D_Mesh.NoDefaults.dfs` |

### 未触碰的东西（自证）

- **原资产零修改**。`build` 只写 `Decompiled/` 命名空间下的镜像。
  唯一一次非文本写入来自 `decompile-all` 的 R3 嵌入脚本抽取
  （`/Game/Decompiled/…/Scripts/NS_ScanScene_*.uasset`，也在 `Decompiled/` 下）。
- 工程仓库在本次会话开始前就带着几处 `M`（`Plugins/AtlasFX/Content/Templates/NS_Atlas2D_*.uasset`
  mtime 17:47、`Content/GameActor2D/Misaka/Material/DefAtk/FX_Defatk.uasset` mtime 18:07），
  全部早于本次实测（20:20 起），**不是本次产生的**。
- 插件仓库在写报告前 `git status` 干净。
- 没有改动 `D:\UnrealEngine-5.8.2`。
