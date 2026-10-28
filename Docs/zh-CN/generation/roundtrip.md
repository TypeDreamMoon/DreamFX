---
title: "往返与等价"
description: "反编译契约、缺口如何暴露、四个验证层各自回答什么，以及关于“全绿不等于正确”的三条教训。"
---

反编译不是一种便利导出。它是一份契约：

1. 任何 `UNiagaraSystem` 都会导出为 `.dfs`；
2. 该导出是**逐字节幂等**的——将导出的文本构建为资产，再次导出，得到
   相同的文本；
3. 语言无法表达的任何内容都会被**作为显式缺口写入文件头**，
   并报告为 `DFX8xxx`，绝不静默丢弃；
4. `Adopt` 在接管资产前会重新导出并比较，一旦不匹配就拒绝。

第 3 点最为关键。一个静默降级的导出器会让你在三个月后盯着一份看起来不对劲的
效果，却无从下手。

<a id="what-a-gap-looks-like"></a>

## 缺口长什么样

**[DFX8014](../diagnostics/DFX8xxx.md#dfx8014)：** 系统导出会保留原生 `inherits` 和
选定的父版本，但完整覆盖、事件/阶段快照以及不可用的父基线仍然存在限制。独立 `.dfe` 导出会扁平化该关系。有关确切契约，请参见
[原生发射器继承](../language/native-emitter-inheritance.md)。

**[DFX8015](../diagnostics/DFX8xxx.md#dfx8015)：** 发射器上的多个事件处理器无法被携带，导出会列出重建后的
发射器将不再接收哪些事件。

**[DFX8016](../diagnostics/DFX8xxx.md#dfx8016)：** 自定义 C++ 阶段类（不是引擎的通用阶段）的一个阶段没有 DreamFXLang 形式。

以上每一项都会出现在两个地方：导出文件的头部，以及日志中。

<a id="the-four-layers"></a>

## 四个层

| 层 | 命令 | 它回答的问题 |
| --- | --- | --- |
| **L1** | `mirror-diff` | 镜像的导出是否与原件**逐行**匹配？ |
| **L2** | `mirror-diff` | 镜像是否能**干净编译**？ |
| **L3** | 运行时等价夹具 | 重建后的系统**模拟**结果是否相同？固定步长 SimCache、逐帧粒子数 |
| **asset-diff** | `asset-diff` | 两者**作为资产**是否一致？通过反射遍历得到的事实，独立于导出器 |

`mirror-diff -NoCompile` 会跳过 L2——快得多，而且当镜像是在当前执行比较的同一会话中构建时，这是正确的做法。

<a id="why-the-fourth-layer-has-to-exist"></a>

### 为什么必须有第四层

在前三层中，L1 比较的是**同一导出器**的两份输出。导出器自身丢弃的东西会在两侧对称地
丢失：L1 逐字节相同，L2 仍然干净编译，而 L3——只统计粒子数——什么也看不到。但画面，
与此同时，是错的。

这不是假设。在一个真实案例中，L1 逐字节相同，L3 报告完全一致，而效果
看起来是错的；根因是一个派生位（`bBindingExistsOnSource`），它根本不出现在任何导出中。
因此 `asset-diff` 走了一条完全不同的路线：它**通过反射遍历两个资产的事实**，
包括编译器自身的视角（`compiled` 事实族），并且从不接触导出器。

<a id="the-corpus-compares-a-fifth-thing"></a>

### 语料库还比较第五样东西

除此之外，55 个用例的语料库会对比“按夹具构建的资产”与“按其导出重建的资产”。
这能捕获**对称丢失**：在文本比较的两侧被同样丢弃、因而对它不可见的
东西。

<a id="three-measurement-lessons"></a>

## 三条测量教训

> **警告：**
>
> **每次运行只测量一个系统。** L3 夹具每次执行只能测量一个系统；超过这个数量，
> 同一个资产就会被报告为不同的事物。

> **警告：**
>
> **一个没有确定性的随机系统不等于它自身。** 因此 L3 会先使用 A 对 A 自比较
> 来判断可判定性：一个与自身不一致的系统会被判定为不可判定，而不是
> 不同。

> **警告：**
>
> **静态全绿却出现行为分歧 ⇒ 测量导数。** 在出生速度恰好差一半的案例中，
> 根因是一个丢失的 `bFixedTickDelta`——Settings 表中少了一项条目。
> 行差异比较只报告第一处差异，因此修复一处就意味着要重新运行整个过程。

<a id="measuring-your-own-content"></a>

## 测量你自己的内容

```bash
# how much of my content can be represented? bucketed by feature
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 coverage

# export a whole content tree to source, in one editor boot
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 decompile-all -Path=/Game/VFX

# L1 + L2
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 mirror-diff -Path /Game/VFX

# asset facts, independent of the exporter; -DumpFacts writes both sides to Saved/DreamFX/*.facts
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 asset-diff -Path /Game/VFX -DumpFacts
```

> **注意：**
>
> `asset-diff` 在比较前会**强制编译两侧**。否则，`compiled` 事实
> 族报告的是各侧的编译历史而不是其内容，这就不是在比较
> 任何东西。`-NoCompile` 仅用于诊断。

<a id="the-mirror-namespace"></a>

## 镜像命名空间

导出会落入 `Decompiled/<original path>` 命名空间：整棵树都是一等源码，
会重建为**镜像**，并且结构上无法触及原件。展开说明见
[导出 vs 接管](adopt.md)。
