---
title: "DFX1xxx — 驱动与文件 I/O"
description: "DFX1xxx — 驱动与文件 I/O"
---

<a id="dfx1xxx-----driver-and-file-io"></a>

# DFX1xxx — 驱动与文件 I/O

> 生成标记之间的块由 `.skill/gen-diagnostics.ps1` 写入。
> 标记下方的所有内容均为手写，重新生成后仍会保留。

<a id="dfx1000"></a>

## DFX1000

<!-- generated:begin DFX1000 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Could not read source file '%s'.
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXParser.cpp:2133``, ``Source/DreamFXEditor/Private/WriteBack/DreamFXPull.cpp:2527``
<!-- generated:end DFX1000 -->

**原因.** 路径不存在，或者进程读不了它。``from`` 引用和 ``-File=`` 参数都落在这里。

**修复.** 检查路径。相对的 ``from`` 路径先相对引用它的文件解析，再相对每一个 DFX 根解析。

<a id="dfx1001"></a>

## DFX1001

<!-- generated:begin DFX1001 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unterminated string literal.
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:314``
<!-- generated:end DFX1001 -->

**原因.** 一个带引号的字符串一直到行尾都没有闭合。

**修复.** 补上引号。DreamFX 的字符串不跨行。

<a id="dfx1002"></a>

## DFX1002

<!-- generated:begin DFX1002 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unterminated block comment.
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:127``
<!-- generated:end DFX1002 -->

**原因.** 一个 ``/*`` 从来没有闭合。

**修复.** 补上 ``*/``。报出来的位置是注释开始的地方，不是文件结束的地方。

<a id="dfx1003"></a>

## DFX1003

<!-- generated:begin DFX1003 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unexpected character '%c' (U+%04X).
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:345``
<!-- generated:end DFX1003 -->

**原因.** 一个不属于这门语言的字符出现在字符串、注释或 raw block 之外。最常见的来源是从文档里粘过来的弯引号。

**修复.** 删掉它。码位会一并打印出来，所以长得像的字符也能认出是哪一个。

<a id="dfx1004"></a>

## DFX1004

<!-- generated:begin DFX1004 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unterminated raw block: missing '}'.
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:502``
<!-- generated:end DFX1004 -->

**原因.** 一个 raw block —— ``hlsl { }``, ``Body = { }`` —— 开了但没关。raw block 是按花括号配平的，所以 HLSL **内部**一个不配对的花括号会让这个块一直吃到文件末尾。

**修复.** 把块内部的花括号配平。

<a id="dfx1005"></a>

## DFX1005

<!-- generated:begin DFX1005 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Unterminated back-quoted name. A `name` must close on the same line.
```

**触发位置：** ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:195``, ``Source/DreamFX/Private/Parser/DreamFXLexer.cpp:208``
<!-- generated:end DFX1005 -->

**原因.** 一个反引号开了一个名字，但到行尾都没有闭合；或者开了就立刻闭合，中间什么都没有。

反引号名字是这门语言用来承载那些否则写不出来的 Niagara 名字的办法 ——
``` `PillarPower(0~1)` ```, ``` `Ring/DiscDistributionMode` ```。名字从不跨行，所以缺了闭合引号会在行尾就报出来，而不是把文件剩下的部分整个吞掉。

**修复.** 补上引号。如果名字本身含有一个反引号，那它就没有书写形式：转义还需要转义，而 Niagara 里也不会产出这种名字 —— 改掉那个参数的名字。

只在名字本身不是标识符时才需要反引号。``` `Speed` ``` 合法但是噪音，写 ``Speed``。反编译器就是照这条规则输出的，所以重新导出一次是看清「哪些名字需要引号」的最快办法。
