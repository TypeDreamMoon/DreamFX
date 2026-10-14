---
title: "Agent 技能"
description: "插件自带的四个技能，以及编码代理在编写 DreamFXLang 时必须遵循的“先读 schema”循环。"
---

插件在
[`.skill/`](https://github.com/TypeDreamMoon/DreamFX/tree/main/.skill) 下自带四个 agent 技能。它们是给编码代理（Claude Code 之类）的说明：如何编写、构建、调试和迁移特效，全程 headless。

| 技能 | 做什么 |
| --- | --- |
| `dream-fx-create` | 按自然语言描述编写新的 `.dfs` / `.dfe` / `.dfm`，然后**构建它以证明它能编译** |
| `dream-fx-verify` | 以 headless 方式构建或检查：单个文件，或整棵 DFX 树。其他三个技能调用的执行环境 |
| `dream-fx-diagnose` | 接收一个 `DFXnnnn`，查找成因、解释它、修复源码 |
| `dream-fx-decompile` | 将现有 Niagara 系统导出为源码，或回答“这个项目的 VFX 有多少可以表达” |

<a id="the-step-everyone-skips"></a>

## 每个人都会跳过的一步

`dream-fx-create` 是一个五步循环，而第 2 步才是关键：


<a id="find-the-modules"></a>

### 找到模块
`dfx.ps1 list` 会打印搜索路径暴露的每个模块。

<a id="read-their-real-signatures"></a>

### 读取它们的真实签名
`dfx.ps1 schema <Module>` —— **每个你打算调用的模块读一次。**

这就是人们会跳过、然后花上二十分钟的那一步。模块输入名是从真实资产上读出来的：带空格的名字、由静态开关才显示出来的输入、只存在于某个栈中的输入。没有人——包括模型——能可靠地猜出它们。

<a id="write-the-file"></a>

### 编写文件
放在一个 `DFX/` 根目录下。

<a id="build-it"></a>

### 构建它
`dfx.ps1 build <file> -Force`。

<a id="fix-and-repeat"></a>

### 修复并重复
诊断信息带有行号和列号；在[诊断](../diagnostics/overview.md)中查找该代码。


> **注意：**
>
> **证明这一步不是可选的。** “看起来没问题”在这里毫无意义——只有构建通过才说明模块名、输入名、类型和开关顺序都正确。这也是为什么四个技能中有一个专门只是构建门禁。

<a id="wiring-them-into-your-own-agent"></a>

## 把它们接入你自己的代理

这些技能就是纯文本加上 `dfx.ps1`，没有其他依赖。要在你的项目中使用它们：

1. 让代理运行 `pwsh`；
2. 使引擎可解析（`UE_ENGINE_ROOT` 或已注册的 `EngineAssociation`）；
3. **写包的命令需要关闭编辑器** —— 确保代理知道这一点；
4. 将 `.skill/` 中的四个 `SKILL.md` 文件放入你代理的技能目录。

<a id="why-this-language-suits-agents"></a>

## 为什么这门语言适合代理

不是因为时代——而是因为三件具体的事：

- **文本是唯一的创作界面。** 代理会写文本，但它点不了 Slate 按钮。
- **诊断带有稳定的代码和位置。** `DFX3003` 始终是同一种失败模式，而行号和列号指向 token——这是一条程序可以消费的反馈通道，而不是一段散文。
- **构建是 headless 的，并且有退出码。** “它是否正确”有一个不需要人眼来看的答案。

这三点对人类同样有益。代理只是让人更难忽视“反馈通道必须机器可读”这一要求。
