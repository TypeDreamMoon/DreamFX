---
title: "`.dfs` — 一种系统"
description: "`.dfs` — 一种系统"
---

<a id="dfs--a-system"></a>

# `.dfs` — 一种系统

产生一个 `UNiagaraSystem`。

添加 `Parent="../Base.dfs"` 以继承另一个系统的源。关于合并、路径与重建规则，见
[系统源继承](system-inheritance.md)。

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

<a id="settings"></a>

## `Settings`

schema 驱动：名字是从活资产上读出来的，所以拼错会报出真实的可用列表
（DFX3020）。常用的几个：

```cpp
Settings = {
    EffectType  = "/Niagara/Default/FX_Default.FX_Default";
    WarmupTime  = 0.0;
    FixedBounds = box(-200, -200, -50, 200, 200, 300);   // implies UseFixedBounds = true
    UseFixedBounds = true;          // the system's "Fixed Bounds" override flag (bFixedBounds)
    FixedTickDelta     = true;      // substep at a fixed rate instead of once per frame
    FixedTickDeltaTime = 0.01667;   // ... of 60Hz. Changes simulation, not just smoothness
    ModulePaths = ["/Niagara/Modules", "/Game/FX/Modules"];
}
```

`ModulePaths` 是 DreamFX 自己的，不是 Niagara 的：它给解析模块短名追加搜索根
（L4）。引擎默认项（`/Niagara/Modules`、`/Niagara/DynamicInputs`、`/Niagara/Functions`）会留在
列表上，所以声明自己的目录是追加，而不是替换。

重建会把省略掉的受支持系统与发射器设置重置为全新资产的引擎默认值，
然后应用源中声明的设置。因此移除一个受支持的设置会清除它之前的覆盖；
这一重置覆盖的是生成器映射的设置，而不是无关的资产字段。

声明一个 `FixedBounds` 盒会打开系统的覆盖标志，因为动态边界下的盒是不起作用的。
写 `UseFixedBounds = false;` 可以在边界保持动态的同时保留已编写的盒；反编译器对一个持有盒但标志关闭的
资产正是这样写的，因此往返能保留两者。

<a id="properties--user-parameters"></a>

## `Properties` —— user 参数

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

每一条变成 `User.<Name>`，可在蓝图中用 `SetNiagaraVariable*` 设值。这个名字是跨重建的稳定
键（plan 4.5），所以重命名会打断每一个引用它的蓝图。

参数还可以用 `Group("Name") { ... }` 作用域来组织，与 DreamShader 一致：

```cpp
Properties = {
    Group("Burst") {
        int   SparkCount = 24;
        float SparkSpeed = 450.0;
    }
    Group("Look") {
        Color TintA = (1.0, 0.72, 0.25, 1.0);
    }
    float Loose = 1.0;
}
```

作用域内的每个参数都继承组名，而 `SortPriority` 是按声明顺序盖上的 —— 第 10 步，一个计数器
贯穿整个块，所以组和嵌套按一条连续的序列给成员编号。嵌套与 `|`（Unreal 的子类别
语法）组合：`Group("Outer") { Group("Inner") { … } }` 把它的成员命名为 `Outer|Inner`。参数自身显式的
`[ Group=… ]` 或 `[ SortPriority=… ]` 优先，且不占用自动编号槽位；松散顶层参数两者都不带。
关键字不区分大小写。

有三个诊断守护这一形式：DFX2027（名字必须是带引号的字符串）、DFX2028（它必须
非空），以及 DFX2029（参数块内不得有其他形式打开花括号）。

`Description` 会到达资产，`Group` 和 `SortPriority` 也会：适配器会根据
计划重建系统的 user 参数层级（`UNiagaraSystemEditorData::UserParameterHierarchy`），因此细节面板按源中布局的方式对参数分组和排序。
UE 5.8 的逐变量 `CategoryName` / `EditorSortPriority` 元数据在引擎侧已废弃 ——
层级才是参数面板实际读取的内容。计划是关于组织的全部真相：在编辑器内手工做出的排列不会在重建后保留。

数据接口参数带着它们的配置，即导出器写出的那串引号包裹的 JSON：

```cpp
Properties = {
    DI<RigidMeshCollisionQuery> Collide_StaticMesh = "{\"ActorTags\":[\"collider\"], … }";
    DI<Curve>                   SizeCurve;          // declared bare: a slot to fill at runtime
}
```

直到 2026-08-12，这只是可声明的（plan 3.5，DFX5098「运行时喂给它」）。那是一次
有意的范围削减，而它老得很难看：一个碰撞源或一个属性读取器**本身就是**它的配置，
所以重建出来的镜像 default-construct 了特效用来查询世界的那些对象 —— 烟还是那股烟，
撞到的东西不是那个东西了。DFX5098 现在的意思是值的**形状**不对，而不是它会被忽略。

没有值的声明仍然是一个留给运行时填的槽，而且一直如此。`curve { }` 仍然是曲线接口的
可读写法 —— 见 [values.md](values.md)。

<a id="stacks"></a>

## 栈

六个栈，两个在系统作用域、四个在每个发射器里（L1），另外还有每个发射器一个的事件栈（用
`OnEvent` 声明）和任意多个 simulation stage 栈（用 `Stage` 声明）（两者见下）：

```
SystemSpawn      SystemUpdate           <- top level of the .dfs
EmitterSpawn     EmitterUpdate          <- inside an Emitter block
ParticleSpawn    ParticleUpdate
```

写的顺序就是模块顺序。有两种语句形态（L2）：

```cpp
ParticleUpdate = {
    GravityForce(Gravity = (0, 0, -680));      // module call
    Particles.Moon.Seed = 0.5;                 // assignment
    Particles.Moon.Tint = User.TintA;          // ...folds into the same Set Parameters module
    SolveForcesAndVelocity();                  // ...and this call ends the run
}
```

**连续的赋值折叠成一个 Set Parameters 模块**，而模块调用打断这一串。
这条规则正是往返能够对称的原因：一个 Set Parameters 模块导出成一段赋值。

没有声明的栈会被原样留着，构建会说明它留了什么（DFX5003）。这对
`SystemUpdate` 尤其重要 —— 新系统会自带一个 `SystemState`；如果把每个没声明的栈都清空，
那每个不写显式 `SystemUpdate` 的 `.dfs` 都会产出一个永远不运行的系统。要接管一个栈
并清空它，就把它声明成空的：`SystemUpdate = { }`。

发射器的四个栈不受这条影响：DreamFX 建发射器时不带任何默认模块，所以它们本来就是空的。

<a id="module-calls"></a>

### 模块调用

```cpp
ModuleName(Input = Value, Input = Value);
Spawn/Initialization/V2/InitializeParticle(...);   // partial path, when the short name is ambiguous
ModuleName@1.2(...);                               // R7 version pin
disabled GravityForce(Gravity = (0, 0, -980));     // in the stack, not executed
Grid3D_ResampleFloat() as Grid3D_ResampleFloat003; // pin the node's name (see below)
```

`as <name>` 固定的是函数调用**节点**的名字。一旦有东西链接
`Output.<node>.<value>`，这件事就至关重要 —— 那些链接按节点显示名解析，引擎给新节点编号是按添加顺序来的，
而一个编号背着多年编辑史的原件（一个 `003`，它的兄弟早被删光了）重建出来会拿到不同的
编号，于是每一条 output 链接都悬空成「读在写之前」。反编译器在节点名不等于模块资产名时会写
`as`；手写的源很少需要它。名字在每个发射器内唯一（DFX5034）。

实参永远具名（DFX2008）。输入名会归一化 —— Niagara 的 `Loop Duration` 写成
`LoopDuration`。

`disabled` 把模块停在那里而不删掉：它留在栈里、保留输入、不运行。这是 Niagara 自己的
「留着但关掉」状态。它也可以前缀一个赋值：

```cpp
Color Particles.Color = (1, 1, 1, 1);
disabled Color Particles.Color = (1, 0, 0, 1);
float Particles.CustomValue = 2.0;
```

连续赋值只有在启用状态一致时才会折叠。这个例子创建三个 Set Parameters 节点；中间节点保持禁用，
两侧节点保持启用。反编译器会保留该状态。`Defaults` 中的声明不能被禁用（DFX2024），
因为默认值是参数元数据，而不是一个被执行的栈节点。

**静态开关会闸控其他输入，而在模块上，源顺序就是写入顺序。** 一个要等开关取值后才存在的输入，
必须写在它后面：

```cpp
EmitterState(
    LifeCycleMode = Self,        // gates everything below
    LoopBehavior  = Once,
    LoopDuration  = 0.15
);
```

在 **dynamic input** 上这条规则被放宽：开关会被提前，无论以什么顺序出现都先写入，所以两种情况
都能用。

```cpp
VelocityStrength = RandomRangeFloat(
    Minimum        = User.Speed * 0.5,
    Maximum        = User.Speed,
    RandomnessMode = SimulationDefaults   // a switch, written before the two above
)
```

`dfx schema <Module> -Stack <Stack>` 打印构建看到的签名，静态开关包含在内。

`@1.2` 记录源是照哪个版本写的，在模块节点上**选中**它，资产不再暴露它就报错
（DFX3009）。选中是图层面的操作，所以任何引擎上都能用。

<a id="assignments"></a>

### 赋值

```cpp
Particles.Moon.Seed        = 0.5;          // first write declares the attribute (L2)
Color Particles.Color      = hlsl { ... }; // a type, when the value carries none
Emitter.MyCounter          = 0;
```

目标带命名空间限定（DFX4025）。类型来自值；`hlsl` 块、dynamic
input 和内联表达式都不带类型，所以那几种需要显式标注（DFX4022）。

<a id="onevent--reacting-to-another-emitters-events"></a>

### `OnEvent` —— 响应另一个发射器的事件

```cpp
Emitter Receiver
{
    ParticleSpawn = { … }

    OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = {
        ReceiveLocationEvent();        // ordinary stack statements; runs per received event
    }
}
```

header 配置 handler，block 和其他任何栈一样。`Source` 指定这个系统中的一个 emitter，`Event` 是它生成的事件（一个 `GenerateLocationEvent()` 模块发送 `"LocationEvent"`），其余是 handler 的开销控制，只在它们与默认值不同时才写出：`Mode`（`SpawnedParticles` 每个事件生成 `SpawnNumber` 个新粒子并在它们上运行 block；`EveryParticle` 在已有粒子上运行它）、`MaxEventsPerFrame`、`UpdateAttributeInitialValues`、`RandomSpawnNumber` 与 `MinSpawnNumber`。`Source` 和 `Event` 是必需的（DFX2025）。

每个 emitter 一个 `OnEvent` block（DFX5031）：DreamFX 通过与其他四个相同的轨道访问事件栈，而这些轨道只能到达一个 handler。一个应当响应两条事件流的 emitter 要变成两个 emitter。

`Source` 通常是一个 emitter 名。反编译器也会在那里写出两个带方括号的 token，文本中可能带有它们：`` `<none>` ``，这个无效 id 表示 handler 从它所处的 emitter 读取；以及 `` `<unresolved GUID>` ``，一个系统中没有任何 emitter 响应的 id —— 这就是一个 handler 被带进复制系统、且途中没有任何东西重映射其来源时的样子，于是多个系统共享同一个陈旧 id。两者都原样写回，所以这样的文本能够重建并保持对资产的忠实；第二个是警告（DFX5036），因为来源已不存在的 handler 永远不会触发。反引号是词法器对非裸名称的规则，不是引号选择。

事件*生成器*通常需要在源 emitter 的 `Settings` 中指定 `RequiresPersistentIDs = true`，因为它读取 `Particles.ID`；没有它，构建会报告引擎自身的栈问题（DFX6003）。

<a id="stage--simulation-stages"></a>

### `Stage` — 模拟阶段

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

stage 是在 GPU emitter 上于 `ParticleUpdate` 之后运行的粒子栈（`SimTarget = GPU`；带有 `Stage` block 的 CPU emitter 是 DFX5033）。声明顺序就是运行顺序，每个 block 都和其他任何栈一样 —— 模块调用、折叠赋值、`disabled`，全都可以。

header 持有 stage 自己的属性，只在它们与刚添加的 stage 不同时才写出：`DataInterface` 指定 stage 迭代的网格（每个单元格一个线程，而不是每个粒子），并且已经隐含了迭代来源；`Iteration` 为需要它的形状显式说明（`Particles`、`DataInterface`、`DirectSet`），例如未绑定任何内容的数据接口迭代；`NumIterations` 重复该 stage（默认 1）；`Enabled = false` 将其暂停。未知或形状错误的参数是 DFX2026。两个同名 `Stage` block 是 DFX5032 —— stage 按名称标识，否则构建会悄悄保留最后运行的那个栈。

`DataInterface` 是一个名称，而这个名称就是全部契约：引擎在编译期按名称解析迭代网格，从不读取绑定所存储的类型（已创作的资产在之后会话中会带有解析为垃圾的类型句柄，但仍能正常模拟）。网格本身通常由模块的内部写入物化 —— 它不需要是声明的参数，绑定就能工作；拼错的名称会表现为 stage 针对一个不存在的网格进行编译。

`NumIterations` 和 `Enabled` 各自接受**值或参数**。值的位置决定是哪种：数字、`true` 或 `false` 是字面量，任何读作名称的都是绑定。

```cpp
Stage Project(NumIterations = 6, NumIterations = Emitter.OVERRIDE.SolveIterations) = { … }
Stage DebugSlice(Enabled = Grid3D_GAS_CONTROLS_SPAWN.RenderDebugSlice) = { … }
```

两者可以如上同时出现，因为引擎会同时存储两者：数字是绑定所覆盖的回退值。这一点值得写下来，而不是简化掉 —— 一个 `Enabled` 被绑定的 stage 会在特效说运行时运行，而一个只保留字面标志的重建会*始终*运行它，这正是 Ninja 的调试切片在每面镜子中画到流体上方的原因。

自定义 C++ stage 类的 stage（任何不是引擎通用 stage 的东西）仍是一个空白，带有自己的 header 行（DFX8016）。

通用 stage 还保留其 dispatch 和粒子过滤配置：

| 参数 | 值 |
| --- | --- |
| `DirectDispatchType` | `OneD`、`TwoD`、`ThreeD` 或 `Custom` |
| `DirectDispatchElementType` | `NumThreads`、`NumThreadsNoClipping` 或 `NumGroups` |
| `ElementCountX`、`ElementCountY`、`ElementCountZ` | 非负整数回退值和/或参数名 |
| `OverrideGpuDispatchNumThreads` | `true` 或 `false` |
| `OverrideGpuDispatchNumThreadsX`、`OverrideGpuDispatchNumThreadsY`、`OverrideGpuDispatchNumThreadsZ` | 非负整数回退值和/或参数名 |
| `GpuDispatchForceLinear`、`DisablePartialParticleUpdate` | `true` 或 `false` |
| `ParticleIterationStateEnabled` | `true` 或 `false` |
| `ParticleIterationStateBinding` | 粒子属性名，或 `None` 以清除 |
| `ParticleIterationStateRange` | 包含两端的整数对，例如 `(1, 3)` |

```cpp
Stage Dispatch(
    Iteration = DirectSet,
    DirectDispatchType = TwoD,
    ElementCountX = 128, ElementCountX = Emitter.Width,
    ElementCountY = 64,
    OverrideGpuDispatchNumThreads = true,
    OverrideGpuDispatchNumThreadsX = 8,
    OverrideGpuDispatchNumThreadsY = 8
) = { ... }
```

与 `NumIterations` 一样，用数字和参数重复同一个计数会保留引擎绑定的两半。导出会写出与刚添加的 stage 不同的值，并保留绑定。移除某个选项会在下次构建时将其重置为刚添加 stage 的默认值；旧绑定不会残留。线程组覆盖参数保留 Niagara 的 `static int` 类型；元素计数使用普通的 `int`。`ParticleIterationStateBinding = None` 显式清除默认粒子属性绑定。

<a id="defaults--what-a-read-produces-when-nothing-wrote"></a>

## `Defaults` — 没人写过时读到什么

> 自 2026-08-09 起可用。看起来像 API 缺口的东西其实是一个顺序 bug：写入动作创建了它随后拒绝填充的那条条目，所以现在默认值在栈之后、外加一趟隐含的 Value pass 应用。反编译器在 emitter 图上带着一份与全新构建不同的 `Defaults` block 时导出它。

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

赋值说的是*这个参数现在是这个*。默认说的是*如果没人设过它，读它得到这个* —— 就是 Niagara 的 `DefaultMode`，也是「一个读得以编译」和「读不过」的区别。条目是带声明类型的赋值（DFX4028）；值必须是字面量、枚举或另一个参数，因为默认值不能逐粒子计算（DFX4029）。

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

属性是 schema 驱动的（L8）：每种 renderer 类型都拿到它的全部属性集，没有逐类型语法，名字不认识就报出真实列表。只有类型关键字本身是闭集（DFX3004）。

一个持有资产*列表*的属性 —— 例如网格 renderer 上的 `Meshes` 和 `OverrideMaterials` —— 写成路径数组：

```cpp
MeshRenderer Body
{
    Meshes           = ["/Engine/BasicShapes/Cube"];
    bOverrideMaterials = true;
    OverrideMaterials = ["Plugin.MoonToon:Materials/FX/M_Chunk"];
}
```

网格材质覆盖默认禁用。非空的 `OverrideMaterials` 数组需要 `bOverrideMaterials = true;`；否则 DreamFX 会报告 DFX7105，并且 renderer 会使用网格自身的材质。网格 renderer 没有单独的 `Material` 属性：写入它是错误（DFX3049）。

每个元素其实是个结构体，资产只是其中一个字段；哪个字段是靠反射找出来的，所以这对还不存在的 renderer 类型同样成立。结构体的*其他*字段 —— 网格的逐元素轴心、缩放和 LOD 范围 —— 没有语法。会因此丢东西的导出把它写进文件头，而不是抹平了事。

`Bind` 与属性赋值是分开的，因为属性绑定并非普通字段——绑定结构体缓存了一个显示名、一个数据集名和若干 source-mode 标志，只有它自己的 `SetValue` 会重新计算这些内容，因此直接写入序列化字段会留下半个绑定。

其他绑定结构，包括 Sprite 或 Ribbon renderer 的 `MaterialUserParamBinding`，在导出时以带引号的 JSON 属性值形式承载。用户参数绑定只携带其名称，例如 `"{\"parameter\":{\"name\":\"User.Material\"}}"`；固定的参数类型由 renderer 提供。这样避免了序列化 Niagara 的进程本地类型索引。类型不符合预期的绑定会报告一个导出缺口。这些绑定与属性 `Bind` 语句是分开的。

**声明顺序就是 renderer 顺序**，而且没有其他寻址方式。调换两个 renderer 块的位置会重画这个特效。

在 Sprite、Ribbon 或 Decal renderer 上省略 `Material` 会套用引擎默认值（DFX5004）。

<a id="referencing-a-dfe"></a>

## 引用一个 `.dfe`

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

合并做什么、不做什么，见 [dfe.md](dfe.md)。
