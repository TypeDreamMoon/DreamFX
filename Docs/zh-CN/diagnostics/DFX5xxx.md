---
title: "DFX5xxx --- 生成与资产写入"
description: "DFX5xxx --- 生成与资产写入"
---

<a id="dfx5xxx-----generation-and-asset-writing"></a>

# DFX5xxx --- 生成与资产写入

> 生成标记之间的块由 `.skill/gen-diagnostics.ps1` 写入。
> 标记以下的所有内容均为手工编写，并在重新生成后保留。

<a id="dfx5001"></a>

## DFX5001

<!-- generated:begin DFX5001 -->
**严重级别** 错误

**消息**

```
Stack '%s' has no Niagara script usage mapping.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1355`
<!-- generated:end DFX5001 -->

**原因。** 一种栈类型背后没有对应的 Niagara 脚本 usage。解析器能产出的每一种栈类型今天都有映射——
六个固定栈、`OnEvent` 和 `Stage`——所以走到这里意味着有人往枚举里加了新的栈类型，
却没有同时扩展 `ScriptUsageForStack`。

**修复。** 使用有映射的那些栈；如果你正在添加一种新的栈类型，请把适配器的这对映射
（`ScriptUsageForStack`、`StackForScriptName`）一起扩展。

<a id="dfx5002"></a>

## DFX5002

<!-- generated:begin DFX5002 -->
**严重级别** 警告

**消息**

```
This system declares no emitters, so it will produce nothing.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2479`
<!-- generated:end DFX5002 -->

**原因。** 没有发射器的系统能编译，但什么都不产生。

**修复。** 添加一个 `Emitter` 块。

<a id="dfx5003"></a>

## DFX5003

<!-- generated:begin DFX5003 -->
**严重级别** 提示

**消息**

```
'%s' is not declared in this source, so its existing modules are left as-is: %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4064`
<!-- generated:end DFX5003 -->

**原因。** 声明一个栈意味着接管它；这份源码从未提及的栈会保留它原有的内容。`CreateNiagaraSystem` 会把一个 `SystemState` 放入 `SystemUpdate`，如果将其整体清除，会让每一个没有显式 `SystemUpdate` 的 `.dfs` 产出一个永远不运行的系统。设为提示级别，是为了让这种差异可见而不是悄无声息。

**修复。** 通常什么都不用做。要接管这个栈就声明它——空的 `SystemUpdate = { }` 会将其清除。

<a id="dfx5004"></a>

## DFX5004

<!-- generated:begin DFX5004 -->
**严重级别** 提示

**消息**

```
No Material was set, so the engine default was applied: %s. Write 'Material = \"...\";' to choose your own.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4323`
<!-- generated:end DFX5004 -->

**原因。** 没有 `Material` 的渲染器会拿到引擎默认值，这也是为什么一个没有贴图的特效仍然能绘制出来。

**修复。** 写入 `Material = "...";` 来选择你自己的。

<a id="dfx5015"></a>

## DFX5015

<!-- generated:begin DFX5015 -->
**严重级别** 错误

**消息**

```
Emitter '%s' has no record in the last build's plan.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4106`
<!-- generated:end DFX5015 -->

**原因。** _尚未编写。_

**修复。** _尚未编写。_

<a id="dfx5020"></a>

## DFX5020

<!-- generated:begin DFX5020 -->
**严重级别** 错误

**消息**

```
The last build of this stack recorded %d module node(s) for %d module(s), so its nodes cannot be addressed.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2919`
<!-- generated:end DFX5020 -->

**原因。** _尚未编写。_

**修复。** _尚未编写。_

<a id="dfx5027"></a>

## DFX5027

<!-- generated:begin DFX5027 -->
**严重级别** 错误

**消息**

```
Could not read native parent defaults: %s.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2255`、`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2334`
<!-- generated:end DFX5027 -->

**原因。** 显式默认值改变了原生父级的参数类型，或者 Niagara 无法应用发射器声明或继承的默认值。

**修复。** 保留继承的参数类型并修正值、绑定或图条件——它们由随附的适配器错误指出——然后重新构建。

<a id="dfx5030"></a>

## DFX5030

<!-- generated:begin DFX5030 -->
**严重级别** 错误

**消息**

```
SavePackage failed for '%s'.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1436`
<!-- generated:end DFX5030 -->

**原因。** 写入包失败——文件只读、源码管理锁定，或者进程无法写入该路径。

**修复。** 检查文件权限；如果它在源码管理下，请先签出。

<a id="dfx5031"></a>

## DFX5031

<!-- generated:begin DFX5031 -->
**严重级别** 错误

**消息**

```
Emitter '%s' declares more than one OnEvent block. Only one event handler per emitter is representable; split the extra handlers into their own emitters.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2404`
<!-- generated:end DFX5031 -->

**原因。** 一个发射器块声明了两个或更多 `OnEvent` 块。

DreamFX 通过外部编辑 API 的普通栈轨道访问事件栈，而那些轨道只能寻址 usage id 为零 guid 的事件脚本——
也就是每个发射器恰好一个可寻址的处理器。第二个处理器会存在于资产上，但对之后的每一次读写都不可见，
所以生成器在前面就拒绝它，而不是构建一个它永远无法往返还原的东西。

**修复。** 每个发射器保留一个 `OnEvent` 块。如果一个发射器确实需要响应两条事件流，
就把它拆成两个发射器、各带一个处理器——栈保持完全相同，两边都针对同一批源事件生成。

<a id="dfx5032"></a>

## DFX5032

<!-- generated:begin DFX5032 -->
**严重级别** 错误

**消息**

```
Emitter '%s' declares two Stage blocks named '%s'. Stages are identified by name; rename one.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2382`
<!-- generated:end DFX5032 -->

**原因。** 一个发射器声明了两个同名的 `Stage` 块。写入侧按名字「创建或复用」stage
（这也是引擎 UI 自己对 stage 的身份认定），所以两个块会落到同一个 stage 上，
而它会静默地只保留最后写入的那个栈。同一段代码也承载构建期由适配器报告的 stage 失败——
一个从未闭合的切片、资产上某个类不是引擎通用 stage 的 stage。

**修复。** 给其中一个块改名。一个发射器内的 stage 按名字标识，所以每个都需要有自己的名字。

<a id="dfx5033"></a>

## DFX5033

<!-- generated:begin DFX5033 -->
**严重级别** 错误

**消息**

```
Emitter '%s' declares Stage blocks but simulates on the CPU. Simulation stages are a GPU feature: set `SimTarget = GPU` in the emitter's Settings.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4495`
<!-- generated:end DFX5033 -->

**原因。** 一个发射器声明了 `Stage` 块，却在 CPU 上模拟。模拟阶段是
GPU 计算特性；CPU 发射器无法运行它，而 Niagara 自己的抱怨会晚一步以一条不指向任何源码行的
编译错误出现。这项检查读取的是活动发射器而不是本文件的
`Settings`，因为一个 `from` 发射器可能是 GPU 的，而 `.dfs` 里并没有写。

**修复。** 在发射器的 `Settings` 块中设置 `SimTarget = GPU`（连同 `FixedBounds`，
GPU 发射器无法自行计算它），或者删除 `Stage` 块。

<a id="dfx5034"></a>

## DFX5034

<!-- generated:begin DFX5034 -->
**严重级别** 错误

**消息**

```
Emitter '%s' uses 'as %s' on two module calls. Node names are unique per emitter; rename one.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2459`
<!-- generated:end DFX5034 -->

**原因。** 一个发射器中的两次模块调用带有相同的 `as <name>` 后缀。这个后缀命名的是函数调用
节点，`Output.<node>.<value>` 链接按这个名字解析，而节点名在一个发射器图内是唯一的——两个占用者
会让所有这类链接解析到遍历时最先遇到的任意一个节点。

**修复。** 给其中一个改名。反编译器只会用原资产的节点名来写出 `as`，
那些名字天然唯一，所以这条只会在手工编辑过的源码上触发。

<a id="dfx5035"></a>

## DFX5035

<!-- generated:begin DFX5035 -->
**严重级别** 错误

**消息**

```
'%s' is still open in the Niagara script editor. The rebuild was cancelled to protect its editable copy. Close the editor, then rebuild again.
```

**触发位置** `Source/DreamFXEditor/Private/UI/DreamFXEditorGeneration.cpp:46`
<!-- generated:end DFX5035 -->

**原因。** 目标 Niagara 脚本编辑器无法关闭，例如其关闭确认被取消了。如果继续重建，
它的可编辑副本可能会覆盖生成的图。

**修复。** 处理编辑器中挂起的更改并关闭它，然后重建。成功关闭的编辑器会在生成完成后重新打开。

<a id="dfx5036"></a>

## DFX5036

<!-- generated:begin DFX5036 -->
**严重级别** 警告

**消息**

```
OnEvent source '%s' is an id no emitter in this system answers to, so this handler never fires. Point it at the emitter that raises the event, or delete the handler.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4404`
<!-- generated:end DFX5036 -->

**原因.** 某个 `OnEvent` 头的 `Source` 是一个本系统中没有任何 emitter 响应的 id，它被写成了反编译器写无法解析的 id 的那种形式：`` `<unresolved GUID>` ``。有两种情况会产生它。把一个 handler 带进复制的系统时，它会保留在原系统中的 id —— 复制时没有任何东西会重新映射它，于是多个系统最终共用一个过期的 id。或者源 emitter 被删掉了，而 handler 留了下来。

该 handler 会带着这个 id 被写回，所以往返是忠实的，但它永远不会触发：运行时会根据 id 的**字符串形式**构建事件处理器查找表，然后按**名字**找到事件集（`NiagaraEmitterInstanceImpl.cpp:418-419`，查找位于 `:1307`），而没有任何 emitter 会响应一个 guid。资产正处于完全相同的状态 —— 这是关于资产的事实，而不是关于文本的。

**修复.** 判断这个系统中哪个 emitter 本来应该触发 `Event`，并在 `Source` 中写出它的名字。源可以出现在文档中接收者之前或之后：handler 是在所有 emitter 都存在之后才添加的。如果已经没有东西再触发那个事件，就删除该 handler 以及它的栈所携带的模块 —— 这会让资产丢掉一些事实，所以必须告诉构建安全门（`-Force`），它的报告会列出丢掉了什么。之后重新构建：警告会一直存在，直到 `Source` 指向一个活着的 emitter。

<a id="dfx5093"></a>

## DFX5093

<!-- generated:begin DFX5093 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'MaterialParam' is reserved syntax and is not implemented in v1 (plan section 7).
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1777`
<!-- generated:end DFX5093 -->

**原因.** `MaterialParam` 是保留语法（L8），v1 没有实现。

**修复.** 用不了。改成在材质实例上设材质参数。

<a id="dfx5097"></a>

## DFX5097

<!-- generated:begin DFX5097 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Only System documents can be generated right now; this file declares a %s.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:5192`
<!-- generated:end DFX5097 -->

**原因.** 只有 `.dfs` 和 `.dfm` 产资产。`.dfe` 按拷贝合并进它的宿主（R3），自身没有可生成的东西。

**修复.** 在某个 `.dfs` 中用 `from` 引用这个 `.dfe`。

<a id="dfx5098"></a>

## DFX5098

<!-- generated:begin DFX5098 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Data interface parameter '%s' takes its configuration as a quoted JSON object, the form the decompiler writes.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2037`
<!-- generated:end DFX5098 -->

**原因.** `DI<T>` user 参数的配置是一个带引号的 JSON 对象 —— 与模块的数据接口输入是同一种逐字形式，也是导出器写出来的形式。这里放别的东西，应用器读不了。配置格式正确但引擎拒绝应用时也会在 DFX5098 下报告，那通常意味着 blob 里的某个属性名在那个接口类上已经不存在了。

**修复.** 把配置写成带引号的 JSON 对象，或者干脆让参数保持裸的 —— 没有值的声明是一个留给运行时填的槽，合法且不会有任何提示。对一个已经把接口按你想要的方式配好的系统跑 `dfx decompile`，它会打印出那个 blob 的确切内容。

> **2026-08-12 变更。** 这条以前是警告，意思是「只声明，v1 不应用默认值」（计划 3.5）。现在配置会被应用。那次范围削减在付出真实的行为代价：`NS_Spawn_Ninja_Root` 上的碰撞源和属性读取器全是 user 数据接口，每一个镜像都把它们 default-construct 了 —— 特效画出来了，但不对任何东西产生交互。

<a id="dfx5100"></a>

## DFX5100

<!-- generated:begin DFX5100 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'%s' is a %s with no generated asset at '%s', and %s
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:834`
<!-- generated:end DFX5100 -->

**原因.** 没有找到以前生成的资产，而这次构建也没有可用的图后端去造一个。这比以前罕见得多：不导出那五个声明的引擎仍然能拿到反射后端，所以走到这里意味着它的启动自检没过，消息里点名的就是哪一项检查。三种结果见 [dfm.md](../language/dfm.md)。

**修复.** 读那条被点名的检查 —— 它说的是后端期待、但没找到的那个引擎形状，那才是真正要修或要上报的东西。在此之前，去一台后端跑得起来的引擎上生成这个模块并提交资产；任何引擎都能正常加载、引用和 cook 它。或者直接绕开这个模块：用内联 `hlsl { }` 表达式，或者一个已有的 dynamic input 资产。

<a id="dfx5101"></a>

## DFX5101

<!-- generated:begin DFX5101 -->
**严重级别：** 错误 (`error`)

**消息：**

```
(built at runtime)
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:716`
<!-- generated:end DFX5101 -->

**原因.** `.dfm` 上的 `Root="..."` 没有指向一个已挂载的内容根。

**修复.** `Root` 取 `Game`、空串或 `Plugin.<PluginName>`。

<a id="dfx5102"></a>

## DFX5102

<!-- generated:begin DFX5102 -->
**严重级别：** 提示 (`info`)

**消息：**

```
Input '%s' is marked [StaticSwitch]. Tier-one generation (plan 3.3) lowers the whole Body to a single custom HLSL node, which has no branch for a switch to select, so it is written as an ordinary input instead. The body reads it the same way; only the compile-time folding is lost.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1068`
<!-- generated:end DFX5102 -->

**原因.** 一级生成（计划 3.3）把整个 body 放进一个 custom HLSL 节点，那里没有给开关选择的分支，所以一个 `[StaticSwitch]` 输入被写成了普通输入。这件事要说出来，因为把一个声明过的编译期开关静默降级成运行时值，正是那种日后以「性能问题但谁也说不清来源」形式浮现的差别。

**修复.** 什么都不用做 —— body 读它的方式完全一样，丢掉的只是编译期折叠。

<a id="dfx5103"></a>

## DFX5103

<!-- generated:begin DFX5103 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Package '%s' exists on disk but could not be loaded.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:746`
<!-- generated:end DFX5103 -->

**原因.** 包在磁盘上存在但加载不了。通常是写了一半的文件，或者被另一个进程占着。

**修复.** 检查那个文件。编辑器开着它的话，关掉资产再重建。

<a id="dfx5104"></a>

## DFX5104

<!-- generated:begin DFX5104 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Package '%s' exists but holds no Niagara script named '%s'. Refusing to overwrite it.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:755`
<!-- generated:end DFX5104 -->

**原因.** 目标包存在，但里面装的不是期待的那个脚本。DreamFX 拒绝覆盖它，而不是替掉一个无关资产。

**修复.** 改 `Name="..."`，或者那个资产确实过时了就删掉它。

<a id="dfx5105"></a>

## DFX5105

<!-- generated:begin DFX5105 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Could not create package '%s'.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1117`
<!-- generated:end DFX5105 -->

**原因.** 包创建不出来 —— 通常是根没挂载，或者名字非法。

**修复.** 检查 `Root` 和 `Name`。

<a id="dfx5106"></a>

## DFX5106

<!-- generated:begin DFX5106 -->
**严重级别：** 错误 (`error`)

**消息：**

```
Could not wire the module graph. The Niagara schema rejected a parameter map connection.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1216`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1241`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1250`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1264`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1341`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1350`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1367`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1379`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1387`
<!-- generated:end DFX5106 -->

**原因.** 建模块图时 Niagara 的 schema 拒绝了一条连接。最常见的原因是输入的声明类型与它要喂的引脚类型不匹配。

**修复.** 检查输入的类型。如果那是 Niagara 没有对应引脚的类型，模块就接不了它。

<a id="dfx5107"></a>

## DFX5107

<!-- generated:begin DFX5107 -->
**严重级别：** 错误 (`error`)

**消息：**

```
'%s' no longer matches the module asset at '%s', and this build cannot regenerate it. Rebuild it where a graph backend runs and commit the updated asset; %s
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:851`
<!-- generated:end DFX5107 -->

**原因.** `.dfm` 源码与它提交进来的资产已经不一致，而这次构建没有图后端可以重新生成。与 DFX5100 分开，是因为补救办法不同：资产是有的，只是过期了。

**修复.** 去一台后端跑得起来的引擎上重建这个模块，并提交更新后的资产。消息后半段点名的是没通过的那项检查 —— 如果你本来预期这台引擎能生成，那就是要修的地方。
