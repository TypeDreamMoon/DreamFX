---
title: DFX2xxx — 词法与语法
description: DFX2xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 22 条（21 错误 / 1 警告）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx2000"></a>

## DFX2000

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown document type '%s'. Expected System, Emitter, Module or DynamicInput.
```

**触发位置：** `Parser/DreamFXParser.cpp:1740`

**原因.** 文件的第一个词不是四种文档类型之一。

**修复.** 写 `System`、`Emitter`、`Module` 或 `DynamicInput`，并与文件扩展名一致。

<a id="dfx2001"></a>

## DFX2001

**严重级别：** 错误 (`error`)

**消息：**

```text
Expected '{' to open a raw block but found '%s'.
```

**触发位置：** `Parser/DreamFXLexer.cpp:399, Parser/DreamFXParser.cpp:87`

**原因.** 一个需要 raw block 的构造后面没有跟 `{`。

**修复.** 补上花括号。`hlsl` 和 `Body` 永远是 `hlsl { ... }` / `Body = { ... }`。

<a id="dfx2002"></a>

## DFX2002

**严重级别：** 错误 (`error`)

**消息：**

```text
Expected a name but found %s.
```

**触发位置：** `Parser/DreamFXParser.cpp:98`

**原因.** 这里期待一个标识符 —— 模块名、输入名、emitter 名。

**修复.** 检查报出位置之前有没有多余的标点。

<a id="dfx2003"></a>

## DFX2003

**严重级别：** 错误 (`error`)

**消息：**

```text
Expected a number but found '%s'.
```

**触发位置：** `Parser/DreamFXParser.cpp:394`

**原因.** 这里期待一个数字：向量字面量里、`box()` 里，或者曲线的键上。

**修复.** 向量分量和曲线时间只能是数字，参数引用不能出现在那里。

<a id="dfx2004"></a>

## DFX2004

**严重级别：** 错误 (`error`)

**消息：**

```text
Expected a value but found %s.
```

**触发位置：** `Parser/DreamFXParser.cpp:581`

**原因.** `=` 后面期待一个值。

**修复.** 要么右边少了东西，要么上一条语句少了 `;`。

<a id="dfx2005"></a>

## DFX2005

**严重级别：** 错误 (`error`)

**消息：**

```text
'#EndRegion' has no matching '#Region'.
```

**触发位置：** `Parser/DreamFXParser.cpp:789`

**原因.** 出现了 `#EndRegion`，但没有打开着的 `#Region`。

**修复.** 删掉它，或者补上开头的 `#Region "..."`。

<a id="dfx2006"></a>

## DFX2006

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown directive '#%s'. Expected Region or EndRegion.
```

**触发位置：** `Parser/DreamFXParser.cpp:799`

**原因.** 指令只有 `#Region` 和 `#EndRegion` 两个。

**修复.** 检查拼写。`#Region` 是一种注释装置（L5），从不到达资产。

<a id="dfx2007"></a>

## DFX2007

**严重级别：** 错误 (`error`)

**消息：**

```text
Expected a version after '@'.
```

**触发位置：** `Parser/DreamFXParser.cpp:460, Parser/DreamFXParser.cpp:481, Parser/DreamFXParser.cpp:859`

**原因.** `@` 引入一个 R7 版本固定，后面必须跟版本号。

**修复.** 写 `ModuleName@1.2`，或者把 `@` 去掉。

<a id="dfx2008"></a>

## DFX2008

**严重级别：** 错误 (`error`)

**消息：**

```text
Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'.
```

**触发位置：** `Parser/DreamFXParser.cpp:878, Generation/DreamFXGenerator.cpp:973`

**原因.** 模块调用被喂了一个裸值。Niagara 按名字寻址模块输入，而它们在栈 UI 里出现的顺序并不是声明顺序。

**修复.** 每个实参都写成 `Name = Value`。`dfx schema <Module>` 会列出名字。

<a id="dfx2010"></a>

## DFX2010

**严重级别：** 警告 (`warning`)

**消息：**

```text
'#Region \"%s\"' was never closed with '#EndRegion'.
```

**触发位置：** `Parser/DreamFXParser.cpp:963`

**原因.** 一个 `#Region` 到块结束都没等到 `#EndRegion`。无害 —— region 只是文本 —— 但通常意味着某一段被搬走了，闭合行留在了原地。

**修复.** 补上 `#EndRegion`。

<a id="dfx2013"></a>

## DFX2013

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is a system-scope stack and must be written at the top level of a .dfs, not inside an emitter.
```

**触发位置：** `Parser/DreamFXParser.cpp:1153`

**原因.** `SystemSpawn` 和 `SystemUpdate` 属于系统，不属于 emitter（L1）。

**修复.** 把这个块挪到 `.dfs` 的顶层，和 `Emitter` 块并列。

<a id="dfx2015"></a>

## DFX2015

**严重级别：** 错误 (`error`)

**消息：**

```text
'from' must be followed by a quoted path to a .dfe source file.
```

**触发位置：** `Parser/DreamFXParser.cpp:1483`

**原因.** `from` 后面要跟一个带引号的 `.dfe` 路径。

**修复.** 加引号。扩展名可省；路径先相对本文件解析。

<a id="dfx2016"></a>

## DFX2016

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Parser/DreamFXParser.cpp:1097`

**原因.** `Defaults = { … }` 块里出现了赋值以外的东西。默认值说的是「没人写过它时读出什么」，模块调用在那里没有任何含义。

**修复.** 把模块调用挪进某个栈（`ParticleSpawn`、`ParticleUpdate`…）。`Defaults` 里只能写 `<type> Namespace.Name = value;`。

<a id="dfx2018"></a>

## DFX2018

**严重级别：** 错误 (`error`)

**消息：**

```text
A Module or DynamicInput must declare a 'Body = { }' block.
```

**触发位置：** `Parser/DreamFXParser.cpp:1650`

**原因.** 一个没有 `Body` 的 `.dfm` 只声明了输入，生成出来的模块什么都不做。

**修复.** 加上 `Body = { ... }`。

<a id="dfx2019"></a>

## DFX2019

**严重级别：** 错误 (`error`)

**消息：**

```text
Header argument '%s' must be a quoted string.
```

**触发位置：** `Parser/DreamFXParser.cpp:1677, Parser/DreamFXParser.cpp:1694, Parser/DreamFXParser.cpp:1702`

**原因.** 文件头的参数（`Name=`、`Root=`）是带引号的字符串。

**修复.** 给值加引号。

<a id="dfx2020"></a>

## DFX2020

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown curve key attribute '%s'. Expected Interp, Tangent, Arrive or Leave.
```

**触发位置：** `Parser/DreamFXParser.cpp:365`

**原因.** 曲线键的属性列表只接受 `Interp`、`Tangent`、`Arrive` 和 `Leave`。

**修复.** 检查拼写。切线只有在 `Interp` 让它有意义时才被采纳。

<a id="dfx2021"></a>

## DFX2021

**严重级别：** 错误 (`error`)

**消息：**

```text
File declares '%s' but its extension is '%s'. Rename the file to '%s' or change the declaration.
```

**触发位置：** `Parser/DreamFXParser.cpp:1754`

**原因.** 声明的文档类型和文件扩展名对不上。

**修复.** 改文件名或改声明 —— 构建是按扩展名枚举源码的。

<a id="dfx2022"></a>

## DFX2022

**严重级别：** 错误 (`error`)

**消息：**

```text
Unexpected '%s' after the end of the document. A DreamFX file declares exactly one top-level object.
```

**触发位置：** `Parser/DreamFXParser.cpp:1783`

**原因.** 顶层对象的收尾花括号后面还有内容。通常是某个块被复制了一份，或者上面某处少了一个花括号。

**修复.** 一个文件，一个顶层对象。报出的位置是多余内容开始的地方，不是花括号失配的地方。

<a id="dfx2023"></a>

## DFX2023

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is a module call, so it cannot be given a type. Types are written only on assignments.
```

**触发位置：** `Parser/DreamFXParser.cpp:919`

**原因.** 模块调用前面写了类型。类型是给赋值用的 —— L2 需要知道一个新属性是什么类型；模块调用的类型来自它自己的 schema。

**修复.** 把类型去掉。

<a id="dfx2024"></a>

## DFX2024

**严重级别：** 错误 (`error`)

**消息：**

```text
'disabled' can only prefix a module call, and '%s' is an assignment.
```

**触发位置：** `Parser/DreamFXParser.cpp:930`

**原因.** `disabled` 前缀的是**模块调用**，而它写在的这条语句是赋值。

赋值没有东西可以禁用：它会和这个栈里其他所有赋值一起折进该栈自己的 Set Parameters 模块，
把那个模块关掉会静默地把它们全部丢掉。

**修复.** 删掉 `disabled`，改成把这一行注释掉；或者把这个赋值挪进一个可以整体停用的模块调用里。

<a id="dfx2025"></a>

## DFX2025

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown OnEvent argument '%s'. Expected Source, Event, Mode, SpawnNumber, MaxEventsPerFrame, UpdateAttributeInitialValues, RandomSpawnNumber or MinSpawnNumber.
```

**触发位置：** `Parser/DreamFXParser.cpp:1311, Parser/DreamFXParser.cpp:1319, Parser/DreamFXParser.cpp:1334`

**原因.** `OnEvent(...)` 头部带了一个解析器不认识的参数，或者某个参数缺了值，或者必填参数（`Source`、`Event`）根本没写。

这个头部不是模块调用 —— 它的参数配置的是**处理器自身**（听哪个 emitter 的事件、怎么花掉它们），
所以接受的集合是固定的：`Source`（本系统里的 emitter 名）、`Event`（事件名，带引号）、
`Mode`（`SpawnedParticles` 或 `EveryParticle`）、`SpawnNumber`、`MaxEventsPerFrame`、
`UpdateAttributeInitialValues`、`RandomSpawnNumber`、`MinSpawnNumber`。**响应**事件的模块写在块里面，不写在头部。

**修复.** 按上面的列表拼写（匹配时忽略大小写），每个参数给一个形状正确的值 ——
`Source` 和 `Mode` 是标识符，`Event` 是名字或字符串，数量是整数，标志是 `true`/`false` ——
并且至少写上 `Source` 和 `Event`：

```
OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 1) = {
    ReceiveLocationEvent();
}
```

<a id="dfx2026"></a>

## DFX2026

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown Stage argument '%s'. Expected Iteration, DataInterface, NumIterations, ExecuteBehavior or Enabled.
```

**触发位置：** `Parser/DreamFXParser.cpp:1447, Parser/DreamFXParser.cpp:1455`

**原因.** `Stage name(...)` 头部带了一个语法不认识的参数，或者某个参数的值形状不对。
四个参数是：`Iteration`（一个标识符，取 `ENiagaraIterationSource` 的条目：`Particles`、
`DataInterface`、`DirectSet`）、`DataInterface`（绑定网格的点号名，字符串或裸标识符均可）、
`NumIterations`（整数）和 `Enabled`（`true`/`false`）。全部可选 —— 裸的
`Stage name = { }` 就是一个启用的、按粒子迭代、跑一次的 stage。

**修复.** 照上面的列表拼写；这四个之外的 stage 属性（执行行为、state 迭代那一组）目前还没有文本形式，不能在这里要求。
