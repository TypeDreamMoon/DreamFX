---
title: "构建流水线"
description: "一次构建实际做了什么——发现、排序、schema 探测、写入、Niagara 编译、保存、盖戳，以及为什么未变更的源会被跳过。"
---

一次构建通过六个阶段将源转换为资产——而诊断代码的第一位数字就是抛出该诊断的
阶段：

| 阶段 | 做什么 | 代码 |
| --- | --- | --- |
| 1 | 驱动与文件 I/O | `DFX1xxx` |
| 2 | 词法与语法 | `DFX2xxx` |
| 3 | 声明与文档结构 | `DFX3xxx` |
| 4 | 值、类型与表达式 | `DFX4xxx` |
| 5 | 生成与资产写入 | `DFX5xxx` |
| 6 | Niagara 编译 | `DFX6xxx` |

还有两个族不在这条线上：`DFX7xxx` 是
[溯源、漂移与 lint](provenance.md)，而 `DFX8xxx` 是
[反编译器](roundtrip.md)。

<a id="order-modules--emitters--systems"></a>

## 顺序：模块 → 发射器 → 系统

`build -All`（以及编辑器中的 *Rebuild DFX*）按这个顺序构建，理由很具体：当一个
`.dfm` 和调用它的 `.dfs` 一起排队时，模块必须先存在，否则系统
无法解析它。

编辑器侧的观察器使用相同的顺序——它把更改过的文件盖戳进一个待处理队列，由
防抖 ticker 排空该队列。

<a id="one-files-journey"></a>

## 一个文件的旅程


<a id="parse"></a>

### 解析

词法分析器加解析器生成一份文档。位置信息（文件、行、列）在这里附加，并
一直传递到最后一条诊断，这就是为什么失败的构建可以弹出一个 **Open in VSCode** 链接，
直接定位到出错的列。

<a id="resolve-module-names-and-schemas"></a>

### 解析模块名与 schema

短模块名在 `ModulePaths`（L4）上解析。一旦名称解析成功，**输入签名是
探测出来的，而非假定的**：模块被添加到一个临时系统中，并读回它在
该栈中真正暴露的输入。

这个绕道是必要的——资产级 schema 完全会遗漏内联编辑条件和由静态开关揭示的
输入。`dfx schema <Module> -Stack <Stack>` 重现了这同一趟探测。

<a id="write"></a>

### 写入

系统、发射器、栈、模块和输入通过 Niagara 外部编辑 API 写入。
写入顺序是有意义的：静态开关必须先于它所揭示的输入存在之前被写入
（见 [`.dfs`](../language/dfs.md)）。

<a id="compile"></a>

### 编译

系统写入后即编译。编译不干净就是 `DFX6xxx`——包括
引擎自身的栈问题（未满足的依赖、未绑定的参数），DreamFX 将它们重述为
带位置的诊断。

<a id="save-and-stamp"></a>

### 保存与盖戳

`SavePackage` 写入包并在资产上盖戳溯源信息：源哈希、生成器
版本和模块版本 GUID。保存失败是
[DFX5030](../diagnostics/DFX5xxx.md#dfx5030)，一条构建错误——**不是**致命错误，所以“文件正在被
另一个程序使用”不再会连带拖垮编辑器。


<a id="unchanged-sources-are-skipped"></a>

## 未变更的源会被跳过

第二次构建一个未改动过的文件会报告 `0 built, 1 up to date`：资产上盖戳的
哈希与源的一致，所以无事可做。

| 目的 | 使用 |
| --- | --- |
| 无视哈希强制重建 | `-Force` |
| 构建但不写入包 | `-NoSave` |
| 只运行静态检查，不碰任何资产 | `lint` |
| 对照源检查资产，不写入任何内容 | `verify` |

<a id="building-in-the-editor-vs-on-the-command-line"></a>

## 在编辑器中构建 vs 在命令行中构建

一条流水线，两个入口点。只有一个差别，而且很硬性：

> **警告：**
>
> **不要在编辑器打开时运行写入包的命令。** 两个进程保存同一批
> 包会静默竞争，谁后保存谁赢。`build`、`corpus`、`mirror-diff` 和
> `decompile-all` 都会写入包。

还有一条同样硬性：**重建系统之前先 despawn 该系统的实例**——见
[日常循环](../getting-started/workflow.md)。

<a id="performance-why-a-full-tree-build-is-not-linear"></a>

## 性能：为什么全树构建不是线性的

在全树构建中，大部分时间既不是解析也不是写入——而是 **Niagara
编译**。在单个资产上测得，84% 的时间花在那里，每个资产大约触发四次系统编译。

存在若干可调节项，而默认值已经是快的那一侧。它们主要是为了 A/B 对比和
应急逃生：

| 开关 | 效果 |
| --- | --- |
| `-Window=<n>` | 流水线深度：编译请求与 finalize 之间可以有多少个系统。`1` 恢复完全串行的构建 |
| `-NoWriteScope` | 每次写入都重建编辑上下文（旧行为，慢得多） |
| `-RebuildOnStructural` | 每次结构写入后丢弃编辑上下文（旧行为） |
| `-RebuildOnSwitch` | 每次静态开关写入后丢弃编辑上下文（旧行为） |
| `-RebuildPerAdd` | 支付引擎的逐次添加栈刷新，而不是每个栈一次批量刷新 |

> **注意：**
>
> 这些不是日常用的开关。它们的价值在于**快路径和慢路径可以在同一台机器上、用同一个二进制
> 都跑一遍**——这样“那次优化到底有没有帮助”是被测量出来的，
> 而不是从代码里读出来的。

<a id="related"></a>

## 相关

  - **[溯源与 verify](provenance.md)** —— 戳里存有什么，`verify` 报告的三种漂移，以及四条 lint 规则。
  - **[往返与等价](roundtrip.md)** —— 反编译、缺口，以及四层验证。
  - **[命令行](../tools/headless-compilation.md)** —— 每个 `dfx.ps1` 子命令和开关。
