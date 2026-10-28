---
title: "你的第一个特效"
description: "从空目录到一个能运行的 NS_Hello——一个 .dfs、一条命令、一个标准 UNiagaraSystem。"
---

十五分钟，无需编辑器。


<a id="write-the-source"></a>

## 编写源文件

在项目的 `DFX/` 下创建 `Effects/NS_Hello.dfs`：

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
            // Advances NormalizedAge. Anything age-driven needs it, and Niagara reports an
            // unmet dependency if it is missing.
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

文件头部的 `Root="Game"` 指明了 `Name="..."` 相对于哪个内容根，因此该文件会构建到
`/Game/Effects/NS_Hello`。


<a id="build-it"></a>

## 构建

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

你应该会看到 `1 built, 0 up to date, 0 failed | 0 error(s)`，并且该资产会列在“本次运行写入磁盘的
资产”下。在编辑器中打开 `/Game/Effects/NS_Hello`，它就能播放。

不做任何修改再构建一次，会报告 `0 built, 1 up to date`——资产上的来源戳记
保存着源文件的哈希，哈希匹配就意味着无事可做。`-Force` 可以覆盖它。


<a id="change-one-number-and-build-again"></a>

## 改一个数字，再构建一次

把 `SpawnRate` 从 `20.0` 改为 `60.0`，然后再次构建。这一次它会重新构建——哈希变了。

这就是整个循环：**文本是唯一的创作界面，而资产是构建产物。**
不要在 Niagara 编辑器中编辑生成的资产；下一次重新构建会抹掉改动，而且不会
事先询问。


<a id="when-it-does-not-compile"></a>

## 编译不过的时候

每条诊断都是一个 `DFXnnnn`，带有文件、行和列：

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

有两样东西值得先伸手去拿：

- **[诊断](../diagnostics/overview.md)**——每一条诊断码，以及它的成因和修复方法；
- **`dfx.ps1 schema <Module>`**——从资产读取出的模块*真实*输入签名。这是
  名字的权威：Niagara 输入名包含空格（`Loop Duration`），DreamFX
  会将其规范化，而有些输入只有在它们上方的静态开关被设置后才存在。

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 schema GravityForce
```

> **注意：**
>
> 省略 `ParticleState()` 不是语法错误——它表现为 Niagara 自身未满足的
> 依赖。它推进 `NormalizedAge`，而每个由年龄驱动的模块都会读取它。

<a id="things-to-try-next"></a>

## 接下来可以尝试

- 把 `VelocityStrength` 换成一个随机范围，看看嵌套动态输入是怎么写的：

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

  `RandomnessMode` 是一个静态开关。在**模块**上，开关必须写在它所控制的输入
  之前；而在**动态输入**上，这条规则被放宽了——开关会被提前——所以上面的顺序
  无论哪种方式都成立。参见[值与规则](../language/values.md)。

- 添加一个由曲线驱动的颜色淡出：

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

  - **[布局与命名](project-layout.md)**——`DFX/` 根、`Root` 和 `Name` 如何解析、模块搜索路径。
  - **[`.dfs` 参考](../language/dfs.md)**——Settings、Properties、六个栈、渲染器、`from`、`Defaults`。
