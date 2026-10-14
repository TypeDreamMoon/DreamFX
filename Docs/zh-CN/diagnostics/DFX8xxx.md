---
title: "DFX8xxx —— 反编译器"
description: "DFX8xxx —— 反编译器"
---

<a id="dfx8xxx-----decompiler"></a>

# DFX8xxx —— 反编译器

> 生成标记之间的内容由 `.skill/gen-diagnostics.ps1` 写入。
> 标记以下的全部内容为手写，重新生成后依然保留。

<a id="dfx8000"></a>

## DFX8000

<!-- generated:begin DFX8000 -->
**严重级别：** 错误

**消息：**

```
Cannot decompile a null system.
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2316`
<!-- generated:end DFX8000 -->

**原因.** 资产路径解析不到任何东西，或者解析到的不是一个 Niagara System。

**修复.** 检查路径。`dfx decompile /Game/FX/NS_Spark` 接收的是包路径。

<a id="dfx8001"></a>

## DFX8001

<!-- generated:begin DFX8001 -->
**严重级别：** 错误

**消息：**

```
Could not read emitters: %s
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2513`
<!-- generated:end DFX8001 -->

**原因.** 该系统的发射器读不出来。

**修复.** 内层消息是 adapter 的。系统整个加载不了的情况报的是 DFX8000。

<a id="dfx8002"></a>

## DFX8002

<!-- generated:begin DFX8002 -->
**严重级别：** 警告

**消息：**

```
Skipping emitter '%s': %s
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2542`
<!-- generated:end DFX8002 -->

**原因.** 某一个发射器导不出来，系统的其余部分仍然导出了。

**修复.** 那个发射器的导出是不完整的。`dfx coverage` 会在所有系统上一次性给出同样的信息。

<a id="dfx8003"></a>

## DFX8003

<!-- generated:begin DFX8003 -->
**严重级别：** 错误

**消息：**

```
Cannot decompile a null emitter.
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2687`
<!-- generated:end DFX8003 -->

**原因.** 触发 Export .dfe 入口时什么都没选中，或者选中的资产加载失败。

**修复.** 右键单个 `UNiagaraEmitter` 资产。存在于系统内部的发射器不是独立资产 —— 那种情况应改为导出整个系统。

<a id="dfx8004"></a>

## DFX8004

<!-- generated:begin DFX8004 -->
**严重级别：** 错误

**消息：**

```
Could not create a host system to read the emitter through: %s
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2699`
<!-- generated:end DFX8004 -->

**原因.** 读取发射器需要一个拥有它的系统：Niagara 外部编辑 API 中的每个读取器都通过系统来寻址。`/Temp/DreamFX` 下那个一次性宿主没能创建出来。

**修复.** 内层消息是 adapter 的。`/Temp` 下的任何内容都不会写入磁盘，所以这是一次内存内的失败 —— 通常是 Niagara 插件未能初始化，而不是这个发射器有什么问题。

<a id="dfx8005"></a>

## DFX8005

<!-- generated:begin DFX8005 -->
**严重级别：** 错误

**消息：**

```
Could not copy emitter '%s' into a host system: %s
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2726`
<!-- generated:end DFX8005 -->

**原因.** 发射器无法拷贝进宿主系统。Niagara 的 `AddEmitter` 把它作为模板拒绝了。

**修复.** 内层消息是 Niagara 的。最常见的原因是这个发射器资产是由老得多的引擎版本保存的；在 Niagara 编辑器里打开并重新保存即可升级它。

<a id="dfx8006"></a>

## DFX8006

<!-- generated:begin DFX8006 -->
**严重级别：** 错误

**消息：**

```
Could not read emitter '%s': %s
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2747`
<!-- generated:end DFX8006 -->

**原因.** 发射器已拷贝进宿主，但它的拓扑读不回来。

**修复.** 内层消息是 adapter 的。`dfx coverage` 会在所有系统上跑同一条读取路径，是判断「问题出在这个发射器还是读取路径本身」最快的办法。

<a id="dfx8010"></a>

## DFX8010

<!-- generated:begin DFX8010 -->
**严重级别：** 错误

**消息：**

```
'%s' cannot be adopted: '%s' has no DreamFXLang form yet, so adopting would destroy it on the first rebuild. Export .dfs instead.
```

**触发位置：** `Source/DreamFXEditor/Private/UI/DreamFXAssetCommands.cpp:540`
<!-- generated:end DFX8010 -->

**原因.** **Adopt** 会拒绝带有 DreamFXLang 表达不了的特性的资产。每一项特性报一行。

这道门是 adopt 之所以安全的原因。adopt 意味着文本成为该资产唯一的真相来源，所以带着已知缺口去 adopt，会在第一次重建时把没能存活下来的东西销毁掉 —— 静默地销毁，而且没有任何 diff 可看。

**修复.** 改用 **Export .dfs**。它写出同样的文本、不碰资产，并把缺口列在文件头里。想知道自己内容里哪些桶是敞开的，跑 `dfx.ps1 coverage`。

<a id="dfx8011"></a>

## DFX8011

<!-- generated:begin DFX8011 -->
**严重级别：** 错误

**消息：**

```
Cannot adopt '%s': source destination or another source already exists at '%s'. Edit that file instead; Adopt never replaces existing source.
```

**触发位置：** `Source/DreamFXEditor/Private/UI/DreamFXAssetCommands.cpp:456`
<!-- generated:end DFX8011 -->

**原因.** 已经有另一个 `.dfs` 声明了同一个目标资产。两份源码生成同一个资产会轮流互相覆盖，谁后构建谁赢。

两边在构建期都发现不了这件事 —— 两边都会成功。唯一能拦住它的地方就是这里，在第二份存在之前。

**修复.** 去改那份已有的源码。它的路径在消息里，也在 toast 里。

<a id="dfx8012"></a>

## DFX8012

<!-- generated:begin DFX8012 -->
**严重级别：** 错误

**消息：**

```
'%s' was adopted, but re-exporting the rebuilt asset does not reproduce this file. First difference at %s
```

**触发位置：** `Source/DreamFXEditor/Private/UI/DreamFXAssetCommands.cpp:636`
<!-- generated:end DFX8012 -->

**原因.** 资产被 adopt 并重建了，但把重建后的资产再导出一次，得不到刚写下的那份文件。第一处差异行在消息里。

因为 DFX8010 已经排除了每一个**已知**缺口，所以这里的不匹配是生成器或反编译器的真实缺陷，而不是预期内的丢失。

**修复.** 源文件留在磁盘上，资产也是照它重建的，所以无论哪种情况文本都是权威的那一份 —— 什么都没丢。请把差异行报上来：往返不是一个不动点，正是 `DreamFX.Corpus.RoundTrip` 存在要防的事，所以走到这里的案例是语料尚未覆盖到的那一类。

<a id="dfx8013"></a>

## DFX8013

<!-- generated:begin DFX8013 -->
**严重级别：** 错误

**消息：**

```
This file sits in the decompiled output directory but Name=\"%s\" builds '%s', outside the '%s/' namespace. That would overwrite the asset it was exported from. Re-export it, or move the file out of the decompiled tree to keep this name.
```

**触发位置：** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4757`
<!-- generated:end DFX8013 -->

**原因.** 这个文件住在 *Decompiled Output Directory*（默认 `DFX/Decompiled`）里，但它的
`Name=` 解析出的资产位于 `Decompiled/` 内容命名空间之外。构建它会盖掉
它当初被导出的那个资产 —— 而那通常是作者只打算读一读的第三方内容。

几乎每一次都是插件开始给导出「换家」之前写下的导出：那些文件命名的是
原资产，当时是靠把整棵反编译树排除在构建之外来兜底的。现在这棵树是
普通源码，所以这项检查搬到了文件自己身上。

**修复.** 重新导出该资产（右键 > *Export .dfs*，或 `dfx decompile-all -Path=...`）。新文件
命名 `Decompiled/<original directory>/<asset>`，在原件旁边重建一个镜像 —— 这正是它可以安全地
编辑和保存的原因。

如果你的本意确实是让这段文本成为该资产的真相来源，那件事叫 *Adopt* 而不是 *Export*：
把文件从反编译树里挪进某个 `DFX/` 目录，名字保持不变。Adopt 写出的
正是这个安排，并且在导出会丢掉语言还表达不了的东西时拒绝执行（DFX8010）。

<a id="dfx8014"></a>

## DFX8014

<!-- generated:begin DFX8014 -->
**严重级别：** 警告

**消息：**

```
Emitter '%s' inherits from '%s'. The export flattens the inheritance: the merged stack is carried in full, but the rebuilt emitter no longer follows the parent, so later parent edits will change the original and not the mirror.
```

**触发位置：** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1077`、`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1676`、`Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2173`
<!-- generated:end DFX8014 -->

**原因.** 导出无法保留发射器原生继承的每一个方面。系统 `.dfs` 导出现在会保留 `inherits "<parent asset>" version "<GUID>"`，并省略与所选父版本完全相同的受支持内容。这条警告指出一项仍然存在的局限：被改动的堆栈或渲染器组会变成完整覆盖；事件处理器与 stage 是快照；继承来的默认值、处理器或整个渲染器/stage 组无法在源码中移除；或者父关联、版本或基线无法可靠读出。它还会报告父资产不再是可继承的独立资产、无效的父链，以及父或子资产上未合并的改动。独立 `.dfe` 导出仍然会把父关系摊平成快照。

**修复.** 在采纳导出之前，先阅读具体消息和覆盖缺口。将所属系统导出为 `.dfs` 以保留原生父级链接。在 Niagara 中合并并保存待处理的父级/祖先更改，合并子级，然后重新导出；先修复无效的父级资产或版本。对于有意的整块覆盖，请记住警告中的继承限制：之后对该块内父级的编辑不会在重建时传播。对于没有源拼写的删除，应保留编辑器编写或更改父级/源设计，而不是将快照视为完整的往返。有关支持的覆盖规则，请参阅[原生发射器继承](../language/native-emitter-inheritance.md)。

<a id="dfx8015"></a>

## DFX8015

<!-- generated:begin DFX8015 -->
**严重级别** 警告

**消息**

```
Emitter '%s' carries %d event handler(s) this export cannot represent%s. The rebuilt emitter will receive no events -- an event-spawned emitter comes back permanently empty. The gap header names each handler's source emitter and event.
```

**触发位置** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1125`
<!-- generated:end DFX8015 -->

**原因.** 该发射器携带导出没有文本形式的事件处理器。事件处理器本身**是支持的** —— 它们导出为 `OnEvent(...)` 块 —— 所以现在仅在两种更窄的情况下触发：

- 该发射器有**不止一个**处理器（每个发射器只有一个 `OnEvent` 块可表达，因为写入侧通过单一的零 usage id 到达事件栈）；
- 导出的是**独立的 `.dfe`**，它没有兄弟发射器可供处理器的 `Source` 命名（存储的 `SourceEmitterID` 是一个任何重建都无法复现的 handle guid，所以来源按发射器**名称**传递）。

**修复.** 对于多处理器的情况，拆分接收方发射器，使每个只携带一个处理器。对于独立的情况，改为将整个系统导出为 `.dfs`，这样源发射器存在、可以被命名。两种情况下缺口头都会写明每个处理器的源发射器与事件，所以没有东西会静默丢失。

<a id="dfx8016"></a>

## DFX8016

<!-- generated:begin DFX8016 -->
**严重级别** 警告

**消息**

```
Emitter '%s' carries %d simulation stage(s) this export cannot represent (custom stage class or missing script). The gap header names each one.
```

**触发位置** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:1172`
<!-- generated:end DFX8016 -->

**原因.** 该发射器携带导出没有文本形式的模拟阶段。普通阶段导出为 `Stage name(...)` 块，不会触发这条；剩下的是那些类不是引擎 `UNiagaraSimulationStageGeneric` 的阶段（自定义 C++ 阶段），或者脚本对象在资产上缺失的阶段。缺口头会逐个点名，于是这次丢失会一路留到代码审查，而不是只活在一份 commandlet 日志里。

**修复.** 源文件里没什么可修的 —— 这是表达能力的边界。如果这个阶段很重要，那就继续在 Niagara 编辑器里编写那个发射器；重建出来的镜像不会运行它。

<a id="dfx8017"></a>

## DFX8017

<!-- generated:begin DFX8017 -->
**严重级别** 警告

**消息**

```
emitter '%s' uses unsupported Lightweight/Stateless mode. Keep this emitter in Niagara; DreamFX cannot rebuild its stateless modules and renderers.
```

**触发位置** `Source/DreamFXEditor/Private/Decompiler/DreamFXDecompiler.cpp:2529`
<!-- generated:end DFX8017 -->

**原因.** 某发射器句柄使用了 Niagara 的 Lightweight/Stateless 模式。DreamFX 目前还无法表达其无状态模块和渲染器。导出会记录覆盖缺口并省略该发射器，而不是将一个空的标准发射器当作完整导出呈现。采纳会在重建资产之前拒绝带有此缺口的导出。

**修复.** 继续在 Niagara 中编写此发射器。部分导出对于检查系统中受支持的部分仍然有用，但它不是一次完整的迁移。只有在适合该效果时，才在 Niagara 中将该发射器转换为受支持的标准发射器，然后再次导出。

<a id="dfx8018"></a>

## DFX8018

<!-- generated:begin DFX8018 -->
**严重级别** 错误

**消息**

```
'%s' was not saved: rebuilding it would drop %d fact(s) this source cannot express, and a save would destroy them. They are listed above, and the asset still holds every one of them. -Force writes anyway; full lists in %s
```

**触发位置** `Source/DreamFXEditor/Private/Diff/DreamFXBuildSafetyGate.cpp:543`, `Source/DreamFXEditor/Private/Diff/DreamFXBuildSafetyGate.cpp:549`, `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:4963`
<!-- generated:end DFX8018 -->

**原因.** 重建会让资产持有的内容少于现在，因此保存被拒绝。每一条会丢失的事实会报一行，然后这一条作为汇总；事实列表本身写入 `Saved/DreamFX/BuildSafety/<asset>.{before,after,lost}.facts`，因为控制台会截断长事实，而且在一次 `-Force` 保存之后，资产根本不再持有 `before` 状态。

构建会把文本重放到资产上，而文本并不是 Niagara 系统的完整描述。反编译器抑制的输入（R8：只有与原始模块不同的输入才会被打印）、commandlet 在最后一次构建后补入的渲染器绑定、没有任何设置表携带的属性 —— 重建会静默地替换掉这些所持有的内容。在本项目自身内容上实测：一个资产上有 41 个存储的模块输入常量，另一个上 `Sprite_Atlas_Size.MeshYaw` -90 -> 0，第三个上有 61 个。它们每一个的 L1 和 L2 都是绿的，因为 L1 比较的两侧都是同一个有损导出器的输出。

这道门与 DFX8010 的区别在于它读取的位置。DFX8010 使用导出器自己无法携带的特性列表来拒绝一次*采纳*，而上面三个资产没有一个出现在该列表中。这里是通过反射将资产与自身比较 —— `DescribeSystemFacts`，在构建的第一次写入之前读取，并在保存之前立即再次读取 —— 所以导出器在两侧造成的丢失正是它能够看到的东西。系统自身的属性也是该比较的一部分，而在这道门存在之前它们不是。

**修复.** 阅读上面的行：每一行都点出一个地址、它当前持有的值，以及重建将改为写入的值，或者当重建根本没有对应值时显示 `(missing)`。然后选择一项：

- 教会源文件携带它。被抑制的输入通常可以直接写出；渲染器的材质参数绑定会以反编译器输出的 `MaterialParameters = "{...}"` JSON 形式传递。
- 继续在编辑器中编写该资产，并用(*导出 .dfs*)而不是采纳它。采纳会让文本成为唯一的真相来源，而这道门正是把这一点从副作用变成决定的东西。
- `-Force` 仍然写入。同一列表会以警告严重级别记录，因此强制构建之后仍然可问责；拒绝从来不是静默的。

被拒绝的构建既不会写入资产，也不会写入其来源标记，所以下一次普通构建不会因为是最新而被跳过。在编辑器会话中，被拒绝的包还会从磁盘重新加载：重建已经被应用到内存中的资产上，若将其留在那里，*全部保存*、自动保存或打开的 Niagara 编辑器就会写入正是这道门所拒绝的内容。在命令行上，进程结束，不保留任何内容。

什么都不丢失的重建不会记录任何内容，而尚无资产的目标永远不会被拒绝：第一次生成没有更早的内容可替换。
