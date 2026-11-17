<a id="dreamfxlang-cheat-sheet"></a>

# DreamFXLang 速查表

一页讲完整门语言：三种文件类型、系统的形态、值的写法，以及最耗时的那些错误。本页中的完整系统由插件的 gate 构建。要了解某一行背后的规则，请阅读[语言参考](language/README.md)；要查某个代码，请阅读[诊断](diagnostics/README.md)，或同样数据的 JSON 版本[`diagnostics/codes.json`](../diagnostics/codes.json)。

DreamFXLang 不是 HLSL、C++ 或任何其他 Niagara 工具的语法。**模块输入名来自真实资产**：在调用模块之前先运行 `dfx.ps1 schema <Module>`（或在编辑器中查看该模块）。

<a id="files"></a>

## 文件

| 文件 | 声明 | 构建 |
| :-- | :-- | :-- |
| `.dfs` | `System(Name="…", Root="…") { … }` | a `UNiagaraSystem` |
| `.dfe` | `Emitter(Name="…", Root="…") { … }` | 自身不构建任何内容 — 通过 `Emitter X from "…"` 复制到系统中 |
| `.dfm` | `Module(…) { … }` 或 `DynamicInput(…) { … }` | a `UNiagaraScript` |

源文件位于 `DFX/` 下。`Root="Game"`（或留空）构建到 `/Game` 下，`Root="Plugin.MyPlugin"` 构建到该插件下；`Name="Effects/NS_Spark"` 是它下面的路径。

<a id="a-system"></a>

## 系统

```cpp
// DFX/CheatSheet/NS_CheatSheetSparks.dfs -- a burst of sparks that fly out, fall and fade, once a second
System(Name="CheatSheet/NS_CheatSheetSparks", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-200, -200, -50, 200, 200, 300);
    }

    Properties = {                                   // user parameters: User.SparkCount, …
        Group("Burst") {
            int   SparkCount = 24;                   // int: it drives an int32 input
            float SparkSpeed = 450.0;
        }
        Color Tint = (1.0, 0.72, 0.25, 1.0) [ Group="Look" ];
    }

    Emitter Sparks
    {
        Settings = {
            SimTarget          = CPU;
            Determinism        = true;
            RandomSeed         = 7;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(
                LifeCycleMode = Self,                // a static switch: written before what it gates
                LoopBehavior  = Infinite,
                LoopDuration  = 1.0
            );
            SpawnBurst_Instantaneous(SpawnCount = User.SparkCount, SpawnTime = 0.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(  // a partial path picks one of two modules
                LifetimeMode         = Random,
                LifetimeMin          = 0.3,
                LifetimeMax          = 0.6,
                SpriteSizeMode       = RandomUniform,
                UniformSpriteSizeMin = 3.0,
                UniformSpriteSizeMax = 8.0
            );
            SystemLocation();
            AddVelocityInCone(
                ConeAngle        = 40.0,
                VelocityStrength = RandomRangeFloat(Minimum = User.SparkSpeed * 0.6, Maximum = User.SparkSpeed)
            );
        }

        ParticleUpdate = {
            ParticleState();                         // advances NormalizedAge; age-driven inputs need it
            GravityForce(Gravity = (0, 0, -680));
            Drag(Drag = 1.4);
            SolveForcesAndVelocity();
            // An hlsl value carries no type, so annotate it. Its body is pasted verbatim: no comments inside.
            Color Particles.Color = hlsl {
                float4(User.Tint.rgb, saturate(1.0 - Particles.NormalizedAge))
            };
        }

        SpriteRenderer Core
        {
            Alignment  = VelocityAligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;
        }
    }
}
```

- 栈：`SystemSpawn` 和 `SystemUpdate` 位于顶层；`EmitterSpawn`、`EmitterUpdate`、`ParticleSpawn`、`ParticleUpdate` 位于发射器中，外加 `OnEvent(…) = { }` 和 `Stage Name(…) = { }`。书写顺序即模块顺序。未声明的栈保持不变；`SystemUpdate = { }` 会将其清空。
- `Settings` 和渲染器属性都从实际资产中读取：拼写错误会报告真实的列表。
- 渲染器的声明顺序即绘制顺序。

<a id="statements-in-a-stack"></a>

## 栈中的语句

```cpp
ModuleName(Input = Value, Input = Value);          // a module call: arguments are always named
Spawn/Initialization/V2/InitializeParticle(…);      // a partial path when the short name is ambiguous
disabled GravityForce(Gravity = (0, 0, -980));      // kept in the stack, not executed
Particles.Moon.Seed = 0.5;                          // an assignment; the first write declares the attribute
Color Particles.Tint = User.Tint;                   // ...consecutive assignments fold into one Set Parameters
```

输入名会去掉 Niagara 中的空格：`Loop Duration` 即 `LoopDuration`。不是标识符的名称用反引号括起，作为一个 token：`` `User.PillarPower(0~1)` ``。枚举值按名称书写，不带空格：`DirectSet`。

<a id="values"></a>

## 值

```cpp
InitializeParticle(
    Lifetime       = 0.25,                                     // literal
    Color          = User.TintA,                               // linked
    SpriteSize     = RandomRangeFloat(Minimum = 1, Maximum = 4), // dynamic input
);

Color Particles.Color = hlsl {                                 // HLSL expression
    float4(Particles.Color.rgb, saturate(1.0 - Particles.NormalizedAge))
};
```

```cpp
450.0        24           true
(1, 0, 0)    (1.0, 0.72, 0.25, 1.0)      // 2, 3 or 4 components
"/Game/FX/M_Spark"                        // asset path
box(-200, -200, -50, 200, 200, 300)       // six numbers: min xyz, max xyz
```

- 链接：任何带命名空间的参数——`User.`、`Particles.`、`Emitter.`、`System.`、`Engine.`——都直接绑定，因此类型必须完全匹配（`float` 不能驱动 `int32`；请将其声明为 `int`）。
- 内联表达式：`+ - * /`、括号和 `normalize saturate clamp lerp frac min max abs floor ceil pow sqrt dot cross length int`，均为位置参数。其他任何内容都放进 `hlsl { }`，且只能是一个表达式。
- `int` → `float` 是隐式的；`float` → `int` 需要 `int(…)`。
- 类型：`float` `int` `bool`、`Vector2` `Vector` `Vector4`、`Color`、`Position`、`Quat`、`DI<X>`（裸数据接口名表示 `DI<Name>`：`Texture` 即 `DI<Texture>`；没有 `Texture2D`）。

在需要曲线数据接口的地方，表示一条曲线：

```cpp
UniformScaleFactor = FloatFromCurve(
    FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
    CurveIndex = Particles.NormalizedAge
)
```

<a id="properties-and-renderers"></a>

## 属性与渲染器

```cpp
Properties = {
    int              SparkCount = 24                     [ Group="Burst"; SortPriority=10 ];
    float            SparkSpeed = 450.0;
    Color            TintA      = (1.0, 0.72, 0.25, 1.0);
    Vector           HitNormal  = (0, 0, 1)              [ Description="Impact normal from blueprint" ];
    Texture          NoiseTex   = "{\"Texture\":\"/MoonToon/Textures/T_Noise01.T_Noise01\"}";
    DI<SkeletalMesh> TargetMesh;
}
```

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

`Bind` 设置属性绑定；`Meshes = ["/Engine/BasicShapes/Cube"];` 列出网格渲染器的网格（并在 `OverrideMaterials` 之前使用 `bOverrideMaterials = true;`）。

<a id="emitters-from-a-dfe"></a>

## 从 `.dfe` 引入发射器

```cpp
Emitter Flash from "../Emitters/E_MoonFlashCard"
{
    EmitterUpdate = {
        EmitterState(LifeCycleMode = Self, LoopBehavior = Once, LoopDuration = 0.08);
    }
}
```

`from` 会复制一次 `.dfe`，然后由块覆盖它；这不是继承。

<a id="modules-and-dynamic-inputs--dfm"></a>

## 模块与动态输入 — `.dfm`

```cpp
// A module: goes in a stack, reads and writes attributes.
Module(Name="Modules/Moon/ToonSpin", Root="Plugin.DreamFX")
{
    Settings = {
        Usage       = ParticleUpdate;
        Category    = "MoonToon|Motion";
        Description = "Spins a sprite at a constant rate, optionally reversed.";
    }

    Inputs = {
        float SpinRate   = 90.0  [ Description="Degrees per second." ];
        bool  bClockwise = true  [ StaticSwitch ];
        float RateScale  = 1.0   [ Advanced ];
    }

    Body = {
        float Dir = bClockwise ? 1.0 : -1.0;
        Particles.SpriteRotation += SpinRate * RateScale * Dir * Engine.DeltaTime;
    }
}
```

```cpp
// A dynamic input: computes a value for an input slot.
DynamicInput(Name="Modules/Moon/ToonPulse", Root="Plugin.DreamFX")
{
    Settings = { Usage = DynamicInput; Output = float; Category = "MoonToon|Math"; }
    Inputs   = { float Frequency = 6.0; float Sharpness = 2.0; }
    Body     = {
        return pow(0.5 + 0.5 * sin(Engine.Time * Frequency * 6.2831853), Sharpness);
    }
}
```

像任何模块一样从栈中调用：`ToonSpin(SpinRate = 220.0);`；凡是写值的地方都可以用动态输入：`UniformScaleFactor = ToonPulse(Frequency = 4.0)`。

<a id="mistakes-that-cost-the-most-time"></a>

## 最耗时的错误

| 代码 | 严重性 | 消息 |
| :-- | :-- | :-- |
| [DFX3001](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3001) | 错误 | 该模块名在搜索路径上未解析到任何内容。 |
| [DFX3003](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3003) | 错误 | 模块 '%s' 没有名为 '%s' 的输入。%s 可用输入：%s |
| [DFX2008](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx2xxx/#dfx2008) | 错误 | 模块 '%s' 被传入了位置参数。模块输入必须写成 'Name = Value'。 |
| [DFX4027](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4027) | 错误 | '%s' 是 %s，但 '%s' 是 %s。链接会直接绑定参数——不存在类型转换。请将 '%s' 声明为 %s，或用其他方式驱动该输入。 |
| [DFX4003](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4003) | 错误 | 输入 '%s' 是整数，但写入的是 %s。窄化不是隐式的——如果确实想要截断，请写成 int(%s)。 |
| [DFX4022](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4022) | 错误 | 无法从值推断 '%s' 的类型。对新属性的首次赋值必须使用字面量，这样其类型才没有歧义。 |
| [DFX4030](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4030) | 错误 | '%s' 的 hlsl 块必须是单个表达式：不能有语句、局部变量，也不能有 return。请将多语句逻辑移到 .dfm DynamicInput 中，并在此处调用它。 |
| [DFX4031](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4031) | 错误 | '%s' 不是允许的内联函数。允许的函数：%s。其他任何内容都应放在 .dfm 动态输入或 hlsl { } 块中。 |
| [DFX4025](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx4xxx/#dfx4025) | 错误 | '%s' 不是有效的赋值目标。参数名需要带命名空间限定，例如 Particles.MyValue 或 Emitter.MyCounter。 |
| [DFX3004](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx3xxx/#dfx3004) | 错误 | 未知的渲染器类型 '%s'。应为 SpriteRenderer、MeshRenderer、RibbonRenderer、LightRenderer、DecalRenderer、ComponentRenderer、VolumeRenderer 之一，或任何 UNiagaraRendererProperties 子类。 |
| [DFX5033](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx5xxx/#dfx5033) | 错误 | 发射器 '%s' 声明了 Stage 块，但在 CPU 上模拟。模拟阶段是 GPU 功能：请在发射器的 Settings 中设置 `SimTarget = GPU`。 |
| [DFX6004](https://fx.toolchain.64hz.cn/en/docs/diagnostics/dfx6xxx/#dfx6004) | 警告 | 一条警告级别的 Niagara 栈问题。 |

- 猜测的模块输入名对应 `DFX3003`，它会列出真实的输入名。请先阅读 schema。
- 静态开关会控制它之后的输入：把 `LoopBehavior` 写在 `LoopDuration` 之前。
- `hlsl { }` 主体会原样粘贴到着色器中：其中的 `//` 注释可能会把表达式的其余部分注释掉，导致 Niagara 编译失败并报出 `DFX6001`。请把注释写在它上方。
- 若省略 `ParticleState()`，每个由 age 驱动的输入都会静默地保持 age 为零。
- `lint` 可在不触碰资产的情况下检查文件；`build -Force` 可验证这一点；退出码就是错误数量。

<!-- Generated by .skill/gen-knowledge.ps1 from .skill/cheatsheet/cheatsheet.src.md. Edit that file, not this one. -->
