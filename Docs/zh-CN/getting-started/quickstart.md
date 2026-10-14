---
title: "DreamFX 快速入门"
description: "DreamFX 快速入门"
---

<a id="getting-started-with-dreamfx"></a>

# DreamFX 快速入门

DreamFX 将文本编译为标准的 Niagara 资产。你编写一个 `.dfs` 文件，运行一条命令，即可得到一个 `UNiagaraSystem`，关卡、蓝图或 Sequencer 都可以像引用其他资产一样引用它。文本是源，`.uasset` 是构建产物，不手动编辑。

本页将带你从零开始得到一个可运行的效果。十五分钟，无需编辑器。

---

<a id="1-check-the-plugin-is-on"></a>

## 1. 确认插件已启用

`DreamFX` 和 `DreamFXEditor` 是编辑器模块，它们会随编辑器和 commandlet 一起加载。
确认驱动能找到引擎：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list
```

这会打印默认搜索路径上的所有 Niagara 模块。如果在打印任何内容之前就报错，说明引擎无法解析（驱动会在注册表和 Epic Games Launcher 的安装清单中查找项目的 `EngineAssociation`）——传入 `-Engine <root>` 或设置 `UE_ENGINE_ROOT`。

---

<a id="2-where-source-lives"></a>

## 2. 源文件的位置

源文件位于 `DFX/` 目录下，可以是项目自身的，也可以是某个插件的：

```
DevTest/DFX/                        Root=""  or  Root="Game"     -> /Game
DevTest/Plugins/MoonToon/DFX/       Root="Plugin.MoonToon"       -> /MoonToon
```

文件头部的 `Root="..."` 指明其 `Name="..."` 相对于哪个内容根目录。位于 `DFX/Effects/NS_Spark.dfs`、声明了 `Name="Effects/NS_Spark"` 且带有 `Root="Game"` 的文件会构建出 `/Game/Effects/NS_Spark`。

三种扩展名：

| 文件 | 声明内容 | 产出 |
| --- | --- | --- |
| `.dfs` | `System` | `UNiagaraSystem` |
| `.dfe` | `Emitter` | 自身不产出任何内容——由 `from` 合并到一个 `.dfs` 中 |
| `.dfm` | `Module` / `DynamicInput` | `UNiagaraScript` |

---

<a id="3-your-first-effect"></a>

## 3. 你的第一个效果

创建 `DFX/Effects/NS_Hello.dfs`：

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-100, -100, -100, 100, 100, 100);
    }

    // Exposed to blueprint as User.Speed, and settable with SetNiagaraVariableFloat.
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];
    }

    Emitter Motes
    {
        Settings = {
            SimTarget          = CPU;
            Determinism        = true;
            RandomSeed         = 1;
            AllocationMode     = Fixed;
            PreAllocationCount = 64;
        }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode      = DirectSet,
                Lifetime          = 2.0,
                SpriteSizeMode    = Uniform,
                UniformSpriteSize = 8.0
            );
            SystemLocation();
            AddVelocityInCone(ConeAngle = 30.0, VelocityStrength = User.Speed);
        }

        ParticleUpdate = {
            // Advances NormalizedAge. Anything age-driven needs it, and Niagara reports an unmet
            // dependency if it is missing.
            ParticleState();
            GravityForce(Gravity = (0, 0, -400));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core
        {
            Alignment  = Unaligned;
            FacingMode = FaceCamera;
            SortMode   = ViewDepth;
        }
    }
}
```

构建它：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

你应该会看到 `1 built, 0 up to date, 0 failed | 0 error(s)`，并且该资产会列在"本次运行写入磁盘的资产"下。在编辑器中打开 `/Game/Effects/NS_Hello`，它就能播放。

不做任何修改再次构建会报告 `0 built, 1 up to date`——资产上的来源标记保有一个源文件的哈希，哈希匹配就意味着无需做任何事。`-Force` 可覆盖此行为。

---

<a id="4-when-it-does-not-compile"></a>

## 4. 当编译失败时

每条诊断都是一个 `DFXnnnn`，带有文件、行和列：

```
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

可以借助的两样东西：

- **[diagnostics/](../diagnostics/README.md)** —— 每个错误码，及其成因和修复方法。
- **`dfx.ps1 schema <Module>`** —— 从资产中读取的模块真实输入签名。它是名称方面的权威：Niagara 输入名称包含空格（`Loop Duration`），DreamFX 会对其进行规范化，而有些输入只有在其上方的静态开关被设置后才存在。

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce
```

---

<a id="5-iterating"></a>

## 5. 迭代

保存并重新构建已经接好：编辑器会监视每个 `DFX/` 根目录，并在源文件保存时重新构建它。在 Niagara 编辑器中打开生成的资产，它会在每次重新构建时刷新——这就是实时预览，无需任何配置。

保存只写入自上次构建以来发生变化的内容——修改一个值只需一刹那，而不用重新构建整个系统——并且当它无法信任资产的最后状态时，会自动回退到完整重新构建。参见 [tools/incremental-builds.md](../tools/incremental-builds.md)。

有一条规则，且不可选：**在重新构建系统之前先销毁该系统的实例。** 在热切换期间，活跃的 `NiagaraActor` 可能会看到半构建状态的渲染器布局，并在渲染工作线程上断言，从而把编辑器一起带崩。这是引擎的竞态问题，不是 DreamFX 能防范的。

---

<a id="6-working-from-inside-the-editor"></a>

## 6. 在编辑器内部操作

以上所有内容都可以无头运行，而每一项也都有对应的按钮。按钮调用的是同一套代码——菜单只是入口，绝不是第二套实现。

| 位置 | 内容 |
| --- | --- |
| *工具 ▸ DreamFX* | **Rebuild DFX**、**Verify DFX**、**Open DreamFX Workspace (VSCode)** |
| 关卡编辑器工具栏 | **DFX**（= 重新构建）和工作区按钮 |
| 右键单击 Niagara System | **DreamFX ▸** 导出 .dfs / 采纳，或者——如果该资产是 DreamFX 生成的——打开源文件 / 从源文件重新构建 / 校验 |
| 右键单击 Niagara Emitter | **DreamFX ▸ 导出 .dfe** |
| Niagara 系统编辑器工具栏 | 针对正在编辑的系统的同一个 **DreamFX** 菜单 |

其中有两项在使用前值得先了解一下：

- **Open DreamFX Workspace** 会写入 `DFX/DreamFX.code-workspace`，每个源根目录对应一个文件夹，并在 VSCode 中打开它。该文件每次都会被完整重写，所以请把每用户的设置放在 `DFX/.vscode/` 中。
- **采纳（Adopt）** 不是导出。导出会写入一份你可以阅读*并编辑*的副本：保存它会重新构建一个位于 `/<mount>/Decompiled/<original directory>/<asset>` 的镜像，而绝不是它读取自的那个资产。采纳则让文本成为该资产的唯一事实来源，并且当资产的任何方面无法被表达时会拒绝执行。完整对比见 [tools/editor-integration.md](../tools/editor-integration.md#export-vs-adopt)。

构建失败现在会弹出吐司通知，带有一个直接跳转到出错行的 **Open in VSCode** 链接，而"此资产为生成资产"的警告也带有一个指向其源文件的链接。

使用 `-NoDreamFXEditor` 启动可关闭整个交互界面——菜单、监视器和守卫——当 DreamFX 本身就是被调试对象时使用。

---

<a id="7-before-you-commit"></a>

## 7. 提交之前

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

四个步骤，每一步都能捕获其他步骤无法捕获的问题：

| 步骤 | 捕获 |
| --- | --- |
| `lint` | 没有边界的 GPU 发射器、没有上限的生成速率、没有种子的随机性 |
| `build` | 任何阻止文本成为资产的问题 |
| `verify` | **源文件被编辑并提交却没有重新构建**——单独执行构建会通过，因为构建会修复它 |
| `corpus` | *行为*发生变化而非破坏：诊断位置移动、拓扑结构变化、反编译失去幂等性 |

`verify` 是那个值得理解的。它会把每个资产的来源标记与其源文件进行比较；不匹配意味着已提交的资产和已提交的文本不一致，这在其他任何方式下都是不可见的。

**先关闭编辑器。** `build` 和 `corpus` 会写入 `.uasset` 文件，而在同一项目上打开的编辑器也会写入它们——后保存的一方获胜，而且不会有任何提示。运行时检测到编辑器进程时会发出警告，但它无法判断该编辑器打开的是哪个项目，所以该警告只是建议而非门禁。

---

<a id="where-to-go-next"></a>

## 接下来去哪里

- **[language/](../language/README.md)** —— 完整的语言：[`.dfs`](../language/dfs.md)、[`.dfe`](../language/dfe.md)、[`.dfm`](../language/dfm.md)，以及 [值与规则](../language/values.md)。
- **[tools/editor-integration.md](../tools/editor-integration.md)** —— 每个菜单、每个吐司通知、工作区文件、VSCode 发现机制，以及如何从 Python 调用相同的命令。
- **[tools/incremental-builds.md](../tools/incremental-builds.md)** —— 保存会写入什么、每种编辑的代价，以及构建何时会回退到完整重新构建。
- **[diagnostics/](../diagnostics/README.md)** —— 每一个 `DFXnnnn`。
- **[../CHANGELOG.md](../../../CHANGELOG.md)** —— 每个版本涵盖的内容，以及已知问题列表。

要对你自己的内容提出覆盖度问题——其中有多大比例 DreamFX 已经能够表示，哪些缺口值得填补——你可以自己运行：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff       # L1 text + L2 compile
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff        # facts, independent of the exporter
```

有四项技能可用于代理辅助工作：`dream-fx-create`、`dream-fx-verify`、`dream-fx-diagnose` 和 `dream-fx-decompile`。
