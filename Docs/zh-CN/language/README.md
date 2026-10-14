---
title: "DreamFXLang 参考"
description: "DreamFXLang 参考"
---

<a id="dreamfxlang-reference"></a>

# DreamFXLang 参考

这门语言按你所写的内容划分。凡规则因引擎所迫而存在的 —— 栈为什么是语句列表、表达式白名单为什么很短、`#Region` 为什么只是注释 —— 页面都会在陈述该规则之处说明原因。

| 页面 | 涵盖内容 |
| --- | --- |
| [dfs.md](dfs.md) | `.dfs` —— 一个系统：user 参数、系统栈、发射器、renderer |
| [dfe.md](dfe.md) | `.dfe` —— 一个可复用发射器，以及 `from` 对它做了什么 |
| [system-inheritance.md](system-inheritance.md) | `.dfs` 源父级、覆盖、依赖重建和循环错误 |
| [native-emitter-inheritance.md](native-emitter-inheritance.md) | 带 `inherits` 的原生 Niagara 发射器父级 |
| [dfm.md](dfm.md) | `.dfm` —— 一个模块或 dynamic input，以及哪些引擎能生成它 |
| [values.md](values.md) | 每一个值位置、类型规则，以及按名称列出的八条 `L*` 规则 |

<a id="shape-of-every-file"></a>

## 每个文件的形状

一个顶层对象，在所有地方都用相同的头声明：

```cpp
<Kind>(Name="<path under the root>", Root="<root token>")
{
    ...
}
```

`Kind` 是 `System`、`Emitter`、`Module` 或 `DynamicInput`，并且必须与文件扩展名一致
(DFX2021)。`Root` 是 `Game`、空(同样含义)或 `Plugin.<PluginName>`；`Name` 是相对于该根的内容目录的资源
路径。

系统还接受 `Parent="../Base.dfs"`，用于[源继承](system-inheritance.md)。
发射器声明可以使用 `inherits "/Game/FX/NE_Base"` 指定
[原生 Niagara 父级](native-emitter-inheritance.md)。发射器 `from` 保留其源拷贝语义；
对 `.dfe` 的编辑会通过源监视器重建依赖系统。

块使用 `Name = { ... }`，语句以 `;` 结尾，`//` 和 `/* */` 是注释。
属性以方括号形式挂在声明上：`[ Group="Burst"; SortPriority=10 ]`。
在 `Properties` / `Inputs` 内部，`Group("Name") { ... }` 作用域会把这两个属性加盖到
其中包含的每一个参数上 —— 参见 [dfs.md](dfs.md)。

<a id="what-is-deliberately-not-here"></a>

## 有意不在这里的东西

- **`MaterialParam`。** 保留(L8)，未实现(DFX5093)。
- **任何需要通用表达式编译器的东西。** 参见 [values.md](values.md) 中的 L6。
