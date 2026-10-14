---
title: DFX5xxx — 生成与资产写入
description: DFX5xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 21 条（16 错误 / 1 警告 / 4 提示）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx5001"></a>

## DFX5001

**严重级别：** 错误 (`error`)

**消息：**

```text
Stack '%s' has no Niagara script usage mapping.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1095`

**原因.** 一种栈类型背后没有对应的 Niagara 脚本 usage。解析器能产出的每一种今天都有映射 ——
六个固定栈、`OnEvent` 和 `Stage` —— 所以走到这里意味着有人往枚举里加了新栈类型，却没有同时扩展
`ScriptUsageForStack`。

**修复.** 用有映射的那些栈；如果你正在加一种，把 adapter 的这对映射（`ScriptUsageForStack`、
`StackForScriptName`）一起扩展。

<a id="dfx5002"></a>

## DFX5002

**严重级别：** 警告 (`warning`)

**消息：**

```text
This system declares no emitters, so it will produce nothing.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2104`

**原因.** 没有 emitter 的系统能编译，但什么都不产生。

**修复.** 加一个 `Emitter` 块。

<a id="dfx5003"></a>

## DFX5003

**严重级别：** 提示 (`info`)

**消息：**

```text
'%s' is not declared in this source, so its existing modules are left as-is: %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2849`

**原因.** 声明一个栈意味着接管它；这份源码从没提过的栈保留它原有的内容。
`CreateNiagaraSystem` 会往 `SystemUpdate` 里放一个 `SystemState`，整个清掉的话，
每一个不写 `SystemUpdate` 的 `.dfs` 都会产出一个永远不运行的系统。这条是提示级的，
目的是让这个差别看得见而不是悄无声息。

**修复.** 通常什么都不用做。要接管这个栈就声明它 —— 空的 `SystemUpdate = { }` 会把它清掉。

<a id="dfx5004"></a>

## DFX5004

**严重级别：** 提示 (`info`)

**消息：**

```text
No Material was set, so the engine default was applied: %s. Write 'Material = \"...\";' to choose your own.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2962`

**原因.** 没写 `Material` 的 renderer 会拿到引擎默认材质，这也是为什么一个没贴图的特效还是画得出来。

**修复.** 写 `Material = "...";` 选你自己的。

<a id="dfx5030"></a>

## DFX5030

**严重级别：** 错误 (`error`)

**消息：**

```text
SavePackage failed for '%s'.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1391`

**原因.** 写包失败 —— 只读文件、源码管理锁定，或者进程写不了那个路径。

**修复.** 检查文件权限；在源码管理下的话先 check out。

<a id="dfx5031"></a>

## DFX5031

**严重级别：** 错误 (`error`)

**消息：**

```text
Emitter '%s' declares more than one OnEvent block. Only one event handler per emitter is representable; split the extra handlers into their own emitters.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2035`

**原因.** 一个 emitter 块里声明了两个或更多 `OnEvent` 块。

DreamFX 通过外部编辑 API 的普通栈轨道去够事件栈，而那套轨道只能寻址 usage id 为零 guid 的事件脚本 ——
也就是每个 emitter 恰好一个可寻址的处理器。第二个处理器会存在于资产上，但对之后每一次读写都不可见，
所以生成器在前面就拒绝，而不是建一个它永远往返不回来的东西。

**修复.** 每个 emitter 保留一个 `OnEvent`。真的需要响应两条事件流，就拆成两个 emitter、各带一个处理器 ——
栈可以完全一样，两边都对着同一批源事件生成。

<a id="dfx5032"></a>

## DFX5032

**严重级别：** 错误 (`error`)

**消息：**

```text
Emitter '%s' declares two Stage blocks named '%s'. Stages are identified by name; rename one.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2013`

**原因.** 一个 emitter 里声明了两个同名的 `Stage` 块。写入侧按名字「创建或复用」stage（这也是引擎 UI 自己
对 stage 的身份认定），于是两个块会落到同一个 stage 上，而它会静默地只留下最后写入的那个栈。
同一段代码也承载构建期由 adapter 报出的 stage 失败 —— 没闭合的切片、资产上某个类不是引擎通用 stage 的 stage。

**修复.** 给其中一个改名。一个 emitter 内的 stage 按名字标识，每个都得有自己的名字。

<a id="dfx5033"></a>

## DFX5033

**严重级别：** 错误 (`error`)

**消息：**

```text
Emitter '%s' declares Stage blocks but simulates on the CPU. Simulation stages are a GPU feature: set `SimTarget = GPU` in the emitter's Settings.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3091`

**原因.** 一个 emitter 声明了 `Stage` 块，却在 CPU 上模拟。simulation stage 是 GPU compute 特性，
CPU emitter 跑不了；Niagara 自己的抱怨会晚一步以一条不指向任何源码行的编译错误出现。
这项检查读的是**活 emitter** 而不是本文件的 `Settings`，因为一个 `from` 来的 emitter 可能本来就是 GPU，
而 `.dfs` 里并没有写。

**修复.** 在 emitter 的 `Settings` 里设 `SimTarget = GPU`（并配一个 `FixedBounds`，GPU emitter 算不出自己的 bounds），
或者把 `Stage` 块删掉。

<a id="dfx5034"></a>

## DFX5034

**严重级别：** 错误 (`error`)

**消息：**

```text
Emitter '%s' uses 'as %s' on two module calls. Node names are unique per emitter; rename one.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:2087`

**原因.** 一个 emitter 里两次模块调用带了相同的 `as <name>` 后缀。这个后缀命名的是函数调用**节点**，
`Output.<node>.<value>` 链接按这个名字解析，而节点名在一个 emitter 图内唯一 ——
两个claimant 会让所有这类链接解析到遍历时先撞见的那个节点上。

**修复.** 改掉一个。反编译器只会用原资产的节点名来写 `as`，那些名字天然唯一，所以这条只会在手改过的源码上触发。

<a id="dfx5093"></a>

## DFX5093

**严重级别：** 错误 (`error`)

**消息：**

```text
'MaterialParam' is reserved syntax and is not implemented in v1 (plan section 7).
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1513`

**原因.** `MaterialParam` 是保留语法（L8），v1 没有实现。

**修复.** 用不了。改成在材质实例上设材质参数。

<a id="dfx5097"></a>

## DFX5097

**严重级别：** 错误 (`error`)

**消息：**

```text
Only System documents can be generated right now; this file declares a %s.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3511`

**原因.** 只有 `.dfs` 和 `.dfm` 产资产。`.dfe` 是按拷贝合并进宿主的，自身没有可生成的东西。

**修复.** 在某个 `.dfs` 里用 `from` 引用这个 `.dfe`。

<a id="dfx5098"></a>

## DFX5098

**严重级别：** 错误 (`error`)

**消息：**

```text
Data interface parameter '%s' takes its configuration as a quoted JSON object, the form the decompiler writes.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1778`

**原因.** `DI<T>` user 参数的配置是一个**带引号的 JSON 对象** —— 与模块的数据接口输入是同一种逐字形式，
也是导出器写出来的形式。这里放别的东西，应用器读不了。配置格式正确但引擎拒绝应用时也报这个码，
那通常意味着 blob 里的某个属性名在那个接口类上已经不存在了。

**修复.** 把配置写成带引号的 JSON 对象，或者干脆让参数保持裸的 —— 没有值的声明是一个留给运行时填的槽，
合法且不会有任何提示。对一个已经把接口配好的系统跑 `dfx decompile`，它会打印出那个 blob 的确切内容。

> **2026-08-12 变更。** 这条以前是警告，意思是「只声明，v1 不应用默认值」。现在配置会被应用。
> 那次范围削减在付出真实的行为代价：`NS_Spawn_Ninja_Root` 上的碰撞源和属性读取器全是 user 数据接口，
> 每一个镜像都把它们 default-construct 了 —— 特效画出来了，交互没有了。

<a id="dfx5100"></a>

## DFX5100

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is a %s with no generated asset at '%s', and %s
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:799`

**原因.** 没有找到以前生成的资产，而这次构建也没有可用的图后端去造一个。这比以前罕见得多：
不导出那五个声明的引擎仍然能拿到反射后端，所以走到这里意味着它的启动自检没过，
消息里点名的就是**哪一项**检查。三种结果见 [dfm.md](../language/dfm.md)。

**修复.** 读那条被点名的检查 —— 它说的是后端期待、但没找到的那个引擎形状，那才是真正要修或要上报的东西。
在此之前，去一台后端跑得起来的引擎上生成这个模块并提交资产；任何引擎都能正常加载、引用和 cook 它。
或者直接绕开这个模块：用内联 `hlsl { }` 表达式，或者一个已有的 dynamic input 资产。

<a id="dfx5101"></a>

## DFX5101

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:681`

**原因.** `.dfm` 上的 `Root="..."` 没有指向一个已挂载的内容根。

**修复.** `Root` 取 `Game`、空串或 `Plugin.<PluginName>`。

<a id="dfx5102"></a>

## DFX5102

**严重级别：** 提示 (`info`)

**消息：**

```text
Input '%s' is marked [StaticSwitch]. Tier-one generation (plan 3.3) lowers the whole Body to a single custom HLSL node, which has no branch for a switch to select, so it is written as an ordinary input instead. The body reads it the same way; only the compile-time folding is lost.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1028`

**原因.** 一级生成把整个 body 放进一个 custom HLSL 节点，那里没有给开关选择的分支，
所以 `[StaticSwitch]` 输入被写成普通输入。这件事要说出来，因为把一个声明过的编译期开关静默降级成运行时值，
正是那种日后以「性能问题但谁也说不清来源」形式浮现的差别。

**修复.** 什么都不用做 —— body 读它的方式完全一样，丢掉的只是编译期折叠。

<a id="dfx5103"></a>

## DFX5103

**严重级别：** 错误 (`error`)

**消息：**

```text
Package '%s' exists on disk but could not be loaded.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:711`

**原因.** 包在磁盘上存在但加载不了。通常是写了一半的文件，或者被另一个进程占着。

**修复.** 检查那个文件。编辑器开着它的话，关掉资产再重建。

<a id="dfx5104"></a>

## DFX5104

**严重级别：** 错误 (`error`)

**消息：**

```text
Package '%s' exists but holds no Niagara script named '%s'. Refusing to overwrite it.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:720`

**原因.** 目标包存在，但里面装的不是期待的那个脚本。DreamFX 拒绝覆盖它，而不是替掉一个无关资产。

**修复.** 改 `Name="..."`，或者那个资产确实过时了就删掉它。

<a id="dfx5105"></a>

## DFX5105

**严重级别：** 错误 (`error`)

**消息：**

```text
Could not create package '%s'.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1076`

**原因.** 包创建不出来 —— 通常是根没挂载，或者名字非法。

**修复.** 检查 `Root` 和 `Name`。

<a id="dfx5106"></a>

## DFX5106

**严重级别：** 错误 (`error`)

**消息：**

```text
Could not wire the module graph. The Niagara schema rejected a parameter map connection.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1166, Generation/DreamFXModuleGenerator.cpp:1191, Generation/DreamFXModuleGenerator.cpp:1200, Generation/DreamFXModuleGenerator.cpp:1214, Generation/DreamFXModuleGenerator.cpp:1291, Generation/DreamFXModuleGenerator.cpp:1300, Generation/DreamFXModuleGenerator.cpp:1317, Generation/DreamFXModuleGenerator.cpp:1329`

**原因.** 建模块图时 Niagara 的 schema 拒绝了一条连接。最常见的原因是输入的声明类型与它要喂的引脚类型不匹配。

**修复.** 检查输入的类型。如果那是 Niagara 没有对应引脚的类型，模块就接不了它。

<a id="dfx5107"></a>

## DFX5107

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' no longer matches the module asset at '%s', and this build cannot regenerate it. Rebuild it where a graph backend runs and commit the updated asset; %s
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:816`

**原因.** `.dfm` 源码与它提交进来的资产已经不一致，而这次构建没有图后端可以重新生成。
与 DFX5100 分开，是因为补救办法不同：资产是有的，只是过期了。

**修复.** 去一台后端跑得起来的引擎上重建这个模块，并提交更新后的资产。消息后半段点名的是没通过的那项检查 ——
如果你本来预期这台引擎能生成，那就是要修的地方。
