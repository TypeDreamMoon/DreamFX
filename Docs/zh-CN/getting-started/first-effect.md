---
title: 第一个特效
description: 从空目录到一个能跑的 NS_Hello：写一个 .dfs，跑一条命令，得到一个标准 UNiagaraSystem。
---

十五分钟，不需要打开编辑器。

<a id="建源文件"></a>

## 建源文件

在项目的 `DFX/` 下建 `Effects/NS_Hello.dfs`：

```cpp
System(Name="Effects/NS_Hello", Root="Game")
{
    Settings = {
        WarmupTime  = 0.0;
        FixedBounds = box(-100, -100, -100, 100, 100, 100);
    }

    // 暴露给蓝图的 User.Speed，可以用 SetNiagaraVariableFloat 设。
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
            // 推进 NormalizedAge。任何按寿命驱动的东西都需要它，
            // 缺了 Niagara 会报未满足的依赖。
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

文件头的 `Root="Game"` 说 `Name="..."` 相对哪个内容根，所以这个文件生成
`/Game/Effects/NS_Hello`。

<a id="构建"></a>

## 构建

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

应该看到 `1 built, 0 up to date, 0 failed | 0 error(s)`，以及 "Assets written to disk by this run"
里列出的那个资产。在编辑器里打开 `/Game/Effects/NS_Hello`，它就在播。

再跑一次、什么都没改，会报 `0 built, 1 up to date` —— 资产上的溯源戳里存着源码的 hash，
hash 对上就没事可做。`-Force` 越过它。

<a id="改一个数再建一次"></a>

## 改一个数，再建一次

把 `SpawnRate` 从 `20.0` 改成 `60.0`，再跑一次 build。这次它会重建 —— hash 变了。

这就是整个循环：**文本是唯一的编辑面，资产是构建产物**。不要在 Niagara 编辑器里改生成出来的资产，
下一次重建会把改动铲掉，而且不会问你。

<a id="编译不过的时候"></a>

## 编译不过的时候

每条诊断都是一个带文件、行、列的 `DFXnnnn`：

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

两个东西值得先伸手去拿：

- **[诊断码](../diagnostics/overview.md)** —— 每一条码的成因与修法；
- **`dfx.ps1 schema <Module>`** —— 从资产上读出来的模块**真实**输入签名。这是名字的权威：
  Niagara 的输入名带空格（`Loop Duration`），DreamFX 会归一化；而且有些输入要等上面的静态开关
  取到某个值才存在。

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce
```

**提示：**

`ParticleState()` 缺了，效果不会报语法错，而是 Niagara 报一条未满足的依赖 ——
  它推进 `NormalizedAge`，所有按寿命驱动的模块都要它。

<a id="接下来试试"></a>

## 接下来试试

- 把 `VelocityStrength` 换成一个随机范围，看看嵌套 dynamic input 怎么写：

  ```cpp
  AddVelocityInCone(
      ConeAngle        = 30.0,
      VelocityStrength = RandomRangeFloat(
          Minimum        = User.Speed * 0.5,
          Maximum        = User.Speed,
          RandomnessMode = SimulationDefaults
      )
  );
  ```

  注意 `RandomnessMode` 是个静态开关。在**模块**上开关必须写在它所控制的输入之前，
  在**dynamic input** 上这条规则被放宽了 —— 开关会被提前 —— 所以上面这样写就行。
  见[值与规则](../language/values.md)。

- 加一条按寿命褪色的颜色曲线：

  ```cpp
  ParticleUpdate = {
      ParticleState();
      ScaleColor(
          ScaleAlpha = FloatFromCurve(
              FloatCurve = curve { 0.0 -> 1.0; 1.0 -> 0.0; },
              CurveIndex = Particles.NormalizedAge
          )
      );
  }
  ```

- [目录与命名](project-layout.md) — `DFX/` 根、`Root` / `Name` 怎么解析、模块搜索路径。
  - [`.dfs` 参考](../language/dfs.md) — Settings、Properties、六个栈、renderer、`from`、`Defaults`。
