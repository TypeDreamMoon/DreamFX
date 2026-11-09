<a id="dynamicmesh--procedural-geometry-written-by-the-simulation"></a>

# `DynamicMesh` — 由模拟写入的程序化几何体

`DynamicMesh` 块在 `.dfs` 中声明一个网格，其顶点由 GPU 模拟每帧计算：它的拓扑（四边形网格，按 patch 组织）、它的材质和顶点布局，以及每个 patch 一个 HLSL 程序，说明每个顶点在哪里以及携带什么。构建会把该块转换为普通的 Niagara 组成部分——一个 `DI<DynamicMesh>` 用户参数、每个 patch 一个 GPU 发射器、两个生成的模块，以及一个绘制该网格的发射器——并顺带处理引擎的动态网格缺陷。

它的存在是为了让形状被*计算*出来，而不是烘焙出来：没有逐帧网格，没有顶点数据的翻页动画，也没有任何只在追踪它的那一台摄像机视角下才成立的东西。一片薄片会变成弯曲的外壳，一个扇形会变成锥体，一条条带会变成管状——而同一程序可从任意角度运行。

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

<a id="shape-of-the-block"></a>

## 块的结构

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

只有 `Patch` 是必需的。该块位于系统的顶层，与 `Emitter` 块并列；它的发射器放置在写入该块的位置，因此它像任何发射器一样参与发射器顺序。

`<Name>` 会变成三样东西，且三者都必须未被占用：用户参数 `User.<Name>`（网格）、发射器 `<Name>`（绘制它）以及每个 patch 一个发射器 `<Name>_<Patch>`（计算它）。

<a id="settings"></a>

### `Settings`

| 键 | 值 | 默认值 |
| --- | --- | --- |
| `Material` | 材质资产路径，与 `.dfs` 中其他地方一样 | 无——引擎的默认材质 |
| `TexCoords` | 每个顶点的 UV 通道数，`0`–`8`；主体写入 `UV0` … `UV<n-1>` | `1` |
| `Colors` | `true` / `false`：分配顶点颜色通道 | `true` |
| `Normals` | `None`、`Auto` 或 `Explicit`（见下文） | `None` |
| `Bounds` | 网格局部空间中的 `box(...)`——剔除边界；生成的发射器将其用作固定边界（`CalculateBoundsMode = Fixed`） | `box(-1000, -1000, -1000, 1000, 1000, 1000)` |
| `Duration` | 网格存在的秒数，之后消失 | 与系统运行时间一样长 |
| `Visible` | 任意浮点值（字面量、链接、`hlsl { }`、动态输入）；当该值 > 0.5 时绘制网格 | 始终绘制 |

`Normals`:

- `None`——不分配切线基。无光照的半透明效果很少需要切线基。
- `Auto`——构建会对 patch 主体进行微分：它在 U 和 V 方向上各偏离四分之一个网格步长求值程序（每个顶点四次求值，所有采样都在网格内部，以便极点和边缘结果正确），并写入法线 `cross(dP/dU, dP/dV)` 和切线 `dP/dU`。主体会运行四次，因此在 `Auto` 下它可以读取粒子属性，但不能写入它们（DFX3065）。
- `Explicit`——主体写入 `Normal`（并可选写入 `Tangent`）；拓扑的解析法线是起始值。切线在写入前会相对于法线正交化。

<a id="inputs"></a>

### `Inputs`

```cpp
Inputs = {
    DI<Texture> Shape = User.ShapeTex;                       // a data interface: Shape.LoadTexture2D(...)
    float       Frame = hlsl { floor(Particles.Age * 30.0) };
    float       Gain  = FloatFromCurve(FloatCurve = curve { 0 -> 1; 1 -> 0; }, CurveIndex = Particles.NormalizedAge);
}
```

每个条目都是一个带类型的名称及其值，值可以是模块输入接受的任何内容——字面量、链接、动态输入、内联表达式、`hlsl { }`。在 patch 主体内部，该名称是一个普通变量。输入会按顶点在计算发射器中求值（那里的 `Particles.*` 是该顶点的粒子）。

普通的 `User.`、`System.`、`Emitter.` 和 `Engine.` 参数也可以直接在主体中读取，就像在任何 `.dfm` 主体中一样；`Inputs` 是数据接口或计算值所需的东西。数据接口必须通过 `Inputs` 传入——主体只能对引脚调用函数。

<a id="patch"></a>

### `Patch`

```cpp
Patch Petals = Grid(33, 9, Copies = 12, Material = "/Game/FX/M_Petal.M_Petal", Flip = true)
{
    ... HLSL statements ...
}
```

一个 patch 是由 `Columns × Rows` 个顶点组成的网格，重复 `Copies` 次，并有一个计算每个顶点的程序。patch 按写入顺序追加到网格；patch 内的副本也如此。

| 拓扑 | 网格 | 主体获得的辅助项 | 默认 `Position` | 正面 |
| --- | --- | --- | --- | --- |
| `Grid(Columns, Rows)` | 开放薄片 | — | XY 平面中 100 × 100 的正方形 | +Z |
| `Strip(Columns)` | `Grid(Columns, 2)` | — | XY 平面中 100 × 10 的带状区域 | +Z |
| `Ring(Columns, Rows)` | U 沿周向，V 沿径向 | `Theta`, `Dir` | XY 平面中的圆环，半径 50 → 100 | −Z |
| `Tube(Columns, Rows)` | U 沿周向，V 沿 +Z 方向 | `Theta`, `Dir` | 半径 50，高 100 | 向外 |
| `Sphere(Columns, Rows)` | U 沿周向，V 从 −Z 到 +Z | `Theta`, `Phi`, `Dir` | 半径 100 | 向外 |

计数是**顶点**计数。闭合形状会重复其接缝：`Tube` 的第一列和最后一列位于同一位置（`U` 为 0 和 1 时），这正是让 UV 能从 0 → 1 运行而没有接缝的原因。`Tube(65, …)` 周向有 64 段。

`Theta = 2π·U`；对于环或管，`Dir = (cos θ, sin θ, 0)`；在球体上，`Phi = π·(1 − V)` 是相对 +Z 的角度，`Dir` 是单位方向。所有拓扑底层都是同一个网格——类型只决定辅助项和默认值，因此任何形状都可以写在任意一种拓扑上。

对于每种拓扑，正面都是 `cross(dP/dU, dP/dV)` 指向的一侧；表格列出了默认值下的对应位置。这是实测结果，而非推导结果：使用单面材质渲染时，未翻转的 `Grid` 从 +Z 可见、从 −Z 被剔除，并且 `Normals = Auto` 对球体的光照效果与使用显式向外法线的同一球体完全相同。`Flip = true` 会翻转 patch 的三角形（及其 `Auto` 法线）。双面材质不受影响。

选项：`Copies`（≥ 1，默认 1）、`Material`（此 patch 的材质，而非网格的材质）、`Flip`。

<a id="what-a-body-sees"></a>

#### 主体能看到什么

| 名称 | 类型 | |
| --- | --- | --- |
| `U`, `V` | float | 网格坐标，0 → 1（含端点） |
| `I`, `J` | float | 列和行，`0 … NU−1` 和 `0 … NV−1`（整数值） |
| `NU`, `NV` | float | 网格的列数和行数 |
| `K`, `NK` | float | 哪一个副本，以及总共有多少副本 |
| `Rand` | float | 每个副本（以及每个 patch）一个位于 [0, 1) 中的稳定随机数 |
| `T` | float | 自网格生成以来经过的秒数 |
| `Index` | float | 该顶点在整个网格中的索引 |
| `Theta`, `Phi`, `Dir` | | 拓扑辅助项（上表） |
| 你的 `Inputs` | | 按名称 |

并写入：

| 名称 | 类型 | 初始值 |
| --- | --- | --- |
| `Position` | float3 | 拓扑的默认形状 |
| `Color` | float4 | `(1, 1, 1, 1)`；按每通道 8 位存储，因此 0 → 1 |
| `UV0` … `UV<n-1>` | float2 | `UV0 = (U, V)`，其余为 `(0, 0)` —— 完整的 float 精度，是存放 HDR 或逐顶点数据的地方 |
| `Normal`, `Tangent` | float3 | 拓扑的解析基（仅在 `Normals = Explicit` 时写入） |

输出已经声明好了：直接赋值（`Position = …;`），不要再次声明它们。两张表中的每个名称都是保留的，任何以 `DM_` 开头的名称也是如此。函数体是 HLSL，规则与 `.dfm` 函数体相同（[dfm.md](dfm.md)）：`Particles.*` 属性可以读写（`Normals = Auto` 下除外），数据接口 `out` 参数是普通的未初始化局部变量，并且没有运行时循环——模块函数体还会为 CPU VM 编译，而 CPU VM 的 HLSL 前端会拒绝这些循环（DFX6006）。

<a id="renderer-and-draw"></a>

### `Renderer` 与 `Draw`

网格由系统原点处的一个 CPU 粒子通过绑定到该网格的网格渲染器绘制。

```cpp
Renderer = { SortOrderHint = 45; }        // any mesh renderer property (L8)
Draw = {                                  // appended to that particle's ParticleUpdate
    DynamicMaterialParameters(WriteParameterIndex0 = Vector4, Index0Vector4 = (1.0, 0.0, 0.0, 0.0));
    Particles.Scale = (1.0, 1.0, 1.5);
}
```

`Draw` 是普通的堆栈：任何针对绘制粒子的内容都放在这里——它的颜色（`Particles.Color`，即材质的 Particle Color）、动态材质参数、缩放、朝向、位置偏移。以这种方式移动、旋转或缩放整个网格不会产生任何逐顶点开销。

<a id="what-it-lowers-to"></a>

## 它降级为

对于 `System(Name="FX/NS_Shells", Root="Game")` 中带有 patch `Body` 和 `Cap` 的 `DynamicMesh Shell`：

| 部分 | 内容 |
| --- | --- |
| `User.Shell` | `DI<DynamicMesh>`：每个材质一个 section（按首次使用顺序）、顶点/三角形数量、`TexCoords`、颜色与切线标志、`DefaultBounds = Bounds` |
| `/Game/FX/NS_Shells_DynamicMesh/DM_GridTopology` | 生成的模块，由系统的每个 patch 共享：存储粒子的顶点索引，在生成时一次性写入其四边形的两个三角形 |
| `/Game/FX/NS_Shells_DynamicMesh/DM_Shell_Body` (`…_Cap`) | 每个 patch 生成一个模块：序言（索引、`U`/`V`、辅助函数、默认值）、你的函数体、法线估算以及顶点写入 |
| 发射器 `Shell_Body`（`Shell_Cap`） | GPU，局部空间，一次爆发中产生 `Columns × Rows × Copies` 个粒子，每个顶点一个；`ParticleSpawn` 先运行拓扑模块再运行顶点模块，`ParticleUpdate` 每帧运行顶点模块 |
| 发射器 `Shell` | CPU，一个粒子，一个 `MeshRenderer`，其网格绑定到 `User.Shell`；`Visible` 写入粒子的 `VisibilityTag`（0 为绘制，1 为不绘制），渲染器据此过滤，无需材质提供任何帮助 |
| `/Game/FX/NS_Shells_DynamicMesh/DM_Visibility` | 生成的模块，仅在某个块设置 `Visible` 时存在 |

生成的模块是由普通模块生成器构建的普通 `.dfm` 文本，因此 [dfm.md](dfm.md) 中的所有内容对它们同样适用。会保存的构建会把它们的源码写入 `Saved/DreamFX/DynamicMesh/<root>/<system path>_DynamicMesh/DM_*.dfm`，因此可以读到某个 patch 最终变成的确切代码。它们的来源戳记会记录 `.dfs`，因此对其中一个执行 *Open Source* 会打开生成它的块。函数体中的编译错误会报告在该 patch 的 `{`（DFX6006）处，并附带 Niagara 的消息和该文本的路径。

系统的来源哈希涵盖生成的文本和一个降级版本号，因此一个现在会以不同方式降级的块——包括在引擎升级改变了下文所述的纹理坐标探测之后——即使 `.dfs` 没有变化也会重建。

三角形顺序就是 section 内的绘制顺序，并且是固定的：先按源码顺序的 patch，然后是副本，然后是行（V），最后是列（U）。必须层叠的透明层应放在不同的 patch 或副本中，并按从后到前的顺序写入。

<a id="engine-defects-handled-here"></a>

### 此处处理的引擎缺陷

- **纹理坐标（UE 5.8）。** 在 GPU 和 CPU 路径上，`SetVertexTexCoord` 寻址的是 `Vertex·2 + TexCoord·2` 而不是 `(Vertex·NumTexCoords + TexCoord)·2`，而渲染器读取的是交错布局。当 UV 通道多于一个时，生成器在顶点 `Index·TexCoords` 处写入 UV，并将 `NumVertices` 声明为真实数量乘以 `TexCoords`，这样写入就落在渲染器读取的位置并通过边界检查。该探测会读取引擎的动态网格着色器模板，因此修复了该 bug 的引擎会得到普通写入（并且会重建，因为生成的文本发生了变化）。
- **第一帧。** 粒子在生成的那一帧不会运行其更新堆栈，因此顶点模块也会在生成时运行（使用 `T = 0`）：网格在第一帧就是完整的。

<a id="migrating-a-generator-that-writes-modules-by-hand"></a>

## 迁移手写模块的生成器

`SaberAlterFX/Script/SaberAlter/gen_polar_dyn.py` 就是这个块所取代的东西。每个阶段它会写两个模块（生成：三角形；更新：顶点）、一个 Writer 和一个 Draw 发射器、带有 `NumVertices` 乘以 UV 通道数的 `DI<DynamicMesh>` JSON，以及用于从粒子索引找出顶点的层、侧、角度和径向索引的阶跃求和查找。所有这些现在都属于降级。仍然属于项目自己的是数据（形状和颜色纹理）以及把它转换为位置的数学——也就是函数体。

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

迁移中改动了什么，以及为什么每一行都更短：

- **索引。** `U`、`V`、`I`、`J`、`K` 取代了 `Lf` / `VBase` / `Side` / `A` / `J` 的阶跃求和；原来的 `V = J / max(NR - 1, 1)` 就是 `U`。两种分辨率就是两个 patch。
- **关键帧。** 等间距的关键帧是一个 `floor` 和一个 `saturate`，而不是 27 个 `step` 项。不规则的关键帧可以保留为生成器的阶跃求和，写进函数体。
- **逐层常量**（`CRow`、`Seed`、`Erode`、`Pw`）在遵循某种规律时是该层的算术，否则要么是由生成器发出的阶跃求和，要么——更好的做法——是形状纹理的一个备用通道（`S1.yzw` 未使用），这样块保持手写，而生成器只写数据。
- **完全消失：** 两个模块、两个发射器、JSON、`NumVertices × 4`、每次 UV 写入时的 `Vertex * 4`、`ExecIndex` 记账、`ModulePaths` 编辑，以及 `BEGIN POLAR-*` 拼接标记。

在测试宿主中用 C 阶段的纹理重建后，这个块绘制出的扇形与生成版本相同，而且——因为几何现在就是代码——只需改变 `Position` 就能让它从其他角度成立。把 X 项替换为 `-R * 2.508733 * (0.06 + 0.025 * Lf) - 25.0 * Lf` 会把每个描绘层变成一个朝向相机倾斜的锥体，前面的层倾斜更多：从正面看与之前一样，从侧面看则是一个有深度的爆发，而不是一张卡片。

<a id="limits"></a>

## 限制

- 拓扑是构建时固定的四边形网格：数量是字面量，patch 无法在运行时改变其三角形数量。折叠顶点（或将 `Color.a` 置零）来隐藏部分。
- 只有一种绘制顺序，在构建时设定（见上文）。没有逐帧的三角形排序。
- 顶点颜色每通道 8 位；需要更高精度时请使用 UV 通道。
- 网格位于系统的局部空间。引擎的动态网格不会投射阴影、不会参与碰撞，也不会出现在光线追踪中。
- 当 UV 通道多于一个时，顶点缓冲区会比所需大小大 `TexCoords` 倍（即上面的变通方案）。
- 每个 patch 最多 16,777,216 个顶点（索引运算在 float 中到该数值为止仍是精确的），且声明的顶点数乘以 UV 变通方案后必须能放进一个 `int32`。
- 删除或重命名 block 或 patch 时，已生成的模块不会被删除：旧的 `<system>_DynamicMesh/DM_*` 资产会一直保留，直到手动删除（没有任何东西引用它们）。
- 网格和 patch 名称都是普通标识符（DFX3066）；带反引号的名称无法成为资产名。
- `DynamicMesh` 是系统级 block；`.dfe` 无法容纳它。子系统会继承其父级的 block，并通过声明同名 block 来替换其中一个。
- 导出和 `pull` 看到的是降级后的发射器，而不是 block：导出的系统重新导入时呈现为上面这些发射器和模块，而 `pull` 不会改动 `DynamicMesh` block 的文本。
