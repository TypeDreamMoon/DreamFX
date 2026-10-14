---
title: 事件与 Simulation Stage
description: OnEvent 事件处理器与具名 Stage 块：怎么写、各自的硬限制，以及 DataInterface 名字为什么就是全部语义。
---

`.dfs` 的六个固定栈之外，一个 emitter 还可以带一个**事件处理器**和任意多个 **simulation stage**。
两者都是普通的栈：模块调用、折叠赋值、`disabled`，一样都不少。

<a id="onevent--响应另一个-emitter-的事件"></a>

## `OnEvent` —— 响应另一个 emitter 的事件

```cpp
Emitter Receiver
{
    ParticleSpawn = { … }

    OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = {
        ReceiveLocationEvent();        // 普通栈语句；每收到一个事件跑一次
    }
}
```

头部配置这个处理器，块本身就是一个栈。

| 参数 | 含义 |
| --- | --- |
| `Source` | 本系统里生成事件的那个 emitter。**必填** |
| `Event` | 它生成的事件名（`GenerateLocationEvent()` 发的是 `"LocationEvent"`）。**必填** |
| `Mode` | `SpawnedParticles` 每个事件生成 `SpawnNumber` 个新粒子并在它们身上跑这个块；`EveryParticle` 在已有粒子身上跑 |
| `SpawnNumber` / `RandomSpawnNumber` / `MinSpawnNumber` | 生成数量的控制 |
| `MaxEventsPerFrame` | 每帧最多处理多少个事件 |
| `UpdateAttributeInitialValues` | 是否更新属性初值 |

只写与默认值不同的那几个。`Source` 或 `Event` 缺失是
[DFX2025](../diagnostics/DFX2xxx.md#dfx2025) 家族的参数错误。

**警告：**

**一个 emitter 只能有一个 `OnEvent` 块**（[DFX5031](../diagnostics/DFX5xxx.md#dfx5031)）。
  DreamFX 通过和另外四个栈同一套轨道去寻址事件栈，而那套轨道只能够到恰好一个处理器。
  要响应两条事件流的 emitter，拆成两个 emitter。

事件的**生成方**通常需要在源 emitter 的 `Settings` 里写 `RequiresPersistentIDs = true`，
因为它读 `Particles.ID`；缺了这条，构建会报引擎自己的栈问题
（[DFX6003](../diagnostics/DFX6xxx.md#dfx6003)）。

<a id="stage--simulation-stage"></a>

## `Stage` —— simulation stage

```cpp
Emitter Fluid
{
    Settings = { SimTarget = GPU; }

    ParticleUpdate = { … }

    Stage Settle = {                     // 裸 stage：启用、按粒子迭代、跑一次
        ScaleColor();
    }

    Stage Project(DataInterface = "Emitter.PressureGrid", NumIterations = 10) = {
        …                                // 普通栈语句；按网格单元跑，跑十次
    }
}
```

stage 是跑在 `ParticleUpdate` **之后**的粒子栈，只在 GPU emitter 上有效
（`SimTarget = GPU`；CPU emitter 写 `Stage` 是
[DFX5033](../diagnostics/DFX5xxx.md#dfx5033)）。**声明顺序就是执行顺序。**

| 头部参数 | 含义 |
| --- | --- |
| `DataInterface` | stage 迭代的那个网格（一个线程一个 cell，而不是一个粒子），并已经隐含了迭代源 |
| `Iteration` | 显式说明迭代源：`Particles`、`DataInterface`、`DirectSet` —— 用于需要说清的形态，比如绑定为空的数据接口迭代 |
| `NumIterations` | 重复次数（默认 1） |
| `Enabled` | `false` 把它停在那里 |
| `ExecuteBehavior` | 执行时机（如 `Always`、`OnSimulationReset`） |

只写与「刚加上去的 stage」不同的那几个。参数名不认识或形状不对是
[DFX2026](../diagnostics/DFX2xxx.md#dfx2026)；两个 `Stage` 同名是
[DFX5032](../diagnostics/DFX5xxx.md#dfx5032) —— stage 按名字标识，否则构建会安静地留下最后跑的那个栈。

<a id="datainterface-是个名字而名字就是全部语义"></a>

### `DataInterface` 是个名字，而名字就是全部语义

引擎在编译期**按名字**解析迭代网格，**从不读绑定里存着的类型**。这不是猜测：创作出来的资产带着的
类型句柄在后来的会话里会解析成垃圾，而它们照样正常模拟。

网格本身通常是由某个模块内部的写入物化出来的 —— 它不需要是一个声明过的参数，绑定就能成立；
名字拼错会以「这个 stage 对着一个不存在的网格编译」的形式显形。

<a id="numiterations-与-enabled值或参数"></a>

### `NumIterations` 与 `Enabled`：值**或**参数

值的位置决定是哪一种：数字、`true`、`false` 是字面量；任何读起来像名字的东西是绑定。

```cpp
Stage Project(NumIterations = 6, NumIterations = Emitter.OVERRIDE.SolveIterations) = { … }
Stage DebugSlice(Enabled = Grid3D_GAS_CONTROLS_SPAWN.RenderDebugSlice) = { … }
```

两者可以同时出现（如上），因为引擎两个都存：数字是绑定覆盖不到时的兜底。

**警告：**

这件事值得照写，而不是简化掉。`Enabled` 被绑定的 stage 是在特效说了算的时候才跑；
  只保留字面量标志的重建**永远**跑它 —— Ninja 的 debug slice 就是这样在每一个镜像里
  画到了流体上面。

<a id="缺口"></a>

## 缺口

**往返缺口 — [`DFX8016`](../diagnostics/DFX8xxx.md#dfx8016)**

自定义 C++ stage 类（任何不是引擎那个通用 stage 的东西）表达不了，反编译时作为缺口逐条写进文件头。

**往返缺口 — [`DFX8015`](../diagnostics/DFX8xxx.md#dfx8015)**

一个 emitter 上超过一个的事件处理器同样带不走，导出会点名说明重建出来的 emitter 收不到哪些事件。

两者都属于「显形而不是静默丢弃」的那一类：见[往返与等价](../generation/roundtrip.md)。

<a id="都是被验过的"></a>

## 都是被验过的

事件与 stage 都做过全库往返：全库 27 个真 stage 逐个走过 corpus、mirror-diff 与
asset-diff 的 stage 事实比对，`DFX8016` 归零。零 usage-id 的栈在读写两侧都要特殊对待，
细节属于实现，但结论对使用者很直接 —— **这两族不是「大概能用」，是量过的**。
