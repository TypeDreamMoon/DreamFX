---
title: "系统源继承"
description: "系统源继承"
---

<a id="system-source-inheritance"></a>

# 系统源继承

`.dfs` 系统可以借助 `Parent` 继承另一个 `.dfs` 源：

```cpp
// Base.dfs
System(Name="FX/NS_Base", Root="Game")
{
    Properties = { float Speed = 100.0; Color Tint = (1, 0, 0, 1); }
    Emitter Sparks from "Emitters/E_Sparks" {}
}
```

```cpp
// Variants/Blue.dfs
System(Name="FX/NS_Blue", Root="Game", Parent="../Base.dfs")
{
    Properties = { Color Tint = (0, 0, 1, 1); }
    Emitter Sparks { Settings = { LocalSpace = true; } }
}
```

子级生成 `/Game/FX/NS_Blue`。它保留 `Speed`，覆盖 `Tint`，并继承 `Sparks` 发射器及其附加的 `LocalSpace` 设置。发射器的 `Emitters/E_Sparks` 引用仍相对于 `Base.dfs` 解析，即使子级位于另一个目录中。

这是 DreamFX 在生成之前执行的源组合。它不会创建原生 Niagara System 父子关系。对于原生 Niagara 发射器关系，请使用[发射器资产继承](native-emitter-inheritance.md)。

<a id="resolution-and-merge-rules"></a>

## 解析与合并规则

`Parent` 在 `System` 头文件中只允许出现一次，且需带有非空的带引号源路径。`.dfs` 扩展名是可选的。解析时首先检查声明文件所在目录，然后检查配置的 DreamFX 源根目录，使用与发射器 `from` 引用相同的规则。父级可以拥有父级；循环以及长度超过 128 个源文档的链会在生成更改资产之前被拒绝。

| 部分 | 子级行为 |
| --- | --- |
| `Name`、`Root`、源标识 | 始终来自子级 |
| `Settings` | 覆盖匹配的键；保留其他父级设置 |
| `Properties` | 覆盖匹配的参数名称；追加新参数 |
| 参数类型 | 必须与继承的 Niagara 类型匹配；不兼容的覆盖会报错 |
| 系统堆栈 | 整体替换匹配的堆栈；保留其他堆栈 |
| 发射器 | 按发射器名称合并；保留父级发射器并追加新的发射器 |
| 发射器设置与 `Defaults` | 使用发射器覆盖规则按设置或参数名称合并 |
| 发射器堆栈 | 替换匹配的堆栈类型；命名的 `Stage` 块按阶段名称匹配 |
| 渲染器声明 | 非空的子级渲染器列表会替换该发射器的渲染器列表 |
| 发射器 `from` | 保留父级的引用和原始源目录，除非子级提供了另一个 |

子级发射器上的显式 `from` 或 `inherits` 会选择其新的基础并清除另一种基础类型。其他继承的覆盖仍然保留：切换基础不会丢弃父级的设置、默认值或已合并到该发射器中的堆栈。

参数声明会替换整个继承的声明，包括其默认值和属性。空堆栈会用空堆栈显式替换父级堆栈。此版本中没有用于删除继承的发射器、参数、设置或渲染器列表的语法。名称使用 Niagara 不区分大小写的名称标识。一个源中的重复参数或发射器声明会被拒绝，而不是静默地成为彼此的覆盖。

子级的 `Root` 也是生成期间的默认资产根目录。在不同内容根目录之间共享父级时，请使用显式的 `/Game/...`、`/PluginName/...` 或 `Plugin.Name:...` 路径编写资产引用。诸如 `Parent` 和发射器 `from` 之类的文件引用会保留声明它们的源的目录。

<a id="rebuilds-and-verification"></a>

## 重建与验证

生成子级的源指纹包括每个父级源的内容及其解析后的 `.dfe` 依赖项。仅编辑父级会使普通 `build` 重新生成子级，并使 `verify` 报告漂移。验证不会应用编辑或保存资产。指纹使用编写的引用和源内容，因此移动同一源树不会仅因为其绝对检出路径更改而产生漂移。

源监视器遵循相同的父级图：对父级 `.dfs` 或引用的 `.dfe` 的编辑会将受影响的子级和孙级排队。现有的批量重建阈值仍然适用。诊断会保留继承声明的源文件，因此父级中的错误会指向该父级文件。

| 诊断 | 含义 |
| --- | --- |
| `DFX2019` | 无效的 `Parent` 头文件参数 |
| `DFX3050` | 找不到父级源 |
| `DFX3051` | 父级源无法解析或不是 System |
| `DFX3052` | 循环继承或父级深度过大 |
| `DFX3053` | 子级参数改变了继承的类型 |
| `DFX3054` | 在一个源层中存在重复的参数或发射器 |
