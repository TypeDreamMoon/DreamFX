---
title: "诊断"
description: "每个 DFXnnnn 代码 —— 如何阅读一个代码、每个系列属于哪个流水线阶段，以及指向生成的代码目录的链接。"
---

每个 DreamFX 诊断都是一个 `DFXnnnn`，带有**文件、行和列**：

```text
DFX/Effects/NS_Hello.dfs(31,17): error DFX3003: Module 'GravityForce' has no input named 'Gravty'.
Did you mean 'Gravity'? Available inputs: Gravity, CoordinateSpace
```

| | |
| --- | --- |
| 日志类别 | `LogDreamFX` |
| 严重级别 | `error` / `warning` / `info` —— 三者都确实会被使用 |
| 位置 | 每个代码都带有文件、行和列 |
| 在编辑器中 | 构建失败时会弹出提示，并带有**在 VSCode 中打开**链接，直接跳到对应的行和列 |

**首位数字是抛出该诊断的阶段。** 这既是阅读线索，也指向了应当查看的位置：

| 范围 | 阶段 | 含义 |
| --- | --- | --- |
| [`DFX1xxx`](DFX1xxx.md) | 驱动器与文件 I/O | 文件无法读取，或词法分析器停止 |
| [`DFX2xxx`](DFX2xxx.md) | 词法与语法 | 语法：大括号、分号、关键字 |
| [`DFX3xxx`](DFX3xxx.md) | 声明与结构 | 语法没问题，但某个声明与真实资产不一致 |
| [`DFX4xxx`](DFX4xxx.md) | 值、类型与表达式 | `=` 右侧的某个内容 |
| [`DFX5xxx`](DFX5xxx.md) | 生成与资产写入 | 写入资产失败，或正在宣告一次有意降级 |
| [`DFX6xxx`](DFX6xxx.md) | Niagara 编译 | 资产已写入，而 Niagara 本身提出异议 |
| [`DFX7xxx`](DFX7xxx.md) | 溯源、漂移与 lint | 资产与源不同步，或 lint 发现隐患 |
| [`DFX8xxx`](DFX8xxx.md) | 反编译器 | 导出、Adopt、往返缺口 |

> **注意：**
>
> 这些页面中有一半是**机器写的**：严重级别、消息模板和抛出位置从插件源中扫描出来，
> 而 `gen-diagnostics.ps1 -Check` 是阻止它们漂移的门。人写的那一半 —— 成因与修法 —— 只写一次。
> 见 [ci.ps1 与门](../tools/ci.md)。

<a id="the-order-to-debug-in"></a>

## 排错的顺序


<a id="look-the-code-up"></a>

### 查该代码
使用[生成的代码索引](README.md)或下面的某个系列页面。代码是稳定的：`DFX3003` 永远是同一个失败模式。

<a id="check-against-the-real-signature"></a>

### 对照真实签名核对
`dfx.ps1 schema <Module> -Stack <Stack>` 会打印构建所看到的输入签名 ——
包括静态开关。关于名称，权威在资产，而不在记忆。

<a id="still-wrong-read-the-limits"></a>

### 还是不对？阅读限制
[限制](limitations.md)列出了未覆盖的内容以及 1.0.0 已知问题。


<a id="lookup-by-code"></a>

## 按代码查找

[完整代码索引](README.md)由当前插件源生成。它拥有
严重级别与消息表；本指南链接到该目录，而不是维护第二份副本。

<a id="dfx1xxx--driver-and-file-io"></a>

### DFX1xxx —— 驱动器与文件 I/O

阅读 [DFX1xxx 参考](DFX1xxx.md)，了解当前消息、成因与修法。

<a id="dfx2xxx--lexer-and-syntax"></a>

### DFX2xxx —— 词法与语法

阅读 [DFX2xxx 参考](DFX2xxx.md)，了解当前消息、成因与修法。

<a id="dfx3xxx--declarations-and-document-structure"></a>

### DFX3xxx —— 声明与文档结构

阅读 [DFX3xxx 参考](DFX3xxx.md)，了解当前消息、成因与修法。

指向 [DFX3045](history/DFX3045.md#dfx3045) 的旧链接仍可作为历史条目使用。

<a id="dfx4xxx--values-types-and-expressions"></a>

### DFX4xxx —— 值、类型与表达式

阅读 [DFX4xxx 参考](DFX4xxx.md)，了解当前消息、成因与修法。

<a id="dfx5xxx--generation-and-asset-writing"></a>

### DFX5xxx —— 生成与资产写入

阅读 [DFX5xxx 参考](DFX5xxx.md)，了解当前消息、成因与修法。

指向 [DFX5099](history/DFX5099.md#dfx5099) 的旧链接解释了以前的编组限制。

<a id="dfx6xxx--niagara-compilation"></a>

### DFX6xxx —— Niagara 编译

阅读 [DFX6xxx 参考](DFX6xxx.md)，了解当前消息、成因与修法。

<a id="dfx7xxx--provenance-drift-and-lint"></a>

### DFX7xxx —— 溯源、漂移与 lint

阅读 [DFX7xxx 参考](DFX7xxx.md)，了解当前消息、成因与修法。

<a id="dfx8xxx--decompiler"></a>

### DFX8xxx —— 反编译器

阅读 [DFX8xxx 参考](DFX8xxx.md)，了解当前消息、成因与修法。
