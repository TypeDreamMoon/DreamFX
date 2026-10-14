---
title: "概览"
description: "DreamFXLang 是 Unreal Niagara 特效的文本优先语言，由 DreamFX 插件编译成标准 Niagara 资产——并能再反编译回来。"
---

DreamFXLang 是 Unreal Engine Niagara 的文本优先创作语言。你编写 `.dfs`、`.dfe`
和 `.dfm` 文件，**DreamFX** 插件将它们编译成标准资产——
`UNiagaraSystem`、`UNiagaraEmitter`、`UNiagaraScript`——关卡、蓝图或 Sequencer
引用它们的方式与引用任何其他资产完全一样。

源文件才是你维护的东西。`.uasset` 是构建产物：随时可以丢弃并
重新生成，绝不手工编辑。

反方向同样成立：**任何**现有 Niagara 系统都能反编译回 `.dfs`。
这不是便利导出，而是一份契约——语言无法表达的任何内容都会
作为显式缺口写进文件头，绝不静默丢弃。

| | |
| --- | --- |
| 插件版本 | `1.0.0` |
| 引擎 | Unreal Engine `5.8`（源码构建与安装版引擎共享**同一份源码树，零 `#if` 分叉**） |
| 模块 | `DreamFX`（Runtime）、`DreamFXEditor`（Editor） |
| 源文件扩展名 | `.dfs` · `.dfe` · `.dfm` |
| 项目设置 | *Project Settings ▸ Plugins ▸ DreamFX* |
| 诊断 | [`DFXnnnn` 代码目录](diagnostics/README.md)，带文件、行和列 |
| 许可证 | MIT |

<a id="three-kinds-of-source-file"></a>

## 三种源文件

| 扩展名 | 声明 | 产物 |
| --- | --- | --- |
| `.dfs` | `System` | 一个 `UNiagaraSystem` |
| `.dfe` | `Emitter` | 自身不产任何东西——由 `from` 合并进 `.dfs` |
| `.dfm` | `Module` / `DynamicInput` | 一个 `UNiagaraScript`（通过反射后端在安装版引擎上同样可用） |

三者共用一个文件头：

```cpp
<Kind>(Name="<path under the root>", Root="<root token>")
{
    ...
}
```

`Kind` 必须与文件扩展名一致（[DFX2021](diagnostics/DFX2xxx.md#dfx2021)）。`Root` 是
`Game`、空（同义）或 `Plugin.<PluginName>`；`Name` 是相对那个
根的内容目录的资产路径。见[文件模型](language/README.md)。

<a id="a-minimal-system"></a>

## 最小系统

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];   // exposed as User.Speed
    }

    Emitter Motes
    {
        Settings = { SimTarget = CPU; Determinism = true; RandomSeed = 1; }

        EmitterUpdate = {
            EmitterState(LifeCycleMode = Self, LoopBehavior = Infinite);
            SpawnRate(SpawnRate = 20.0);
        }

        ParticleSpawn = {
            Spawn/Initialization/V2/InitializeParticle(
                LifetimeMode = DirectSet, Lifetime = 2.0,
                SpriteSizeMode = Uniform, UniformSpriteSize = 8.0
            );
            SystemLocation();
            AddVelocityInCone(ConeAngle = 30.0, VelocityStrength = User.Speed);
        }

        ParticleUpdate = {
            ParticleState();
            GravityForce(Gravity = (0, 0, -400));
            SolveForcesAndVelocity();
        }

        SpriteRenderer Core
        {
            Alignment = Unaligned;  FacingMode = FaceCamera;  SortMode = ViewDepth;
        }
    }
}
```

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

这会写出 `/Game/Effects/NS_Hello`，全程无需编辑器参与。编辑器打开时，保存源文件
即重建——把生成的资产在 Niagara 编辑器中开着，它就是这套工作流的实时预览。

> **注意：**
>
> 写包的命令（`build`、`corpus`、`mirror-diff`、`decompile-all`）要求**关闭编辑器**。
> 两个进程保存同一批包会静默竞争，谁后保存谁赢。
> 见[日常循环](getting-started/workflow.md)。

<a id="what-the-language-covers"></a>

## 语言覆盖到哪里

- **用户参数**，包括带 JSON 配置的数据接口参数（`DI<X>`）；
- **六个栈**：`SystemSpawn` / `SystemUpdate` / `EmitterSpawn` / `EmitterUpdate` /
  `ParticleSpawn` / `ParticleUpdate`；
- **事件处理器** `OnEvent(...)` 和**具名模拟阶段** `Stage <Name>(...)`；
- **渲染器**，带 schema 驱动属性和 `Bind`；
- 全部值形态：字面量、链接、枚举、嵌套动态输入、`hlsl { }`、带
  切线模式的 `curve { }`，以及所有位置的静态开关。

不覆盖的，无论出于设计还是尚未实现：Scratch Pad、模块内部图 lowering、GPU/CPU 分支
条件和 Scalability 条件。原生发射器继承通过
[`inherits`](language/native-emitter-inheritance.md) 支持；`from` 则改为拷贝和合并源文件。
剩余边界见[限制](diagnostics/limitations.md)。

<a id="round-trip-verified-four-ways"></a>

## 四层往返验证

反编译的正确性是量出来的，不是声称的：

| 层 | 回答什么问题 |
| --- | --- |
| **L1** `mirror-diff` | 镜像的导出与原件逐行一致吗？ |
| **L2** | 镜像编译干净吗？ |
| **L3** | 重建的系统**模拟起来**和原件一样吗？（固定步长 SimCache，逐帧计数） |
| **asset-diff** | 两者**作为资产**一致吗——反射走查的事实，不依赖导出器？ |

细节见[往返与等价](generation/roundtrip.md)。

<a id="where-to-start"></a>

## 从哪里开始

  - **[快速上手](getting-started/installation.md)** — 确认插件已启用，写出第一个 `.dfs`，并学会保存即重建的循环。
  - **[语言参考](language/README.md)** — 文件模型、三种文件、栈与语句、值，以及八条 `L*` 规则。
  - **[事件与 Stage](language/events-and-stages.md)** — `OnEvent` 处理器、具名模拟阶段，以及各自的硬限制。
  - **[生成与往返](generation/pipeline.md)** — 构建流水线、溯源、反编译、Export 与 Adopt、引擎与后端。
  - **[工具链](tools/headless-compilation.md)** — `dfx.ps1`、`ci.ps1`、编辑器集成、VSCode workspace、Python 接口。
- **[诊断](diagnostics/overview.md)** — 生成的 `DFXnnnn` 目录，分八族，每条都带成因与修法。
  - **[限制](diagnostics/limitations.md)** — 哪些不覆盖、1.0.0 已知问题，以及环境要求。
  - **[更新日志](changelog/index.md)** — 每个版本覆盖了什么。
