---
title: "DreamFX 是什么"
description: "它做什么、不做什么，以及贯穿始终的几条立场——文本是唯一真相、缺口必须显形、菜单不是第二套实现。"
---

DreamFX 把文本编译成标准 Niagara 资产，并把标准 Niagara 资产反编译回文本。这就是全部范围——它不是新的粒子运行时，也不替代 Niagara。它生成的产物是普通的 `UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript`：任何引擎都能加载它，任何 `.dfs` 都能引用它，它能被 cook、能运行，与手工连出来的完全一样。

<a id="why-text-at-all"></a>

## 为什么要用文本

Niagara 的编辑面是图和栈。图很好，直到你需要：

- **diff 一次改动**——谁把生成率从 20 改成了 60？二进制资产回答不了这个问题；
- **一次编辑 40 个特效**——点 40 次，还是改 40 个文本文件？
- **审查代码**——一个包含 `.uasset` 的 PR 只展示结论，看不到任何推理过程；
- **把工作交给代理**——编码代理能写文本、跑构建、读诊断。它没法点 Slate 按钮。

文本化不是为了更“高级”。它是为了让**版本控制、批量编辑和自动化**重新成为可能。

<a id="the-stances"></a>

## 几条立场

<a id="the-text-is-the-truth-the-asset-is-build-output"></a>

### 文本是唯一真相，资产是构建产物

生成的资产带着溯源戳：源 hash、生成器版本、模块版本 GUID。手工改它，下次重建就会把这个改动抹掉；改了源却不重建，`verify` 会把它抓出来。“资产随时可以删掉重建”这句话必须始终为真，否则整个模型就塌了。

<a id="a-gap-must-be-visible-never-dropped"></a>

### 缺口必须显形，绝不静默丢弃

当反编译遇到 DreamFXLang 无法表达的东西时，它**不会**悄悄丢掉：这一项会作为缺口写入导出文件的头部，并以 `DFX8xxx` 警告的形式报出。`Adopt` 更严格——只要有任何缺口，它就拒绝接管该资产。

一个静默降级的工具，会让你在三个月后对着一个看起来不对劲的特效发呆，却没有一条线索可查。

<a id="a-menu-is-an-entry-point-not-a-second-implementation"></a>

### 菜单是入口，不是第二套实现

编辑器里的每个按钮都调用 commandlet 调用的同一份代码。如果 *Rebuild DFX* 能用，`dfx build -All` 就能用，反之亦然。每个命令也都能从 Python 调起——一个只有人手点击才能触达的命令，就是一个永远得不到回归测试的命令。

<a id="a-claim-has-to-be-measurable"></a>

### 声称必须可测量

“反编译是无损的”这句话毫无意义，除非有东西能证伪它。因此有四层验证：文本逐行、镜像编译、运行时固定步长的 SimCache 对比，以及反射遍历资产本身进行对比。第四层存在的理由很具体——前面几层比较的是*同一个*导出器的两份输出，所以导出器在两边都犯的同一个损失是对称的，对它们来说不可见。

<a id="what-it-does-not-do"></a>

## 它不做什么

| | |
| --- | --- |
| **Scratch Pad** | 无法表达。Scratch Pad 脚本是嵌入资产中的图，没有稳定的名字 |
| **把模块体降为节点图** | 一个 `.dfm` 体变成一个自定义 HLSL 节点。把它降下来就是 DreamShader 已有的那约 1.3 万行问题，原样重现 |
| **GPU/CPU 分支条件、Scalability 条件** | 未覆盖 |
| **通过 `from` 的隐式原生继承** | `from` 在重建时拷贝并合并 `.dfe` 源。要用原生 Niagara 父子关系，请使用 [`inherits`](../language/native-emitter-inheritance.md) |
| **通用表达式编译器** | 内联表达式是算术加上一份短白名单。再多的内容属于 `hlsl { }` 或 `.dfm` |

完整清单，加上 1.0.0 的已知问题，都在[限制](../diagnostics/limitations.md)中。

<a id="its-relationship-to-dreamshader"></a>

## 它与 DreamShader 的关系

[DreamShaderLang](https://lang.64hz.cn) 是同一作者的姊妹项目，为材质做同样的事：`.dsm` / `.dsf` / `.dsh` 生成 `UMaterial` 和 `UMaterialFunction` 资产。

两者共享立场（文本是源、诊断携带位置、编辑器集成运行同一管线），但**不共享代码**，语法也不可互换。一个明显的分岔：DreamShader 有完整的表达式编译器，而 DreamFX 有意不做——Niagara 的模块生态已经解决了“计算”这件事，所以 DreamFX 的工作是把模块**接起来**，而不是再造运算符。

<a id="names-and-versions"></a>

## 名字与版本

| | |
| --- | --- |
| 插件 | DreamFX `1.0.0` |
| 语言 | DreamFXLang |
| 模块 | `DreamFX`（Runtime）、`DreamFXEditor`（Editor） |
| Commandlet | `-run=DreamFX` |
| 驱动 | `.skill/dfx.ps1`、`.skill/ci.ps1` |
| 日志分类 | `LogDreamFX` |
| 作者 | TypeDreamMoon · MIT |
