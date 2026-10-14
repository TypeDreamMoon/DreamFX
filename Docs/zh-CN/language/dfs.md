---
title: .dfs — 一个系统
description: 系统 Settings、user 参数、六个栈里的模块调用与赋值、Defaults、renderer 与绑定，以及引用 .dfe。
---

一个 `.dfs` 产生一个 `UNiagaraSystem`。

```cpp
System(Name="Effects/NS_Spark", Root="Game")
{
    Settings   = { ... }        // 系统属性
    Properties = { ... }        // user 参数（User.*）

    SystemSpawn  = { ... }      // 系统作用域的两个栈（L1）
    SystemUpdate = { ... }

    Emitter <Name> { ... }               // 内联
    Emitter <Name> from "<path>" { ... } // 从 .dfe 拷贝，再覆盖
}
```

<a id="settings"></a>

## `Settings`

schema 驱动：名字是从活资产上读出来的，所以拼错会报出真实的可用列表
（[DFX3020](../diagnostics/DFX3xxx.md#dfx3020)）。常用的几个：

```cpp
Settings = {
    EffectType  = "/Niagara/Default/FX_Default.FX_Default";
    WarmupTime  = 0.0;
    FixedBounds = box(-200, -200, -50, 200, 200, 300);
    FixedTickDelta     = true;      // 以固定步长子步进，而不是每帧一次
    FixedTickDeltaTime = 0.01667;   // …60Hz。这改的是模拟本身，不只是顺滑度
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

`ModulePaths` 是 DreamFX 自己的，不是 Niagara 的：它给模块短名解析**追加**搜索根（L4）。
引擎默认的三条一直在，所以声明自己的目录是加，不是换。

<a id="properties--user-参数"></a>

## `Properties` —— user 参数

```cpp
Properties = {
    int              SparkCount = 24                     [ Group="Burst"; SortPriority=10 ];
    float            SparkSpeed = 450.0;
    Color            TintA      = (1.0, 0.72, 0.25, 1.0);
    Vector           HitNormal  = (0, 0, 1)              [ Description="蓝图传进来的撞击法线" ];
    Texture2D        NoiseTex   = "Plugin.MoonToon:Textures/T_Noise01";
    DI<SkeletalMesh> TargetMesh;
}
```

每一条变成 `User.<Name>`，蓝图用 `SetNiagaraVariable*` 设值。这个名字是**跨重建的稳定键**，
改名会打断每一个引用它的蓝图。

`Description` 会到达资产。`Group` 和 `SortPriority` 不会 —— 外部编辑 API 的 user 变量结构体
没有放它们的字段，构建会用 [DFX5099](../diagnostics/history/DFX5099.md#dfx5099) 说一次。它们留在源码里
充当文档。

<a id="数据接口参数带着配置"></a>

### 数据接口参数带着配置

```cpp
Properties = {
    DI<RigidMeshCollisionQuery> Collide_StaticMesh = "{\"ActorTags\":[\"collider\"], … }";
    DI<Curve>                   SizeCurve;          // 裸声明：一个留给运行时填的槽
}
```

配置是导出器写出来的那串引号包裹的 JSON，逐字承载。

**提示：**

到 2026-08-12 之前，DI 参数是**只能声明**的（DFX5098 曾经的意思是「运行时喂给它」）。
  那是一次有意的范围削减，而它老得很难看：一个碰撞源、一个属性读取器**本身就是**它的配置，
  于是重建出来的镜像 default-construct 了特效用来查询世界的那些对象 —— 烟还是那股烟，
  撞到的东西不是那个东西了。现在 [DFX5098](../diagnostics/DFX5xxx.md#dfx5098) 的意思是
  **值的形状不对**，不再是「会被忽略」。

裸声明仍然是「留给运行时填的槽」，这一点没变。曲线接口的可读写法是 `curve { }`，见
[值与规则](values.md#curve)。

<a id="栈"></a>

## 栈

六个栈，两个在系统作用域、四个在每个 emitter 里（L1），另外还有每个 emitter 一个的事件栈
（`OnEvent`）和任意多个 simulation stage 栈（`Stage`），后两者见
[事件与 Stage](events-and-stages.md)。

```
SystemSpawn      SystemUpdate           <- .dfs 顶层
EmitterSpawn     EmitterUpdate          <- Emitter 块里
ParticleSpawn    ParticleUpdate
```

**写的顺序就是模块顺序。** 语句只有两种形态（L2）：

```cpp
ParticleUpdate = {
    GravityForce(Gravity = (0, 0, -680));      // 模块调用
    Particles.Moon.Seed = 0.5;                 // 赋值
    Particles.Moon.Tint = User.TintA;          // …折进同一个 Set Parameters 模块
    SolveForcesAndVelocity();                  // …而这次调用结束这一串
}
```

**连续的赋值折叠成一个 Set Parameters 模块**，模块调用打断这一串。这条规则正是往返能对称的原因：
一个 Set Parameters 模块导出成一段赋值。

<a id="没声明的栈会被留着"></a>

### 没声明的栈会被留着

没写的栈**原样留着**，构建会说它留了什么（[DFX5003](../diagnostics/DFX5xxx.md#dfx5003)）。
这对 `SystemUpdate` 尤其重要 —— 新系统会自带一个 `SystemState`，如果把每个没声明的栈都清空，
那每个不写 `SystemUpdate` 的 `.dfs` 都会产出一个永远不运行的系统。

要接管一个栈并清空它，就把它声明成空的：

```cpp
SystemUpdate = { }
```

emitter 的四个栈不受这条影响：DreamFX 建 emitter 时不带任何默认模块，它们本来就是空的。

<a id="模块调用"></a>

## 模块调用

```cpp
ModuleName(Input = Value, Input = Value);
Spawn/Initialization/V2/InitializeParticle(...);   // 短名有歧义时写偏路径
ModuleName@1.2(...);                               // R7 版本固定
disabled GravityForce(Gravity = (0, 0, -980));     // 在栈里，但不执行
Grid3D_ResampleFloat() as Grid3D_ResampleFloat003; // 固定节点名（见下）
```

实参**永远具名**（[DFX2008](../diagnostics/DFX2xxx.md#dfx2008)）。输入名会归一化 ——
Niagara 的 `Loop Duration` 写成 `LoopDuration`。

<a id="disabled"></a>

### `disabled`

`disabled` 把模块停在那里而不删掉：它留在栈里、保留输入、不运行。这是 Niagara 自己的
「留着但关掉」状态，而**保留输入**正是它比注释掉一行更有用的全部理由。

它只能前缀模块调用。放在赋值前是 [DFX2024](../diagnostics/DFX2xxx.md#dfx2024) ——
赋值折进那个栈共享的 Set Parameters 模块里，禁用它会把旁边所有赋值一起干掉。

<a id="as--固定节点名"></a>

### `as` —— 固定节点名

```cpp
Grid3D_ResampleFloat() as Grid3D_ResampleFloat003;
```

`as` 固定的是函数调用**节点**的名字。一旦有东西链接 `Output.<node>.<value>`，这件事就至关重要：
这些链接按节点显示名解析，而引擎给新节点编号是按添加顺序来的。一个背着多年编辑史的原件
（一个 `003`，它的兄弟早被删光了）重建出来会拿到不同的编号，于是每一条 output 链接都悬空成
「读在写之前」。

反编译器在节点名不等于模块资产名时就会写 `as`；手写的源码很少需要它。名字在一个 emitter 内唯一
（[DFX5034](../diagnostics/DFX5xxx.md#dfx5034)）。

<a id="静态开关的书写顺序"></a>

### 静态开关的书写顺序

**静态开关会揭示别的输入，而在模块上，源码顺序就是写入顺序。** 一个要等开关取值后才存在的输入，
必须写在开关后面：

```cpp
EmitterState(
    LifeCycleMode = Self,        // 它决定下面还有什么
    LoopBehavior  = Once,
    LoopDuration  = 0.15
);
```

在 **dynamic input** 上这条规则被放宽：开关会被提前，无论写在哪里都先写入。

```cpp
VelocityStrength = RandomRangeFloat(
    Minimum        = User.Speed * 0.5,
    Maximum        = User.Speed,
    RandomnessMode = SimulationDefaults   // 一个开关，实际写在上面两条之前
)
```

`dfx schema <Module> -Stack <Stack>` 打印构建看到的签名，静态开关包含在内。

<a id="版本"></a>

### `@版本`

`@1.2` 记录源码是照哪个版本写的，在模块节点上**选中**那个版本，资产不再提供它就报错
（[DFX3009](../diagnostics/DFX3xxx.md#dfx3009)）。选中是图层面的操作，所以任何引擎上都能用。

<a id="赋值"></a>

## 赋值

```cpp
Particles.Moon.Seed        = 0.5;          // 第一次写入声明这个属性（L2）
Color Particles.Color      = hlsl { ... }; // 值本身没有类型时，写上类型
Emitter.MyCounter          = 0;
```

目标必须带命名空间（[DFX4025](../diagnostics/DFX4xxx.md#dfx4025)）。类型来自值；
`hlsl` 块、dynamic input 和内联表达式都不带类型，所以那三种需要显式标注
（[DFX4022](../diagnostics/DFX4xxx.md#dfx4022)）。

<a id="defaults--没人写过时读到什么"></a>

## `Defaults` —— 没人写过时读到什么

```cpp
Emitter Sparks
{
    Defaults = {
        float Particles.MySize = 1.0;                     // 一个值
        Vector Particles.Home  = Engine.Owner.Position;   // 一个绑定
    }

    ParticleSpawn = { … }
}
```

赋值说的是**这个参数现在是这个**；默认说的是**如果没人设过它，读它得到这个** —— 就是 Niagara 的
`DefaultMode`，也是「一个读得以编译」和「读不过」的区别。

条目是带声明类型的赋值（[DFX4028](../diagnostics/DFX4xxx.md#dfx4028)），值必须是字面量、枚举或
另一个参数，因为默认值不能逐粒子计算（[DFX4029](../diagnostics/DFX4xxx.md#dfx4029)）。

**提示：**

这个块曾经被当成 API 缺口。真相是一个顺序 bug：写入动作**创建了**它随后拒绝填充的那条条目。
  修法是把默认值放到栈之后、外加一趟隐含的 Value pass。反编译器在 emitter 图上带着一份与全新构建
  不同的默认值时导出 `Defaults` 块。

<a id="renderers"></a>

## Renderer

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

属性是 schema 驱动的（L8）：每种 renderer 类型都拿到它的**全部**属性集，没有逐类型语法，
名字不认识就报出真实列表。只有类型关键字本身是闭集
（[DFX3004](../diagnostics/DFX3xxx.md#dfx3004)）。

<a id="装资产列表的属性"></a>

### 装资产列表的属性

`Meshes`、`OverrideMaterials` 这类持有资产**列表**的属性写成路径数组：

```cpp
MeshRenderer Body
{
    Meshes            = ["/Engine/BasicShapes/Cube"];
    OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
}
```

每个元素其实是个结构体，资产只是其中一个字段；哪个字段是靠反射找出来的，所以这对还不存在的
renderer 类型同样成立。结构体的**其他**字段 —— 网格的逐元素轴心、缩放、LOD 范围 —— 没有语法。
会因此丢东西的导出把它写进文件头，而不是抹平了事。

<a id="bind-为什么和属性赋值分开"></a>

### `Bind` 为什么和属性赋值分开

属性绑定不是普通字段：绑定结构体缓存了显示名、数据集名和 source-mode 标志，而这些只有它自己的
`SetValue` 会重算。直接写序列化字段会留下半个绑定。

<a id="声明顺序就是-renderer-顺序"></a>

### 声明顺序就是 renderer 顺序

没有别的寻址方式。**调换两个 renderer 块的位置会重画这个特效。**

`Material` 不写会套用引擎默认值（[DFX5004](../diagnostics/DFX5xxx.md#dfx5004)），而不是什么都不画。

<a id="引用一个-dfe"></a>

## 引用一个 `.dfe`

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

合并做什么、不做什么，见 [`.dfe`](dfe.md)。
