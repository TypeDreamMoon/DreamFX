---
title: DFX6xxx — Niagara 编译
description: DFX6xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 8 条（5 错误 / 2 警告 / 1 提示）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx6001"></a>

## DFX6001

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3210`

**原因.** 一条 Niagara 编译错误，被映射回抛出它的那个模块所在的源码行。

**修复.** 消息是 Niagara 自己的。属性解析不了通常意味着写它的那个模块缺了，或者排在读它的模块**后面**。

<a id="dfx6002"></a>

## DFX6002

**严重级别：** 警告 (`warning`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3244`

**原因.** 一条 Niagara 编译警告，映射回源码。

**修复.** 读消息。来自依赖项的警告会标注出来。

<a id="dfx6003"></a>

## DFX6003

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3285`

**原因.** 错误级别的 Niagara 栈问题 —— 最常见的是未满足的模块依赖。栈问题只有在 Slate 存在的地方才读得到，
所以它们出现在编辑器和语料套件里，headless 构建里看不到（`GetStackIssues` 不是 headless 安全的）。

**修复.** 把依赖点名的那个模块加上。最常见的是 `ScaleSpriteSize` 需要 `UpdateAge`（由 `ParticleState` 提供）——
缺了它 `NormalizedAge` 永远不推进，每一条按寿命驱动的曲线都在零点求值。

<a id="dfx6004"></a>

## DFX6004

**严重级别：** 警告 (`warning`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3289`

**原因.** 警告级别的 Niagara 栈问题。被弃用的模块在这里报。

**修复.** 消息通常会点名替代资产。

<a id="dfx6005"></a>

## DFX6005

**严重级别：** 错误 (`error`)

**消息：**

```text
Niagara compilation of '%s' did not succeed (status %s).
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3374, Generation/DreamFXGenerator.cpp:3724`

**原因.** 系统的编译没有到达成功状态。具体错误以 DFX6001 报在这条之前。

**修复.** 先修上面的错误。状态是 `Dirty` 却没有错误，意味着编译没有跑完。

<a id="dfx6006"></a>

## DFX6006

**严重级别：** 错误 (`error`)

**消息：**

```text
Niagara could not compile the body of '%s':\n%s
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1352`

**原因.** `.dfm` 的 body 降下来之后不是合法 HLSL。后面跟着的是 Niagara 的消息。

**修复.** 读翻译器的错误。`Cannot access field 'X' of structure` 意味着某个带命名空间的引用没解析成功 ——
`Particles.*` 必须是已知属性，或者在 body 里带类型声明过（DFX3046）。

<a id="dfx6007"></a>

## DFX6007

**严重级别：** 提示 (`info`)

**消息：**

```text
Nothing in this source writes that parameter, so there is no graph parameter for a default to sit on and Niagara refuses the read. Check the asset before the effect: the decompiler drops any module input whose value matches a freshly probed module, and a value the author set that happens to equal the default is dropped with them -- NS_Spawn_Ground_Root reads Particles.MySize and builds clean once the suppressed writes are exported, so its text was missing a write its asset had. If the source really is the whole story, write the parameter before whatever reads it -- for an 'Emitter.<Module>.<Output>' name, move the module that produces it ahead of its readers -- or drop the read. Building on an engine with the MoonEngine additions makes the message go away by supplying the type's zero, which is not the same as making the effect work.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3237`

**原因.** 挂在一条 DFX6001「读在写之前」上：**源码文本**里没有任何东西写这个参数，
于是没有链接写入去创建它，也就没有一个图参数让默认值坐上去。

**先查资产，再查特效。** 第一个带着这条消息的资产 `NS_Spawn_Ground_Root` 曾被判为「有缺陷的特效」，
证据是它的导出里没有写入方 —— 而**导出**才是有损的那一步。几个栈之外一个被抑制的静态开关被反编译器丢掉了，
重建于是编译了另一个分支，那次丢失的写入顶着一个毫不相干的名字。资产从头到尾都写了那个参数；
自从开关无条件导出（2026-08-11）以后，它在两个引擎上都编得干干净净。那次裁定作废，
而这个错误的形状变成了一条家规：证明「资产里没有 X」需要**资产级证据**（PkgInfo、反射走查、asset-diff），
永远不能拿导出产物当证据。

**修复.** 先用当前版本重新导出这个资产 —— 消息消失，就说明源码缺的是导出器过去会丢掉的东西，特效本身从来没问题。
如果源码确实就是全部（手写的，或者资产真的没有写入方），那就在源码里修：把这个参数写在读它的东西之前 ——
对 `Emitter.<Module>.<Output>` 这类名字，把产出它的模块挪到读者上面 —— 或者把那次读去掉。
在带 MoonEngine 增补的引擎上构建会让消息消失，因为它补上了类型的零值，**这和让特效正确不是一回事**。

**为什么 DreamFX 不干脆自己建这个参数.** 它做得到：`FGraphSurgeon::AddParameter` 已经为 `.dfm`
那条路用反射建过参数。**有意不接上** —— 在 `.dfs` 路径里用反射建参数会把层依赖倒过来，
并且会在每一个引擎上把真实的源码缺陷糊掉。

**这不是**不信任构建的理由。它是挂在一条错误上的解释，自身从来不是错误。

<a id="dfx6008"></a>

## DFX6008

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' finished its compile with stale scripts: %s. The compiled VM was not rebuilt from the graphs this build wrote, so the asset would simulate something other than what the source says. This is a DreamFX pipeline defect -- report it with this source file.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:3423`

**原因.** 构建跑完了它的 Niagara 编译，随后引擎自己的同步性测试说：某个脚本存着的 VM **不是**
照这次构建留下的图编出来的。在构建强制最后一次编译的前提下这本该不可能，这正是它是错误的原因：
要么有写入路径在编译发起之后改了图，要么那次强制在什么地方丢了。
这项检查存在，是因为这类资产真的出过货 —— `NS_Spawn_Teleport_Root` 的 `NE_C` 顶着正确图的 compile id，
背着一份「所有开关都是 Unset」时代的字节码，把错误的分支模拟了好几天，而每一项文本级检查都与源码一致（2026-08-11）。

**修复.** 不在源码里 —— 这条消息点名的是 DreamFX 的管线缺陷。先带 `-Force` 重建一次；还复现的话，
连同源文件和被点名的脚本一起上报。那份 stale 脚本清单会告诉你该看哪个 emitter、哪个阶段。
