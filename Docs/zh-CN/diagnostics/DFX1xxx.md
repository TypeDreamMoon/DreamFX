---
title: DFX1xxx — 驱动与文件 I/O
description: DFX1xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 6 条（6 错误）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx1000"></a>

## DFX1000

**严重级别：** 错误 (`error`)

**消息：**

```text
Could not read source file '%s'.
```

**触发位置：** `Parser/DreamFXParser.cpp:1837`

**原因.** 路径不存在，或者进程读不了它。`from` 引用和 `-File=` 参数都落在这里。

**修复.** 检查路径。相对的 `from` 路径先相对引用它的文件解析，再相对每一个 DFX 根解析。

<a id="dfx1001"></a>

## DFX1001

**严重级别：** 错误 (`error`)

**消息：**

```text
Unterminated string literal.
```

**触发位置：** `Parser/DreamFXLexer.cpp:304`

**原因.** 一个带引号的字符串一直到行尾都没有闭合。

**修复.** 补上引号。DreamFX 的字符串不跨行。

<a id="dfx1002"></a>

## DFX1002

**严重级别：** 错误 (`error`)

**消息：**

```text
Unterminated block comment.
```

**触发位置：** `Parser/DreamFXLexer.cpp:127`

**原因.** 一个 `/*` 从来没有闭合。

**修复.** 补上 `*/`。报出来的位置是注释**开始**的地方，不是文件结束的地方。

<a id="dfx1003"></a>

## DFX1003

**严重级别：** 错误 (`error`)

**消息：**

```text
Unexpected character '%c' (U+%04X).
```

**触发位置：** `Parser/DreamFXLexer.cpp:335`

**原因.** 一个不属于这门语言的字符出现在字符串、注释和 raw block 之外。最常见的来源是从文档里粘过来的弯引号。

**修复.** 删掉它。码位会一并打印出来，所以长得像的字符也能认出是哪一个。

<a id="dfx1004"></a>

## DFX1004

**严重级别：** 错误 (`error`)

**消息：**

```text
Unterminated raw block: missing '}'.
```

**触发位置：** `Parser/DreamFXLexer.cpp:488`

**原因.** 一个 raw block —— `hlsl { }`、`Body = { }` —— 开了但没关。raw block 是按花括号配平的，所以 HLSL **内部**一个不配对的花括号会让这个块一直吃到文件末尾。

**修复.** 把块内部的花括号配平。

<a id="dfx1005"></a>

## DFX1005

**严重级别：** 错误 (`error`)

**消息：**

```text
Unterminated back-quoted name. A `name` must close on the same line.
```

**触发位置：** `Parser/DreamFXLexer.cpp:185, Parser/DreamFXLexer.cpp:198`

**原因.** 一个反引号开了一个名字，但到行尾都没有闭合；或者开了就立刻闭合，中间什么都没有。

反引号名字是这门语言用来承载那些否则写不出来的 Niagara 名字的办法 ——
`` `PillarPower(0~1)` ``、`` `Ring/DiscDistributionMode` ``。名字从不跨行，所以缺了闭合引号会在行尾就报出来，而不是把文件剩下的部分整个吞掉。

**修复.** 补上引号。如果名字本身含有一个反引号，那它就没有书写形式：转义还需要转义，而 Niagara 里也不会产出这种名字 —— 改掉那个参数的名字。

只在名字本身不是标识符时才需要反引号。`` `Speed` `` 合法但是噪音，写 `Speed`。反编译器就是照这条规则输出的，所以重新导出一次是看清「哪些名字需要引号」的最快办法。
