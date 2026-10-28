---
title: "DFX2xxx —— 词法与语法"
description: "DFX2xxx —— 词法与语法"
---

<a id="dfx2xxx-----lexer-and-syntax"></a>

# DFX2xxx —— 词法与语法

> 生成标记之间的块由 `.skill/gen-diagnostics.ps1` 写入。
> 标记之下的所有内容都是手写的，重新生成后仍然保留。

<a id="dfx2000"></a>

## DFX2000

<!-- generated:begin DFX2000 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unknown document type '%s'. Expected System, Emitter, Module or DynamicInput.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:2016`
<!-- generated:end DFX2000 -->

**原因.** 文件的第一个词不是四种文档类型之一。

**修复.** 写 `System`、`Emitter`、`Module` 或 `DynamicInput`，并与文件扩展名一致。

<a id="dfx2001"></a>

## DFX2001

<!-- generated:begin DFX2001 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Expected '{' to open a raw block but found '%s'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXLexer.cpp:410`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:101`
<!-- generated:end DFX2001 -->

**原因.** 一个需要 raw block 的构造后面没有跟 `{`。

**修复.** 补上花括号。`hlsl` 和 `Body` 永远是 `hlsl { ... }` / `Body = { ... }`。

<a id="dfx2002"></a>

## DFX2002

<!-- generated:begin DFX2002 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Expected a name but found %s.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:112`
<!-- generated:end DFX2002 -->

**原因.** 这里期待一个标识符 —— 模块名、输入名、发射器名。

**修复.** 检查报出位置之前有没有多余的标点。

<a id="dfx2003"></a>

## DFX2003

<!-- generated:begin DFX2003 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Expected a number but found '%s'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:425`
<!-- generated:end DFX2003 -->

**原因.** 这里期待一个数字：向量字面量里、`box()` 里，或者曲线的键上。

**修复.** 向量分量和曲线时间只能是数字，参数引用不能出现在那里。

<a id="dfx2004"></a>

## DFX2004

<!-- generated:begin DFX2004 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Expected a value but found %s.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:612`
<!-- generated:end DFX2004 -->

**原因.** `=` 后面期待一个值。

**修复.** 要么右边少了东西，要么上一条语句少了 `;`。

<a id="dfx2005"></a>

## DFX2005

<!-- generated:begin DFX2005 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'#EndRegion' has no matching '#Region'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:941`
<!-- generated:end DFX2005 -->

**原因.** 出现了 `#EndRegion`，但没有打开着的 `#Region`。

**修复.** 删掉它，或者补上开头的 `#Region "..."`。

<a id="dfx2006"></a>

## DFX2006

<!-- generated:begin DFX2006 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unknown directive '#%s'. Expected Region or EndRegion.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:951`
<!-- generated:end DFX2006 -->

**原因.** 指令只有 `#Region` 和 `#EndRegion` 两个。

**修复.** 检查拼写。`#Region` 是一种注释装置（L5），从不到达资产。

<a id="dfx2007"></a>

## DFX2007

<!-- generated:begin DFX2007 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Expected a version after '@'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1010`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:491`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:512`
<!-- generated:end DFX2007 -->

**原因.** `@` 引入一个 R7 版本固定，后面必须跟版本号。

**修复.** 写 `ModuleName@1.2`，或者把 `@` 去掉。

<a id="dfx2008"></a>

## DFX2008

<!-- generated:begin DFX2008 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Module '%s' was given a positional argument. Module inputs must be written as 'Name = Value'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1029`、`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1233`
<!-- generated:end DFX2008 -->

**原因.** 模块调用被喂了一个裸值。Niagara 按名字寻址模块输入，而它们在栈 UI 里出现的顺序并不是声明顺序。

**修复.** 每个实参都写成 `Name = Value`。`dfx schema <Module>` 会列出名字。

<a id="dfx2010"></a>

## DFX2010

<!-- generated:begin DFX2010 -->
**严重级别：** 警告 (`warning`)

**消息：**

```
'#Region \"%s\"' was never closed with '#EndRegion'.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1104`
<!-- generated:end DFX2010 -->

**原因.** 一个 `#Region` 到块结束都没等到 `#EndRegion`。无害 —— region 只是文本 —— 但通常意味着某一段被搬走了，闭合行留在了原地。

**修复.** 补上 `#EndRegion`。

<a id="dfx2013"></a>

## DFX2013

<!-- generated:begin DFX2013 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'%s' is a system-scope stack and must be written at the top level of a .dfs, not inside an emitter.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1310`
<!-- generated:end DFX2013 -->

**原因.** `SystemSpawn` 和 `SystemUpdate` 属于系统，不属于发射器（L1）。

**修复.** 把这个块挪到 `.dfs` 的顶层，和 `Emitter` 块并列。

<a id="dfx2015"></a>

## DFX2015

<!-- generated:begin DFX2015 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'from' must be followed by a quoted path to a .dfe source file.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1726`
<!-- generated:end DFX2015 -->

**原因.** `from` 后面要跟一个带引号的 `.dfe` 路径。

**修复.** 加引号。扩展名可省；路径先相对本文件解析。

<a id="dfx2016"></a>

## DFX2016

<!-- generated:begin DFX2016 -->
**严重级别：** 错误 (`error`)

**消息：**

```
(built at runtime)
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1254`
<!-- generated:end DFX2016 -->

**原因.** 一个 `Defaults = { … }` 块里出现了赋值以外的东西。默认值说的是「没人写过它时读出什么」，模块调用在那里没有任何含义。

**修复.** 把模块调用挪进某个栈（`ParticleSpawn`、`ParticleUpdate`…）。`Defaults` 里只能写 `<type> Namespace.Name = value;`。

<a id="dfx2018"></a>

## DFX2018

<!-- generated:begin DFX2018 -->
**严重级别：** 错误 (`error`)

**消息：**

```
A Module or DynamicInput must declare a 'Body = { }' block.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1914`
<!-- generated:end DFX2018 -->

**原因.** 一个没有 `Body` 的 `.dfm` 只声明了输入，生成出来的模块什么都不做。

**修复.** 加上 `Body = { ... }`。

<a id="dfx2019"></a>

## DFX2019

<!-- generated:begin DFX2019 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Header argument '%s' must be a quoted string.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1942`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1961`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1970`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1978`
<!-- generated:end DFX2019 -->

**原因.** 文件头的参数（`Name=`、`Root=`）是带引号的字符串。

**修复.** 给值加引号。

<a id="dfx2020"></a>

## DFX2020

<!-- generated:begin DFX2020 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unknown curve key attribute '%s'. Expected Interp, Tangent, Arrive or Leave.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:396`
<!-- generated:end DFX2020 -->

**原因.** 曲线键的属性列表只接受 `Interp`、`Arrive` 和 `Leave`（plan 3.5）。

**修复.** 检查拼写。切线只有在 `Interp` 让它有意义时才被采纳。

<a id="dfx2021"></a>

## DFX2021

<!-- generated:begin DFX2021 -->
**严重级别：** 错误 (`error`)

**消息：**

```
File declares '%s' but its extension is '%s'. Rename the file to '%s' or change the declaration.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:2030`
<!-- generated:end DFX2021 -->

**原因.** 声明的文档类型和文件扩展名对不上。

**修复.** 改文件名或改声明 —— 构建是按扩展名枚举源码的。

<a id="dfx2022"></a>

## DFX2022

<!-- generated:begin DFX2022 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unexpected '%s' after the end of the document. A DreamFX file declares exactly one top-level object.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:2059`
<!-- generated:end DFX2022 -->

**原因.** 顶层对象的收尾花括号后面还有内容。通常是某个块被复制了一份，或者上面某处少了一个花括号。

**修复.** 一个文件，一个顶层对象。报出的位置是多余内容开始的地方，不是花括号失配的地方。

<a id="dfx2023"></a>

## DFX2023

<!-- generated:begin DFX2023 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'%s' is a module call, so it cannot be given a type. Types are written only on assignments.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1070`
<!-- generated:end DFX2023 -->

**原因.** 模块调用前面写了类型。类型是给赋值用的 —— L2 需要知道一个新属性是什么类型；模块调用的类型来自它自己的 schema。

**修复.** 把类型去掉。

<a id="dfx2024"></a>

## DFX2024

<!-- generated:begin DFX2024 -->
**严重级别：** 错误 (`error`)

**消息：**

```
A Defaults declaration cannot be disabled; defaults are not executed stack nodes.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1248`
<!-- generated:end DFX2024 -->

**原因.** `disabled` 被用在了 `Defaults` 内的一个声明上。默认值注册的是参数值；它们不是可执行的栈节点。可执行的栈赋值和模块调用确实支持 `disabled`。

**修复.** 从默认值声明中移除 `disabled`，如果这个参数不再需要，就直接删掉该声明本身。

<a id="dfx2025"></a>

## DFX2025

<!-- generated:begin DFX2025 -->
**严重级别** 错误

**消息**

```
Unknown OnEvent argument '%s'. Expected Source, Event, Mode, SpawnNumber, MaxEventsPerFrame, UpdateAttributeInitialValues, RandomSpawnNumber or MinSpawnNumber.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1468`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1476`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1491`
<!-- generated:end DFX2025 -->

**原因.** 一个 `OnEvent(...)` 头部带了一个解析器不认识的参数，或者某个参数缺了值，或者必填参数（`Source`、`Event`）根本没写。

这个头部不是模块调用 —— 它的参数配置的是*处理器自身*（听哪个 emitter 的事件、怎么花掉它们），
所以接受的集合是固定的：`Source`（本系统里的 emitter 名）、`Event`（事件名，带引号）、`Mode`（`SpawnedParticles` 或 `EveryParticle`）、`SpawnNumber`、`MaxEventsPerFrame`、`UpdateAttributeInitialValues`、`RandomSpawnNumber`、`MinSpawnNumber`。*响应*事件的模块写在块里面，不写在头部。

**修复.** 按上面的列表拼写参数（匹配时忽略大小写），每个参数给一个形状正确的值 ——
`Source` 和 `Mode` 是标识符，`Event` 是名字或字符串，数量是整数，标志是 `true`/`false` ——
并且至少写上 `Source` 和 `Event`：

```
OnEvent(Source = Sparks, Event = "LocationEvent", Mode = SpawnedParticles, SpawnNumber = 1) = {
    ReceiveLocationEvent();
}
```

<a id="dfx2026"></a>

## DFX2026

<!-- generated:begin DFX2026 -->
**严重级别** 错误

**消息**

```
Unknown Stage argument '%s'. Expected a stage iteration, enabled, dispatch or particle-state option; see the Stage language reference.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:1690`、`Source/DreamFX/Private/Parser/DreamFXParser.cpp:1698`
<!-- generated:end DFX2026 -->

**原因.** 一个 `Stage name(...)` 头部包含了一个未知参数，或者某个参数的值类型不对。Stage 选项涵盖迭代、启用、执行行为、直接派发、线程组和粒子状态过滤。

**修复.** 使用 [Stage 参考](../language/dfs.md) 中的参数名和值类型。裸 `Stage name = { }` 使用引擎默认值；参数绑定必须命名一个对应类型的参数。

<a id="dfx2027"></a>

## DFX2027

<!-- generated:begin DFX2027 -->
**严重级别** 错误

**消息**

```
Group(...) takes a quoted name: Group(\"Name\") { ... }.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:839`
<!-- generated:end DFX2027 -->

**原因.** 一个 `Group(...)` 声明的名字没加引号或者缺失。

**修复.** 写一个带引号的名字，例如 `Group("Sparks") { ... }`。

<a id="dfx2028"></a>

## DFX2028

<!-- generated:begin DFX2028 -->
**严重级别** 错误

**消息**

```
Group(...) requires a non-empty name.
```

**触发位置：** `Source/DreamFX/Private/Parser/DreamFXParser.cpp:847`
<!-- generated:end DFX2028 -->

**原因.** 一个 `Group(...)` 声明使用了空名字。

**修复.** 给这个组一个非空名字，这样它的 emitter 才能有一致地组织起来。
