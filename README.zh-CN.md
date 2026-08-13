<p align="center">
  <img alt="DreamFX banner" src="./Images/banner.png" />
</p>

<table>
  <tr>
    <td width="64%" valign="top">
      <h1>DreamFX</h1>
      <p><strong>用文本源码编写 Unreal Niagara 特效 —— DreamFXLang。</strong></p>
      <p>
        DreamFX 把 <code>.dfs</code>、<code>.dfe</code>、<code>.dfm</code> 源文件编译成标准的
        <code>UNiagaraSystem</code> / <code>UNiagaraEmitter</code> / <code>UNiagaraScript</code> 资产,
        也能把任意现有 Niagara 系统<strong>反编译回源码</strong>。文本是唯一真相,
        <code>.uasset</code> 是构建产物,随时可以扔掉重建,不手改。
      </p>
      <p>
        <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-313131" />
        <img alt="Version 1.0.0" src="https://img.shields.io/badge/version-1.0.0-blue" />
        <img alt="Works on stock engine" src="https://img.shields.io/badge/%E9%A2%84%E7%BC%96%E8%AF%91%E5%BC%95%E6%93%8E-%E6%94%AF%E6%8C%81-green" />
      </p>
      <p>
        <a href="README.md">English</a> &nbsp;·&nbsp;
        <a href="Docs/getting-started.md">快速上手</a> &nbsp;·&nbsp;
        <a href="Docs/language/README.md">语法参考</a> &nbsp;·&nbsp;
        <a href="Docs/diagnostics/README.md">诊断码</a> &nbsp;·&nbsp;
        <a href="Docs/tools/editor-integration.md">编辑器工具</a> &nbsp;·&nbsp;
        <a href=".skill/">AI 技能</a> &nbsp;·&nbsp;
        <a href="CHANGELOG.md">更新日志</a>
      </p>
      <p>
        <a href="https://github.com/TypeDreamMoon/DreamFX/issues">
          <img alt="Issues" src="https://img.shields.io/github/issues/TypeDreamMoon/DreamFX" />
        </a>
        <a href=".skill/">
          <img alt="Agent skills" src="https://img.shields.io/badge/Agent%20skills-4-8A2BE2" />
        </a>
        <a href="https://github.com/TypeDreamMoon/DreamShader">
          <img alt="Sister project DreamShader" src="https://img.shields.io/badge/%E5%A7%8A%E5%A6%B9%E9%A1%B9%E7%9B%AE-DreamShader-181717" />
        </a>
      </p>
    </td>
    <td width="36%" align="center" valign="middle">
      <img src="./Images/character.png" width="260" alt="DreamFX character" />
    </td>
  </tr>
</table>

> [!TIP]
> 把所有 `.dfs` / `.dfe` / `.dfm` 纳入版本控制。生成的 Niagara 资产随时能从源码重建,所以它们不需要。

---

## 长什么样

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Properties = {
        float Speed = 150.0 [ Group="Motion" ];   // 暴露为 User.Speed
    }

    Emitter Motes
    {
        Settings = {
            SimTarget = CPU;  Determinism = true;  RandomSeed = 1;
            AllocationMode = Fixed;  PreAllocationCount = 64;
        }

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

全程无需打开编辑器——生成是 headless 的。编辑器开着时,存盘即重建(file watcher),
Niagara 预览窗口就是文本工作流的实时预览。

语言覆盖完整创作面:user 参数(含**带配置的 DI 参数**)、六个 system/emitter 栈、
**事件处理器**(`OnEvent(...)`)、**具名 Simulation Stage**(迭代源、绑定、`ExecuteBehavior`、
`NumIterations`/`Enabled` 收值**或**驱动参数)、schema 驱动的 renderer 属性与 `Bind`、
全部值形态的静态开关、嵌套 dynamic input、`hlsl { }` 块、带切线模式的 `curve { }` 字面量。

## 快速开始

```bash
# 1. 探活:驱动脚本能不能摸到引擎
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 list

# 2. 建一个文件 / 只校验不写 / 只 lint
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build  DFX/Effects/NS_Hello.dfs
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 verify DFX/Effects/NS_Hello.dfs
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 lint   DFX/Effects/NS_Hello.dfs

# 3. 把现有 Niagara 系统接进文本世界
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile /Game/VFX/NS_Explosion

# 4. 全树 CI 四步
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

十五分钟走完一遍:[Docs/getting-started.md](Docs/getting-started.md)。

## 生成什么

| 文件 | 声明 | 产物 |
| :-- | :-- | :-- |
| `.dfs` | `System` | `UNiagaraSystem` |
| `.dfe` | `Emitter` | 自身不产资产——由 `.dfs` 的 `from` 拷入 |
| `.dfm` | `Module` / `DynamicInput` | `UNiagaraScript`(预编译引擎经反射后端同样可用) |

## 往返,四层验证

反编译不是便利导出,是契约:语言表达不了的东西**逐条写进文件头缺口注释**,绝不静默丢;
`Adopt` 接管一个资产前,先重导出做逐字节比对,对不上就拒绝。

| 层 | 回答什么问题 |
| :-- | :-- |
| **L1** `mirror-diff` | 镜像的导出与原件的导出逐行相同吗? |
| **L2** | 镜像编译干净吗? |
| **L3** | 重建的系统**模拟起来**和原件一样吗?(固定步长 SimCache 逐帧粒子数,随机内容用 A-vs-A 自对照裁决) |
| **asset-diff** | 两个资产**作为资产**一致吗?——不经导出器的反射走查,含编译器视角的事实族 |

55 条语料在此之上还比「按夹具建出的资产」与「按其导出重建的资产」——文本比对两侧对称的丢失
也藏不住。导出落在 `Decompiled/<原目录>` 命名空间:整棵树是一等源码,重建成镜像,
结构上碰不到原资产。

还有一条:同一份插件源码在自维护源码引擎与预编译安装版引擎上**零 `#if` 分叉**。

## 文档

| | |
| :-- | :-- |
| **[快速上手](Docs/getting-started.md)** | 从零到跑起来,不开编辑器 |
| **[语法参考](Docs/language/README.md)** | `.dfs` / `.dfe` / `.dfm`、值、曲线、事件、stage |
| **[诊断码](Docs/diagnostics/README.md)** | 143 个 `DFXnnnn` 全带文件/行/列,从源码生成并防漂移 |
| **[编辑器工具](Docs/tools/editor-integration.md)** | 菜单、右键、工具栏、VSCode workspace |
| **[性能实测](Docs/performance-2026-08-08.md)** | 构建耗时的测量与结论 |

## 编辑器与工具链

| | |
| :-- | :-- |
| **编辑器集成** | *Tools* 菜单、Content Browser 右键(build / 反编译 / Adopt / 导出 `.dfe`)、Niagara 编辑器工具栏、关卡工具栏——全走同一条管线,`-NoDreamFXEditor` 一键关闭 |
| **file watcher** | 存盘即重建;打开生成资产即实时预览 |
| **`dfx.ps1`** | `build · verify · lint · decompile · decompile-all · mirror-diff · asset-diff · coverage · rename · schema · list · corpus` |
| **`ci.ps1`** | lint → build → verify → corpus 一条命令,关编辑器跑 |
| **溯源戳** | 每个生成资产带源 hash + 生成器版本 + 模块版本 GUID;`verify` 报手改、过期、版本漂移三种 |

## AI 支持

插件自带四个 agent 技能([`.skill/`](.skill/)):`dream-fx-create` / `dream-fx-verify` /
`dream-fx-diagnose` / `dream-fx-decompile`——编码代理可以全程 headless 地创作、构建、排错、迁移特效。

## 运行环境要求与能力边界

- **引擎**:Unreal Engine 5.8(源码构建与预编译安装版均支持,发布轮双引擎验证)。依赖引擎的
  Niagara 外部编辑 API(`UNiagaraExternalEditUtilities`,引擎侧标记 EXPERIMENTAL);
  API 漂移由启动自检显形,绝不静默吸收。
- **宿主项目必须启用与创作项目相同的内容插件**(如 `NiagaraFluids`):插件未挂载时探针没有基线,
  引用其模块的源会以缺口头 / 编译错的形式显形。
- **写包命令必须关编辑器**(`build` / `corpus` / `mirror-diff` / `decompile-all`)——
  两个进程存同一批包,谁后存谁赢,而且两边都不吭声。
- 未覆盖(设计使然或尚未做):Scratch Pad、模块内部图 lowering、GPU/CPU 条件分支、
  Scalability 条件逻辑、真·emitter 继承(`from` 是拷贝)。降级全部有诊断,绝不静默——
  1.0.0 已知问题清单见 [CHANGELOG](CHANGELOG.md)。
