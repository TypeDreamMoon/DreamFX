---
title: "事件与 simulation stage"
description: "OnEvent 处理器与具名 Stage 块——它们怎么写、各自的硬性上限，以及为什么一个 DataInterface 名字就是全部契约。"
---

在六个固定栈之外，一个 emitter 还可以携带一个**事件处理器**和任意数量的
**simulation stage**。两者都是普通的栈：模块调用、折叠赋值、`disabled`，一样都不少。

<a id="onevent--reacting-to-another-emitters-events"></a>

## `OnEvent` —— 响应另一个 emitter 的事件

```cpp
Emitter Receiver
{
    ParticleSpawn = { … }

    OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 2) = {
        ReceiveLocationEvent();        // ordinary stack statements; runs per received event
    }
}
```

头部配置这个处理器；块本身就是一个和别处一样的栈。

| 参数 | 含义 |
| --- | --- |
| `Source` | 本系统里生成该事件的那个 emitter。**必填** |
| `Event` | 它生成的事件（一个 `GenerateLocationEvent()` 模块发出的就是 `"LocationEvent"`）。**必填** |
| `Mode` | `SpawnedParticles` 每个事件生成 `SpawnNumber` 个新粒子并在它们身上运行这个块；`EveryParticle` 在已有粒子上运行它 |
| `SpawnNumber` / `RandomSpawnNumber` / `MinSpawnNumber` | 生成多少个 |
| `MaxEventsPerFrame` | 每帧处理事件的上限 |
| `UpdateAttributeInitialValues` | 是否更新初值 |

只写与默认值不同的那几项。缺少 `Source` 或 `Event`，或者出现未知参数，都是
[DFX2025](../diagnostics/DFX2xxx.md#dfx2025)。

导出也可以使用反引号包裹的源令牌：`` `<none>` `` 表示处理器读取自己的
emitter，`` `<unresolved GUID>` `` 表示一个过期的源标识符。两者都按原样重建；
无法解析的源会报告 `DFX5036`，因为这个处理器永远不会触发。完整的
事件源契约见[系统参考](dfs.md)。

> **警告：**
>
> **每个 emitter 只能有一个 `OnEvent` 块**（[DFX5031](../diagnostics/DFX5xxx.md#dfx5031)）。DreamFX
> 通过和另外四个栈同一套轨道去寻址事件栈，而那套轨道只能到达恰好一个
> 处理器。要响应两条事件流的 emitter，拆成两个 emitter。

事件的*生成方*通常需要在源 emitter 的 `Settings` 里设置 `RequiresPersistentIDs = true`，
因为它会读取 `Particles.ID`；缺了它，构建会报告引擎自身的栈问题
（[DFX6003](../diagnostics/DFX6xxx.md#dfx6003)）。

<a id="stage--simulation-stages"></a>

## `Stage` —— simulation stage

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

stage 是运行在 `ParticleUpdate` **之后**的粒子栈，作用于 GPU emitter
（`SimTarget = GPU`；带 `Stage` 块的 CPU emitter 属于
[DFX5033](../diagnostics/DFX5xxx.md#dfx5033)）。**声明顺序就是执行顺序。**

| 头部参数 | 含义 |
| --- | --- |
| `DataInterface` | stage 迭代的那个网格（一个线程一个 cell，而不是一个粒子一个线程）；它已经隐含了迭代源 |
| `Iteration` | 显式声明的迭代源——`Particles`、`DataInterface`、`DirectSet`——用于需要它的形态，比如没有任何绑定的数据接口迭代 |
| `NumIterations` | 重复该 stage（默认 1） |
| `Enabled` | `false` 把它停在那里 |
| `ExecuteBehavior` | 什么时候运行（例如 `Always`、`OnSimulationReset`） |

这些是常见的头部字段。通用 stage 还额外支持 direct-dispatch 尺寸设定、
线程数覆盖以及粒子迭代状态过滤；完整的参数表见
[系统参考](dfs.md)。

只写与刚添加的 stage 不同的那些。参数不认识或形状不对是
[DFX2026](../diagnostics/DFX2xxx.md#dfx2026)；两个同名的 `Stage` 块是
[DFX5032](../diagnostics/DFX5xxx.md#dfx5032) —— stage 按名字标识，否则构建会
悄悄留下最后运行的那个栈。

<a id="datainterface-is-a-name-and-the-name-is-the-whole-contract"></a>

### `DataInterface` 是个名字，而这个名字就是全部契约

引擎在编译期**按名字**解析迭代网格，并且**从不读取绑定里存着的类型**。这不是猜测：
创作出来的资产带着的类型句柄在后来的会话中会解析成垃圾，而它们照样
正常模拟。

网格本身通常是由某个模块的内部写入物化出来的——绑定要成立并不需要它是一个
声明过的参数，而名字拼错会表现为这个 stage 对着一个并不存在的网格编译。

<a id="numiterations-and-enabled-take-a-value-or-a-parameter"></a>

### `NumIterations` 与 `Enabled` 接受一个值**或**一个参数

值所在的位置决定是哪一种：数字、`true` 或 `false` 是字面量，任何读起来
像名字的东西是绑定。

```cpp
Stage Project(NumIterations = 6, NumIterations = Emitter.OVERRIDE.SolveIterations) = { … }
Stage DebugSlice(Enabled = Grid3D_GAS_CONTROLS_SPAWN.RenderDebugSlice) = { … }
```

两者可以同时出现，如上所示，因为引擎两个都存：数字是绑定覆盖不到时的兜底。

> **警告：**
>
> 这件事值得照写，而不是简化掉。`Enabled` 被绑定的 stage 在特效说了算的时候才运行，
> 而只保留字面量标志的重建会**永远**运行它——Ninja 的 debug slice 就是这样在每一个镜像里
> 画到了流体上面。

<a id="gaps"></a>

## 缺口

**[DFX8016](../diagnostics/DFX8xxx.md#dfx8016)：** 自定义 C++ stage 类的 stage——任何不是引擎通用 stage 的东西——没有
DreamFXLang 形式，会被作为缺口写进导出文件的头部。

**[DFX8015](../diagnostics/DFX8xxx.md#dfx8015)：** 一个 emitter 上超过一个事件处理器也无法携带；导出会点名重建后的
emitter 将不再接收哪些事件。

两者都属于「声明过就绝不丢弃」的契约——见
[往返与等价](../generation/roundtrip.md)。

<a id="both-families-are-measured"></a>

## 两族都是量过的

事件与 stage 都做过全库往返：本项目全部 27 个真实 stage 都走过了 corpus、`mirror-diff` 以及
`asset-diff` 的 stage 事实族，而 `DFX8016` 为零。usage id 为零的
栈在读取和写入两侧都需要特殊处理——那部分属于实现——但对使用者来说结论很直接：**这两族
不是「大概没问题」，而是量过的。**
