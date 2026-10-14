---
title: "能力边界"
description: "有意不做的、还没做的、往返带不走的，以及 1.0.0 的已知问题清单 —— 附带它们各自会怎么显形。"
---

这一页把「DreamFX 不做什么」写清楚。原则只有一条：**每一处降级都有诊断，绝不静默。**
所以下面每一条都配着它显形的方式。

<a id="deliberate"></a>

## 有意不做

| | 为什么 |
| --- | --- |
| **Scratch Pad** | scratch pad 脚本是内嵌在资产里的图，没有稳定名字可以寻址 |
| **把模块 body 铺成节点图** | `.dfm` 的 body 落成一个 custom HLSL 节点。铺开是「二级」，那是 DreamShader 已经背着的约 1.3 万行的问题再来一遍 |
| **通用表达式编译器** | 内联表达式只有算术和一张短白名单（[DFX4031](DFX4xxx.md#dfx4031)）。要更多请写 `hlsl { }` 或 `.dfm` |
| **通过 `from` 的隐式原生继承** | `from` 在重建时拷贝并合并 `.dfe` 源码。原生资产继承使用 [`inherits`](../language/native-emitter-inheritance.md) 显式声明 |
| **`MaterialParam`** | 保留语法，未实现（[DFX5093](DFX5xxx.md#dfx5093)） |
| **`#Region` 到达资产** | 外部编辑 API 没有给栈加注记的函数（L5） |

`Group` 和 `SortPriority` 填充资产的 user 参数层级。前者那条
[DFX5099 限制](history/DFX5099.md#dfx5099)仍为旧链接保留记录，但在已发布的实现里
不再是限制。见[系统参考](../language/dfs.md)。

<a id="not-done-yet"></a>

## 还没做

| | 现在会怎样 |
| --- | --- |
| GPU/CPU 条件分支 | 不表达 |
| Scalability 条件逻辑 | 不表达（emitter 的 `QualityLevelMask` **是**支持的） |
| `.dfm` 静态开关的编译期折叠 | 接受、校验，然后按普通输入 lower（[DFX5102](DFX5xxx.md#dfx5102)） |
| 曲线数据接口的完整可读语法 | 键之外还有配置时退回逐字 JSON |

通用 stage 也支持派发尺寸和粒子迭代过滤器。它们支持的参数列在
[系统参考](../language/dfs.md)；自定义 C++ stage 类仍是一个缺口。

<a id="what-the-round-trip-cannot-carry"></a>

## 往返带不走的

**[DFX8014](DFX8xxx.md#dfx8014)：** **原生 emitter 继承的限制。** 系统导出会保留
`inherits` 并锁定父版本，对完整栈/renderer 覆盖、事件/stage 快照以及不可用的父基线
都有已记录的限制。独立 `.dfe` 导出仍会摊平父关系。见[原生 emitter 继承](../language/native-emitter-inheritance.md)。

**[DFX8015](DFX8xxx.md#dfx8015)：** **一个 emitter 上的第二个事件处理器。** 一个可表达；多的不行。独立 `.dfe`
导出也带不走处理器的 `Source`，因为那里没有兄弟 emitter 可以命名。

**[DFX8016](DFX8xxx.md#dfx8016)：** **自定义 C++ stage 类。** 普通 stage 导出成 `Stage name(...)`；类不是
引擎通用 stage 的带不走。

还有两类不报码但值得知道：

- **renderer 列表元素的其余字段。** `Meshes` 写成路径数组，而每个
  元素其实是个结构体 —— 网格的逐元素轴心、缩放、LOD 范围没有语法。会因此丢东西的导出
  把它写进文件头。
- **内联表达式不按原样回来。** 它们以等价的 `hlsl { }` 块回来。**往返是语义上的，不是文本上的。**

<a id="100-known-issues"></a>

## 1.0.0 已知问题

1. 嵌套 dynamic input 节点上**深度 > 1** 的链接型宣言开关未防护 —— 引擎缺陷可达，但语料零命中；
   计划在 1.0.x 把守卫扩到该深度。
2. **emitter 级 `FixedBounds` 未携带**（系统级已携带）。
3. `bGpuAlwaysRunParticleUpdateScript` 未携带。
4. 数据接口配置以原样 JSON 承载（可读语法挂起）；裸 `DI<T>` 加链接的形态下，
   残留默认实例的计数不稳 —— 那是判据噪音，不是数据丢失。
5. UE 5.8.1 的 refPath 导入回归 —— 引擎 bug，已用
   `NormalizeObjectReferences` 绕过，5.8.2 复测后拆除。
6. **L4 画面对比是人工可选步骤**（OBS 配方已验证，未脚本化）。
7. 长尾未做栏：自定义 C++ stage 类、参数驱动迭代次数等 —— 缺口头会点名，语料里没有实例。

<a id="these-are-not-defects--they-are-requirements"></a>

## 这些不是缺陷，是环境要求

> **警告：**
>
> **宿主项目必须启用与创作项目相同的内容插件**（如
> `NiagaraFluids`）。插件没挂载时模块探针没有基线，引用其模块的源码会以缺口或编译错
> 的形式显形。**一整族解析不了的名字共享同一个前缀 = 挂载点没挂，不是拼写错。**

> **警告：**
>
> **写包的命令要关编辑器**（`build`、`corpus`、`mirror-diff`、
> `decompile-all`）。两个进程存同一批包会静默竞争。

> **警告：**
>
> **重建一个系统之前先让它的实例消失。** 热替换期间活着的 `NiagaraActor` 可能
> 看到建到一半的 renderer 布局，然后在渲染 worker 上断言。这是引擎侧的竞态，DreamFX 挡不住。

<a id="measure-your-own-coverage"></a>

## 自己量自己的覆盖率

别人的数字回答不了你的问题：

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage
```

它按特性分桶报告一整棵内容树里有多少能被表达。展开见
[往返与等价](../generation/roundtrip.md)。
