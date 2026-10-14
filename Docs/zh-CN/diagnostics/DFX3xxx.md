---
title: DFX3xxx — 声明与文档结构
description: DFX3xxx 段的全部诊断码，每条带严重级别、逐字消息、成因与修法。
---

本族共 31 条（31 错误）。消息是编译器持有的格式串：`%s`、`%d`、`%c` 在运行时被替换掉。

<a id="dfx3000"></a>

## DFX3000

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1696, Generation/DreamFXGenerator.cpp:1707`

**原因.** `Root="..."` 没有指向一个已挂载的内容根，或者 `Name="..."` 最后一个斜杠后面没有资产名。

**修复.** `Root` 取 `Game`、空串或 `Plugin.<PluginName>`。`Name` 是一条以资产名结尾的路径，例如 `Systems/NS_Spark`。

<a id="dfx3001"></a>

## DFX3001

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1257`

**原因.** 模块名在搜索路径上解析不到任何东西。这与 DFX3003 是两回事 —— 那个意思是模块在、但没有这个输入。
路径打错和实参打错分开报，否则它们读起来一模一样。

**修复.** 把目录加进 `Settings.ModulePaths`，或者写完整资产路径。`dfx list` 会打印当前路径上的每一个模块。

<a id="dfx3002"></a>

## DFX3002

**严重级别：** 错误 (`error`)

**消息：**

```text
Could not read the input schema of module '%s': %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1298, Generation/DreamFXGenerator.cpp:1351`

**原因.** 模块找到了，但它的输入签名读不出来。schema 是靠把模块加进一个临时系统探测的，所以这通常意味着这个模块不能待在你写它的那个栈里。

**修复.** 检查栈。`dfx schema <Module> -Stack <Stack>` 会复现这趟探测。

<a id="dfx3003"></a>

## DFX3003

**严重级别：** 错误 (`error`)

**消息：**

```text
Module '%s' has no input named '%s'.%s Available inputs: %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1415`

**原因.** 这个模块没有叫这个名字的输入。Niagara 的输入名带空格（`Loop Duration`）；DreamFX 两侧都归一化，
所以 `LoopDuration` 能匹配上，但拼错的匹配不上。

**修复.** 用列出来的名字之一。差一点的会给出建议。`dfx schema <Module>` 打印完整签名，包括藏在静态开关后面的输入。

<a id="dfx3004"></a>

## DFX3004

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, LightRenderer, DecalRenderer, ComponentRenderer, VolumeRenderer, or any UNiagaraRendererProperties subclass.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1478`

**原因.** renderer 类型是一个闭集。

**修复.** 用列出来的类型之一。renderer 的**属性**是 schema 驱动、开放的（L8），固定的只有类型关键字。

<a id="dfx3005"></a>

## DFX3005

**严重级别：** 错误 (`error`)

**消息：**

```text
Emitter '%s' is declared more than once. Emitter names are stable keys and must be unique.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1848`

**原因.** 两个 emitter 同名。名字是重建契约用来匹配 handle 的稳定键，所以重名是数据丢失，不是命名洁癖。

**修复.** 改掉一个。要给一个已经存在于资产里的 emitter 改名，先用 `dfx rename`，这样 handle 和它的快速迭代参数才能活下来。

<a id="dfx3006"></a>

## DFX3006

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is neither an allowed inline function (%s) nor a dynamic input: %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:923`

**原因.** 值位置上的一个调用，既不在 L6 的内置白名单里，也不对应任何 dynamic input 资产。

**修复.** 用列出来的内置函数、引用一个真实的 dynamic input，或者把这段数学写进 `hlsl { }` 块。
放宽白名单是设计决定（L6），不是配置项。

<a id="dfx3007"></a>

## DFX3007

**严重级别：** 错误 (`error`)

**消息：**

```text
Could not read the input schema of dynamic input '%s': %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:965`

**原因.** dynamic input 资产找到了，但签名读不出来。

**修复.** `dfx schema <Name>` 会对同一个资产复现这次读取。

<a id="dfx3008"></a>

## DFX3008

**严重级别：** 错误 (`error`)

**消息：**

```text
Dynamic input '%s' has no input named '%s'. Available inputs: %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1017`

**原因.** 这个 dynamic input 没有叫这个名字的输入 —— 也就是链条下一层的 DFX3003。

**修复.** 用列出来的名字之一。

<a id="dfx3009"></a>

## DFX3009

**严重级别：** 错误 (`error`)

**消息：**

```text
Dynamic input '%s' is pinned to version %s, which its asset does not offer. Available version(s): %s.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1280, Generation/DreamFXGenerator.cpp:948`

**原因.** 一个 R7 `@version` 固定与模块资产暴露的版本对不上，或者固定到了一个从未启用版本化的资产上。
这个固定记录的是源码是照哪个版本写的；DreamFX 建不出别的版本，因为外部编辑 API 没有选版本的办法。

**修复.** 照暴露出来的版本重测并更新固定值，或者把资产上的那个版本恢复回来；模块本来就没有版本化的话，把 `@` 去掉。

<a id="dfx3010"></a>

## DFX3010

**严重级别：** 错误 (`error`)

**消息：**

```text
Property '%s': %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1792, Generation/DreamFXGenerator.cpp:306, Generation/DreamFXGenerator.cpp:893`

**原因.** 某条 `Properties` 声明的形状不对 —— 看内层消息。

**修复.** 修那条声明。可用类型列在 DFX4021 的消息里。

<a id="dfx3020"></a>

## DFX3020

**严重级别：** 错误 (`error`)

**消息：**

```text
Unknown %s setting '%s'. Available settings: %s
```

**触发位置：** `Generation/DreamFXGenerator.cpp:563`

**原因.** 一个不存在的设置键。不检查的话拼错就是什么也不做，于是效果直接是错的，而且没有任何线索。

**修复.** 用列出来的名字之一 —— 它们是从活资产上读出来的，所以是真实的那一批。

<a id="dfx3021"></a>

## DFX3021

**严重级别：** 错误 (`error`)

**消息：**

```text
User parameter '%s' is declared more than once.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1739`

**原因.** 两个 user 参数同名；蓝图的 `SetNiagaraVariable` 会打到赢的那一个上。

**修复.** 改掉一个。

<a id="dfx3030"></a>

## DFX3030

**严重级别：** 错误 (`error`)

**消息：**

```text
A Module or DynamicInput must declare Settings.Usage -- it decides which stacks the module can be placed in.
```

**触发位置：** `Lint/DreamFXLint.cpp:178`

**原因.** `Usage` 决定一个模块能被放进哪些栈。没有它，模块存在，但从任何栈都够不着。

**修复.** 加上 `Usage = ParticleUpdate;`（或别的栈）。数组可以选多个：`Usage = [ParticleSpawn, ParticleUpdate];`

<a id="dfx3031"></a>

## DFX3031

**严重级别：** 错误 (`error`)

**消息：**

```text
A DynamicInput must declare Settings.Output -- its return type cannot be inferred from the body.
```

**触发位置：** `Lint/DreamFXLint.cpp:186`

**原因.** dynamic input 的返回类型没法从 body 推出来。

**修复.** 加上 `Output = float;`（或它实际返回的类型）。

<a id="dfx3032"></a>

## DFX3032

**严重级别：** 错误 (`error`)

**消息：**

```text
A DynamicInput's Usage must be DynamicInput, not '%s'.
```

**触发位置：** `Lint/DreamFXLint.cpp:192`

**原因.** 一个 `DynamicInput` 文档却声明了栈 usage —— 它对「自己是什么」做了两个互相矛盾的陈述。

**修复.** 写 `Usage = DynamicInput;`，或者把文档类型改成 `Module`。

<a id="dfx3033"></a>

## DFX3033

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is declared more than once.
```

**触发位置：** `Lint/DreamFXLint.cpp:203`

**原因.** 两个输入同名，会在同一个 `Module.` 参数上撞车。

**修复.** 改掉一个。

<a id="dfx3034"></a>

## DFX3034

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is marked [StaticSwitch] but is a %s. A static switch must be a bool, an int or an enum.
```

**触发位置：** `Lint/DreamFXLint.cpp:218`

**原因.** 静态开关在编译期求解，所以它必须是开关能分支的东西。

**修复.** 改成 `bool`、`int` 或枚举 —— 或者把 `[StaticSwitch]` 去掉。

<a id="dfx3035"></a>

## DFX3035

**严重级别：** 错误 (`error`)

**消息：**

```text
Input '%s' is a [StaticSwitch], so its default must be a compile-time constant.
```

**触发位置：** `Lint/DreamFXLint.cpp:224`

**原因.** 同一条规则的另一面：编译期求解的开关不能取运行时的值。

**修复.** 给它一个字面量默认值。

<a id="dfx3036"></a>

## DFX3036

**严重级别：** 错误 (`error`)

**消息：**

```text
The Body block is empty.
```

**触发位置：** `Lint/DreamFXLint.cpp:233`

**原因.** 空 body 生成出来的模块占着一个栈位，什么都不做。

**修复.** 把 body 写了，或者把文件删了。

<a id="dfx3037"></a>

## DFX3037

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1014`

**原因.** dynamic input 的 body 不是单个表达式。Niagara 翻译器会把它包成 `Output = (Type)( <body> );`，
所以 return 之前的语句只会产出无效 HLSL，而不是一条说清真正问题的错误。

**修复.** 折成一个表达式，或者改写成 `Module` —— 模块的 body 逐字输出，想写多少条语句都行。

<a id="dfx3038"></a>

## DFX3038

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is not a stack a module can be placed in. Use one of: %s.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:896`

**原因.** `Usage` 只能取六个栈之一（L1），别的都不行。

**修复.** 用列出来的名字之一。

<a id="dfx3039"></a>

## DFX3039

**严重级别：** 错误 (`error`)

**消息：**

```text
A DynamicInput cannot return a data interface; its Output must be a value type.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:937`

**原因.** dynamic input 是往输入槽里喂一个值，而数据接口不是值。

**修复.** 返回一个值类型。数据接口在 `Properties` 里声明。

<a id="dfx3040"></a>

## DFX3040

**严重级别：** 错误 (`error`)

**消息：**

```text
(built at runtime)
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1868`

**原因.** `from` 路径在磁盘上没解析到 `.dfe`。

**修复.** 路径先相对引用它的文件解析，再相对每一个 DFX 根解析。扩展名可省。

<a id="dfx3041"></a>

## DFX3041

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' could not be parsed; see the errors above.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1881`

**原因.** 被引用的 `.dfe` 自己有错；那些错在这一条之前、按 `.dfe` 自己的路径报出来了。

**修复.** 去修那个 `.dfe`。它的诊断带的是它自己的文件和行，不是宿主的。

<a id="dfx3042"></a>

## DFX3042

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' declares a %s, but 'from' needs an Emitter document.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1891`

**原因.** `from` 拉进来的是一个 emitter，而被引用的文件声明的是别的东西。

**修复.** 让 `from` 指向一个 `.dfe`。

<a id="dfx3043"></a>

## DFX3043

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' reads user parameters this system does not declare: %s. Add them to the Properties block.
```

**触发位置：** `Generation/DreamFXGenerator.cpp:1683`

**原因.** `.dfe` 是按拷贝合并的，它读的 `User.*` 一起被拷过来。宿主必须声明这些参数，
否则拷进来的 emitter 会去读一个不存在的参数。诊断指在 `from` 那一行，因为决定是在那里做的。

**修复.** 把点名的那些参数加进宿主的 `Properties` 块。

<a id="dfx3044"></a>

## DFX3044

**严重级别：** 错误 (`error`)

**消息：**

```text
The default for input '%s' has to be a literal or an enum entry. A module input default is stored on the asset, so it cannot reference anything outside the module.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:967`

**原因.** 模块输入的默认值存在资产上，所以它引用不到模块外面的任何东西。

**修复.** 用字面量或枚举项。想让调用方提供，就把它留着，在调用点设。

<a id="dfx3046"></a>

## DFX3046

**严重级别：** 错误 (`error`)

**消息：**

```text
'%s' is not a particle attribute DreamFX knows the type of. Write the type at its first use in the body -- `float %s = ...;` -- the way a .dfs declares a new attribute.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:599`

**原因.** `.dfm` body 碰到了一个 `Particles.*` 属性，它既不是常见的 Niagara 属性，也没在 body 里声明过。
给它接的引脚需要一个类型，而猜出来的类型会把引脚接成错误的宽度。

**修复.** 在第一次使用处写上类型，和 `.dfs` 声明新属性一样：`float Particles.Moon.Phase = 0.0;`

<a id="dfx3047"></a>

## DFX3047

**严重级别：** 错误 (`error`)

**消息：**

```text
A DynamicInput computes a value; it cannot write '%s'. Move the write into a Module.
```

**触发位置：** `Generation/DreamFXModuleGenerator.cpp:1003`

**原因.** dynamic input 是在输入槽里算一个值，它在栈里没有落脚点可以往外写。

**修复.** 把写入挪进一个 `Module`。
