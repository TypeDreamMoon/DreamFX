---
title: 概览
description: DreamFXLang 是 Unreal Niagara 特效的文本化语言，由 DreamFX 插件编译成标准 Niagara 资产，并能把现有系统反编译回源码。
---

DreamFXLang 是 Unreal Engine Niagara 特效的文本化语言。你写 `.dfs`、`.dfe` 和 `.dfm` 文件，
**DreamFX** 插件把它们编译成标准资产 —— `UNiagaraSystem`、`UNiagaraEmitter`、`UNiagaraScript` ——
关卡、蓝图、Sequencer 引用它们的方式和引用任何别的资产完全一样。

源文件才是你维护的东西。`.uasset` 是构建产物，随时可以扔掉重建，不手改。

反方向同样成立：**任何**现有 Niagara 系统都能反编译回 `.dfs`。这不是便利导出，是有契约的往返 ——
语言表达不了的东西逐条写进文件头缺口注释，绝不静默丢弃。

| | |
| --- | --- |
| 插件版本 | `1.0.0` |
| 引擎 | Unreal Engine `5.8`（自维护源码引擎与预编译安装版**同一份源码零 `#if` 分叉**） |
| 模块 | `DreamFX`（Runtime）、`DreamFXEditor`（Editor） |
| 源文件扩展名 | `.dfs` · `.dfe` · `.dfm` |
| 项目设置 | *Project Settings ▸ Plugins ▸ DreamFX* |
| 诊断码 | 143 个 `DFXnnnn`，全带文件 / 行 / 列 |
| 许可证 | MIT |

<a id="三种源文件"></a>

## 三种源文件

| 扩展名 | 声明 | 产物 |
| --- | --- | --- |
| `.dfs` | `System` | 一个 `UNiagaraSystem` |
| `.dfe` | `Emitter` | 自身不产资产 —— 由 `.dfs` 的 `from` 拷入 |
| `.dfm` | `Module` / `DynamicInput` | 一个 `UNiagaraScript`（预编译引擎经反射后端同样可用） |

三者共用一个文件头：

```cpp
<Kind>(Name="<相对内容根的路径>", Root="<根记号>")
{
    ...
}
```

`Kind` 必须与扩展名一致（[DFX2021](diagnostics/DFX2xxx.md#dfx2021)）。`Root` 是 `Game`、
空串（同义）或 `Plugin.<插件名>`，`Name` 是相对那个内容根的资产路径。详见
[文件模型](language/README.md)。

<a id="最小系统"></a>

## 最小系统

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];   // 暴露为 User.Speed
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

生成 `/Game/Effects/NS_Hello`，全程不需要打开编辑器。编辑器开着时，存盘即重建 ——
把生成的资产在 Niagara 编辑器里开着，它就是这套文本工作流的实时预览。

**提示：**

写包的命令（`build`、`corpus`、`mirror-diff`、`decompile-all`）**要关掉编辑器**。
  两个进程存同一批包，谁后存谁赢，而且两边都不吭声。见[日常循环](getting-started/workflow.md)。

<a id="语言覆盖到哪里"></a>

## 语言覆盖到哪里

- **user 参数**，含带 JSON 配置的数据接口参数（`DI<X>`）；
- **六个固定栈**：`SystemSpawn` / `SystemUpdate` / `EmitterSpawn` / `EmitterUpdate` /
  `ParticleSpawn` / `ParticleUpdate`；
- **事件处理器** `OnEvent(...)` 与**具名 simulation stage** `Stage <Name>(...)`；
- **renderer** 的 schema 驱动属性与 `Bind` 绑定；
- 全部值形态：字面量、linked、枚举、嵌套 dynamic input、`hlsl { }`、带切线模式的 `curve { }`，
  以及所有位置的静态开关。

不覆盖的（设计使然或尚未做）：Scratch Pad、模块内部图 lowering、GPU/CPU 条件分支、
Scalability 条件逻辑、真正的 emitter 继承（`from` 是拷贝）。降级全部有诊断，绝不静默 ——
见[能力边界](diagnostics/limitations.md)。

<a id="四层往返验证"></a>

## 四层往返验证

反编译的正确性不是声称的，是量出来的：

| 层 | 回答什么问题 |
| --- | --- |
| **L1** `mirror-diff` | 镜像的导出与原件的导出逐行相同吗？ |
| **L2** | 镜像编译干净吗？ |
| **L3** | 重建的系统**模拟起来**和原件一样吗？（固定步长 SimCache 逐帧粒子数） |
| **asset-diff** | 两个资产**作为资产**一致吗？—— 不经导出器的反射走查，含编译器自己的视角 |

细节见[往返与等价](generation/roundtrip.md)。

<a id="从哪里开始"></a>

## 从哪里开始

- [快速上手](getting-started/installation.md) — 确认插件在线、写出第一个 `.dfs`、跑通保存即重建的循环。
  - [语言参考](language/README.md) — 文件模型、三种文件、栈与语句、值与类型、八条 `L*` 规则。
  - [事件与 Stage](language/events-and-stages.md) — `OnEvent` 事件处理器、具名 simulation stage，以及各自的硬限制。
  - [生成与往返](generation/pipeline.md) — 构建流水线、溯源戳、反编译、Export 与 Adopt、双引擎与反射后端。
  - [工具链](tools/headless-compilation.md) — `dfx.ps1`、`ci.ps1`、编辑器集成、VSCode workspace、Python 脚本接口。
  - [诊断码](diagnostics/overview.md) — 143 个 `DFXnnnn`，按流水线阶段分成八族，每条都带成因与修法。
  - [能力边界](diagnostics/limitations.md) — 哪些东西不覆盖、1.0.0 的已知问题，以及运行环境要求。
  - [更新日志](changelog/index.md) — 每个版本覆盖了什么。
