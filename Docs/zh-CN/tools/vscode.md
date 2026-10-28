---
title: "VSCode 与模块索引"
description: "workspace 文件包含什么、VSCode 如何被发现、跳转链接的规则，以及 .dfx-index.json 为什么要探测输入签名。"
---

<a id="open-dreamfx-workspace"></a>

## Open DreamFX Workspace

*Tools ▸ DreamFX ▸ Open DreamFX Workspace (VSCode)* 会重写 `DFX/DreamFX.code-workspace`，然后
依次通过 VSCode → 系统默认编辑器 → 记事本启动它。

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

> **警告：**
>
> **该文件每次都会被完全重写，从不合并。** 手动添加的 `launch` 或 `tasks` 块会
> 丢失。每用户的配置应放在 `DFX/.vscode/` 中。

项目根始终排在第一位，且始终为 `"."` —— workspace 文件就位于其中，而稳定的
文件夹标识能让 VSCode 的逐文件夹状态在重写后依然保持关联。位于其他
驱动器上的插件根在 Windows 上没有相对路径形式，因此会使用绝对路径。

`dreamfxlang` 是
[DreamFXLang 扩展](https://github.com/TypeDreamMoon/dreamfx-language-support)注册的语言 id。无论该扩展是否安装，
这条关联都会被写入 —— 没有它 VSCode 会回退到纯文本，这无害 —— 而推荐项是 VSCode 只显示一次、
被忽略后便不再显示的提示。

| 提示 | 条件 |
| --- | --- |
| `DreamFX failed to create workspace: {Error}` | 文件无法写入 |
| `Opened DreamFX workspace in VSCode: {Path}` | VSCode 已启动 |
| `Opened DreamFX workspace: {Path}` | 系统默认编辑器已启动 |
| `Opened DreamFX workspace in Notepad: {Path}` | 记事本已启动 |
| `DreamFX could not open workspace: {Path}` | 所有启动器都失败 |

<a id="how-vscode-is-discovered"></a>

## VSCode 如何被发现

仅限 Windows，从最具体到最一般。磁盘上存在的第一个候选者胜出。

| # | 位置 |
| :-- | :-- |
| 1–2 | `%LOCALAPPDATA%\Programs\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 3–4 | `%LOCALAPPDATA%\Programs\Microsoft VS Code Insiders\{Code - Insiders.exe, bin\code-insiders.cmd}` |
| 5–6 | `%ProgramFiles%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 7–8 | `%ProgramFiles(x86)%\Microsoft VS Code\{Code.exe, bin\code.cmd}` |
| 9 | 每一个 `PATH` 条目，检查 `code.cmd`、`code.exe`、`Code.exe`、`code-insiders.cmd`、`Code - Insiders.exe` |

打开一个 **workspace** 会遵守 *Open Workspace In New Window*。打开一个**文件**时始终传递
`--reuse-window -g <path>:<line>:<col>` —— 每次诊断跳转都开一个新窗口将无法使用。

<a id="the-module-index"></a>

## 模块索引

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 index
```

写出 `DFX/.dfx-index.json`：搜索路径暴露的每一个模块和动态输入，以及其
声明的栈、分类、描述和**输入签名**。编辑器扩展读取该文件
用于补全和悬停 —— 它无法询问引擎，因为每一次 `dfx` 调用都会启动引擎，而
在有人打字时发生的任何事都不能花费数十秒。

这两部分的成本差异很大：

| 部分 | 成本 | 来源 |
| --- | --- | --- |
| 栈和元数据 | 免费 | 资产的 usage 位掩码 —— 与引擎自带堆栈 UI 过滤所用的列表相同 |
| 输入签名 | 昂贵 | **探测得出**，因为资产级 schema 完全忽略了内联编辑条件和静态开关 |

本机实测：571 个模块，不探测 2.9 秒，带探测 8.1 秒。一个不含
枚举形输入的补全列表比没有补全更糟，因此这个成本值得付出。

| 标志 | |
| --- | --- |
| `-Out <path>` | 写到别处 |
| `-NoInputs` | 跳过探测。速度快，足以回答“存在什么” |
| `-Retry` | 清除隔离名单并重试每一个模块 —— 用于引擎升级之后 |

> **警告：**
>
> **某些模块图无法遍历。** `/Niagara/Modules/Masks/ConeMask` 会使引擎自身的
> 遍历无界递归，而栈溢出会结束进程。因此遍历是可续的：
> 即将被探测的模块会先被记录，而下一次运行开始时仍被记录的模块会被隔离。`dfx.ps1` 会
> 重复运行直到遍历完成。被隔离的模块会保留其
> 名称、路径和栈；只缺少其输入 —— **并且它会说明这一点。**

索引会记录引擎路径和已启用插件列表，因为正是这些会让它失效：
不同的引擎意味着不同的模块集，而启用一个内容插件会增加一整族。
目前还没有任何机制比较它们 —— 那需要一个实时编辑器来比对 —— 因此重建是一条命令，
而不是靠猜测。

<a id="committing-it"></a>

## 提交

不要提交 `DFX/.dfx-index.json`：它带有本机的引擎路径。见
[布局与命名](../getting-started/project-layout.md)。
