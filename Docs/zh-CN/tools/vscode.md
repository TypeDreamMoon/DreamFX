---
title: VSCode 与模块索引
description: workspace 文件长什么样、VSCode 怎么被找到、跳转链接的规则，以及 .dfx-index.json 里为什么要有输入签名。
---

<a id="open-dreamfx-workspace"></a>

## Open DreamFX Workspace

*Tools ▸ DreamFX ▸ Open DreamFX Workspace (VSCode)* 重写 `DFX/DreamFX.code-workspace`，
然后按 VSCode → 系统默认编辑器 → Notepad 的顺序启动它。

```json
{
  "folders": [
    { "name": "DreamFX Source", "path": "." },
    { "name": "Plugin: DreamFX", "path": "../Plugins/DreamFX/DFX" }
  ],
  "settings": {
    "files.associations": { "*.dfs": "dreamfxlang", "*.dfe": "dreamfxlang", "*.dfm": "dreamfxlang" }
  },
  "extensions": {
    "recommendations": ["typedreammoon.dreamfxlang-language-support"]
  }
}
```

**警告：**

**这个文件每次都被完整重写，从不合并。** 手加的 `launch` 或 `tasks` 块会丢。
  个人配置放 `DFX/.vscode/`。

项目根永远排第一、永远是 `"."` —— workspace 文件就住在里面，而稳定的 folder 标识能让 VSCode
的逐 folder 状态在重写后仍然挂得住。在另一个盘上的插件根在 Windows 上没有相对形式，
会写成绝对路径。

`dreamfxlang` 是 [DreamFXLang 扩展](https://github.com/TypeDreamMoon/dreamfx-language-support)
注册的语言 id。这条关联无论扩展装没装都会写 —— 没装的话 VSCode 回退到纯文本，无害 ——
而推荐项是 VSCode 只提示一次、拒绝后不再提示的东西。

| toast | 条件 |
| --- | --- |
| `DreamFX failed to create workspace: {Error}` | 文件写不出去 |
| `Opened DreamFX workspace in VSCode: {Path}` | VSCode 起来了 |
| `Opened DreamFX workspace: {Path}` | 系统默认编辑器起来了 |
| `Opened DreamFX workspace in Notepad: {Path}` | Notepad 起来了 |
| `DreamFX could not open workspace: {Path}` | 每个启动器都失败 |

<a id="vscode-是怎么找到的"></a>

## VSCode 是怎么找到的

Windows only，从最具体到最一般，第一个磁盘上存在的候选胜出：

| # | 位置 |
| :-- | :-- |
| 1–2 | `%LOCALAPPDATA%\Programs\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 3–4 | `%LOCALAPPDATA%\Programs\Microsoft VS Code Insiders\{Code - Insiders.exe, bin\code-insiders.cmd}` |
| 5–6 | `%ProgramFiles%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 7–8 | `%ProgramFiles(x86)%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 9 | 每一个 `PATH` 条目，检查 `code.cmd`、`code.exe`、`Code.exe`、`code-insiders.cmd`、`Code - Insiders.exe` |

打开一个 **workspace** 会遵守 *Open Workspace In New Window* 设置。打开一个**文件**永远传
`--reuse-window -g <path>:<line>:<col>` —— 每跳一条诊断就开一个新窗口没法用。

<a id="模块索引"></a>

## 模块索引

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 index
```

写出 `DFX/.dfx-index.json`：搜索路径暴露的每一个模块和动态输入，带它声明的栈、分类、描述和
**输入签名**。编辑器扩展读这个文件做补全和 hover —— 它没法去问引擎，因为每一次 `dfx` 调用都要
boot 引擎，而在有人正在打字的时候，任何要花几十秒的东西都不可接受。

两半的成本差得很远：

| 部分 | 成本 | 来源 |
| --- | --- | --- |
| 栈与元数据 | 免费 | 资产的 usage 位掩码 —— 引擎自己的栈 UI 也是用它过滤的 |
| 输入签名 | 贵 | **探测出来的**：资产层面的 schema 完全看不见内联 edit condition 和静态开关 |

本机实测：571 个模块，不探测 2.9 秒，带探测 8.1 秒。一个没有枚举形输入的补全列表比没有补全更糟，
所以这个成本是值得付的。

| 参数 | |
| --- | --- |
| `-Out <path>` | 写到别处 |
| `-NoInputs` | 跳过探测。快，而且足够回答「有什么」 |
| `-Retry` | 清空隔离名单重来 —— 引擎升级后用 |

**警告：**

**有些模块图走不动。** `/Niagara/Modules/Masks/ConeMask` 会让引擎的遍历无限递归，
  栈溢出结束进程。这趟走查是可续的：即将探测的模块先记下来，下次启动时还在记录里的被隔离。
  `dfx.ps1` 会重跑直到走完。被隔离的模块保留名字、路径和栈，只缺输入 —— **而且它会说出来。**

索引会记下引擎路径和启用插件列表，因为正是这两样让它失效：换个引擎就是另一套模块，
启用一个内容插件就多一整族。目前没有东西去比对它们（那需要一个活编辑器来比），
所以重建索引是一条命令，而不是被猜出来的动作。

<a id="提交与否"></a>

## 提交与否

不要提交 `DFX/.dfx-index.json`：它带着本机的引擎路径。见[目录与命名](../getting-started/project-layout.md)。
