<a id="dfx3xxx-----declarations-and-document-structure"></a>

# DFX3xxx —— 声明与文档结构

> 生成标记之间的区块由 `.skill/gen-diagnostics.ps1` 写入。标记下方的所有内容均为手工撰写，在重新生成后依然保留。

<a id="dfx3000"></a>

## DFX3000

<!-- generated:begin DFX3000 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1989`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2000`
<!-- generated:end DFX3000 -->

**原因。** `Root="..."` 没有指向一个已挂载的内容根，或者 `Name="..."` 最后一个斜杠后面没有资产名。

**修复。** `Root` 取 `Game`、空串或 `Plugin.<PluginName>`。`Name` 是一条以资产名结尾的路径，例如 `Systems/NS_Spark`。

<a id="dfx3001"></a>

## DFX3001

<!-- generated:begin DFX3001 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1520`
<!-- generated:end DFX3001 -->

**原因。** 模块名在搜索路径上解析不到任何东西。这与 DFX3003 是两回事 —— 那个意思是模块存在，但没有这个输入 —— 否则路径打错和实参打错读起来会一模一样。

**修复。** 把目录加进 `Settings.ModulePaths`，或者写完整资产路径。`dfx list` 会打印当前路径上的每一个模块。

<a id="dfx3002"></a>

## DFX3002

<!-- generated:begin DFX3002 -->
**严重性** 错误

**消息**

```
Could not read the input schema of module '%s': %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1561`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1614`
<!-- generated:end DFX3002 -->

**原因。** 模块找到了，但它的输入签名读不出来。schema 是靠把模块加进一个临时系统探测的，所以这通常意味着这个模块不能待在它被写进去的那个栈里。

**修复。** 检查栈。`dfx schema <Module> -Stack <Stack>` 会复现这趟探测。

<a id="dfx3003"></a>

## DFX3003

<!-- generated:begin DFX3003 -->
**严重性** 错误

**消息**

```
Module '%s' has no input named '%s'.%s Available inputs: %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1678`
<!-- generated:end DFX3003 -->

**原因。** 这个模块没有叫这个名字的输入。Niagara 的输入名带空格（`Loop Duration`）；DreamFX 两侧都归一化，所以 `LoopDuration` 能匹配上，但拼错的匹配不上。

**修复。** 用列出来的名字之一。差一点的会给出建议。`dfx schema <Module>` 打印完整签名，包括藏在静态开关后面的输入。

<a id="dfx3004"></a>

## DFX3004

<!-- generated:begin DFX3004 -->
**严重性** 错误

**消息**

```
Unknown renderer type '%s'. Expected one of SpriteRenderer, MeshRenderer, RibbonRenderer, LightRenderer, DecalRenderer, ComponentRenderer, VolumeRenderer, or any UNiagaraRendererProperties subclass.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1741`
<!-- generated:end DFX3004 -->

**原因。** 渲染器类型是一个闭集。

**修复。** 用列出来的类型之一。渲染器**属性**是 schema 驱动、开放的（L8），固定的只有类型关键字。

<a id="dfx3005"></a>

## DFX3005

<!-- generated:begin DFX3005 -->
**严重性** 错误

**消息**

```
Emitter '%s' is declared more than once. Emitter names are stable keys and must be unique.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2176`
<!-- generated:end DFX3005 -->

**原因。** 两个发射器同名。名字是重建契约用来匹配 handle 的稳定键（plan 4.5），所以重名是数据丢失，不是命名洁癖。

**修复。** 改掉一个。要给一个已经存在于资产里的发射器改名，先用 `dfx rename`，这样 handle 和它的快速迭代参数才能活下来（R4）。

<a id="dfx3006"></a>

## DFX3006

<!-- generated:begin DFX3006 -->
**严重性** 错误

**消息**

```
'%s' is neither an allowed inline function (%s) nor a dynamic input: %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1184`
<!-- generated:end DFX3006 -->

**原因。** 值位置上的一个调用，既不在 L6 的内置白名单里，也不对应任何 dynamic input 资产。

**修复。** 用列出来的内置函数之一、引用一个真实的 dynamic input，或者把这段数学写进 `hlsl { }` 块。放宽白名单是设计决定（L6），不是配置项。

<a id="dfx3007"></a>

## DFX3007

<!-- generated:begin DFX3007 -->
**严重性** 错误

**消息**

```
Could not read the input schema of dynamic input '%s': %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1226`
<!-- generated:end DFX3007 -->

**原因。** dynamic input 资产找到了，但签名读不出来。

**修复。** `dfx schema <Name>` 会对同一个资产复现这次读取。

<a id="dfx3008"></a>

## DFX3008

<!-- generated:begin DFX3008 -->
**严重性** 错误

**消息**

```
Dynamic input '%s' has no input named '%s'. Available inputs: %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1278`
<!-- generated:end DFX3008 -->

**原因。** 这个 dynamic input 没有叫这个名字的输入 —— 也就是链条下一层的 DFX3003。

**修复。** 用列出来的名字之一。

<a id="dfx3009"></a>

## DFX3009

<!-- generated:begin DFX3009 -->
**严重性** 错误

**消息**

```
Dynamic input '%s' is pinned to version %s, which its asset does not offer. Available version(s): %s.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1208`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1543`
<!-- generated:end DFX3009 -->

**原因。** 一个 R7 `@version` 固定与模块资产暴露的版本对不上，或者固定到了一个从未启用版本化的资产上。这个固定记录的是源码是照哪个版本写的；DreamFX 建不出别的版本，因为外部编辑 API 没有选版本的办法。

**修复。** 照暴露出来的版本重测并更新固定值，或者把资产上的那个版本恢复回来；模块本来就没有版本化的话，把 `@` 去掉。

<a id="dfx3010"></a>

## DFX3010

<!-- generated:begin DFX3010 -->
**严重性** 错误

**消息**

```
Property '%s': %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1146`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2110`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:338`
<!-- generated:end DFX3010 -->

**原因。** 某条 `Properties` 声明的形状不对 —— 看内层消息。

**修复。** 修那条声明。可用类型列在 DFX4021 的消息里。

<a id="dfx3020"></a>

## DFX3020

<!-- generated:begin DFX3020 -->
**严重性** 错误

**消息**

```
Cannot read the default for %s setting '%s'.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2024`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:738`，`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:767`
<!-- generated:end DFX3020 -->

**原因。** 一个设置键不受支持，或者生成器无法从当前引擎读取某个映射设置在全新资产上的默认值。后者会阻止用已被移除设置的陈旧值进行重建。

**修复。** 对于未知的键，使用诊断中列出的受支持名字之一。如果读取默认值失败，检查随附的引擎/探测错误，并报告引擎版本和设置名；重建需要一份有效的默认 schema。

<a id="dfx3021"></a>

## DFX3021

<!-- generated:begin DFX3021 -->
**严重性** 错误

**消息**

```
User parameter '%s' is declared more than once.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2057`
<!-- generated:end DFX3021 -->

**原因。** 两个用户参数同名；蓝图的 `SetNiagaraVariable` 会打到赢的那一个上。

**修复。** 改掉一个。

<a id="dfx3030"></a>

## DFX3030

<!-- generated:begin DFX3030 -->
**严重性** 错误

**消息**

```
A Module or DynamicInput must declare Settings.Usage -- it decides which stacks the module can be placed in.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:187`
<!-- generated:end DFX3030 -->

**原因。** `Usage` 决定一个模块能被放进哪些栈。没有它，模块存在，但从任何栈都够不着。

**修复。** 加上 `Usage = ParticleUpdate;`（或别的栈）。数组可以选多个：`Usage = [ParticleSpawn, ParticleUpdate];`

<a id="dfx3031"></a>

## DFX3031

<!-- generated:begin DFX3031 -->
**严重性** 错误

**消息**

```
A DynamicInput must declare Settings.Output -- its return type cannot be inferred from the body.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:195`
<!-- generated:end DFX3031 -->

**原因。** dynamic input 的返回类型没法从它的 body 推出来。

**修复。** 加上 `Output = float;`（或它实际返回的类型）。

<a id="dfx3032"></a>

## DFX3032

<!-- generated:begin DFX3032 -->
**严重性** 错误

**消息**

```
A DynamicInput's Usage must be DynamicInput or a nonempty array of stack names.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:219`
<!-- generated:end DFX3032 -->

**原因。** 一个 `DynamicInput` 声明了无效的 Usage 值：一个裸栈名、一个空数组，或者一个包含未知栈名的数组。

**修复。** 用 `Usage = DynamicInput;` 取它默认的栈掩码，或者用一个非空栈数组（例如 `Usage = [SystemSpawn, SystemUpdate];`）来选择该输入在哪些地方可用。

<a id="dfx3033"></a>

## DFX3033

<!-- generated:begin DFX3033 -->
**严重性** 错误

**消息**

```
Input '%s' is declared more than once.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:229`
<!-- generated:end DFX3033 -->

**原因。** 两个输入同名，会在同一个 `Module.` 参数上撞车。

**修复。** 改掉一个。

<a id="dfx3034"></a>

## DFX3034

<!-- generated:begin DFX3034 -->
**严重性** 错误

**消息**

```
Input '%s' is marked [StaticSwitch] but is a %s. A static switch must be a bool, an int or an enum.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:244`
<!-- generated:end DFX3034 -->

**原因。** R5：静态开关在编译期求解，所以它必须是开关能分支的东西。

**修复。** 改成 `bool`、`int` 或枚举 —— 或者把 `[StaticSwitch]` 去掉。

<a id="dfx3035"></a>

## DFX3035

<!-- generated:begin DFX3035 -->
**严重性** 错误

**消息**

```
Input '%s' is a [StaticSwitch], so its default must be a compile-time constant.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:250`
<!-- generated:end DFX3035 -->

**原因。** 同一条 R5 的另一面：编译期求解的开关不能取运行时的值。

**修复。** 给它一个字面量默认值。

<a id="dfx3036"></a>

## DFX3036

<!-- generated:begin DFX3036 -->
**严重性** 错误

**消息**

```
The Body block is empty.
```

**触发位置** `Source/DreamFXEditor/Private/Lint/DreamFXLint.cpp:259`
<!-- generated:end DFX3036 -->

**原因。** 空 body 生成出来的模块占着一个栈位，什么都不做。

**修复。** 把 body 写了，或者把文件删了。

<a id="dfx3037"></a>

## DFX3037

<!-- generated:begin DFX3037 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1054`
<!-- generated:end DFX3037 -->

**原因。** dynamic input 的 body 不是单个表达式。Niagara 翻译器会把它包成 `Output = (Type)( <body> );`，所以 return 之前的语句只会产出无效 HLSL，而不是一条说清真正问题的错误。

**修复。** 折成一个表达式，或者改写成 `Module` —— 模块的 body 逐字输出，想写多少条语句都行。

<a id="dfx3038"></a>

## DFX3038

<!-- generated:begin DFX3038 -->
**严重性** 错误

**消息**

```
'%s' is not a stack a module can be placed in. Use one of: %s.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:931`
<!-- generated:end DFX3038 -->

**原因。** `Usage` 只能取六个栈之一（L1），别的都不行。

**修复。** 用列出来的名字之一。

<a id="dfx3039"></a>

## DFX3039

<!-- generated:begin DFX3039 -->
**严重性** 错误

**消息**

```
A DynamicInput cannot return a data interface; its Output must be a value type.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:972`
<!-- generated:end DFX3039 -->

**原因。** dynamic input 是往输入槽里喂一个值，而数据接口不是值。

**修复。** 返回一个值类型。数据接口在 `Properties` 里声明，在运行时喂入（计划 3.5）。

<a id="dfx3040"></a>

## DFX3040

<!-- generated:begin DFX3040 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2195`
<!-- generated:end DFX3040 -->

**原因。** `from` 路径在磁盘上没解析到 `.dfe`。

**修复。** 路径先相对引用它的文件解析，再相对每一个 DFX 根解析。扩展名可省。

<a id="dfx3041"></a>

## DFX3041

<!-- generated:begin DFX3041 -->
**严重性** 错误

**消息**

```
'%s' could not be parsed; see the errors above.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2208`
<!-- generated:end DFX3041 -->

**原因。** 被引用的 `.dfe` 自己有错；那些错在这一条之前、按 `.dfe` 自己的路径报出来了。

**修复。** 去修那个 `.dfe`。它的诊断带的是它自己的文件和行，不是宿主的。

<a id="dfx3042"></a>

## DFX3042

<!-- generated:begin DFX3042 -->
**严重性** 错误

**消息**

```
'%s' declares a %s, but 'from' needs an Emitter document.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2218`
<!-- generated:end DFX3042 -->

**原因。** `from` 拉进来的是一个 emitter，而被引用的文件声明的是别的东西。

**修复。** 让 `from` 指向一个 `.dfe`。

<a id="dfx3043"></a>

## DFX3043

<!-- generated:begin DFX3043 -->
**严重性** 错误

**消息**

```
'%s' reads user parameters this system does not declare: %s. Add them to the Properties block.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1975`
<!-- generated:end DFX3043 -->

**原因。** `.dfe` 是按拷贝合并的（R3），它读的 `User.*` 一起被拷过来。宿主必须声明这些参数，否则拷进来的 emitter 会去读一个不存在的参数。诊断指在 `from` 那一行，因为决定是在那里做的。

**修复。** 把点名的那些参数加进宿主的 `Properties` 块。

<a id="dfx3044"></a>

## DFX3044

<!-- generated:begin DFX3044 -->
**严重性** 错误

**消息**

```
The default for input '%s' has to be a literal or an enum entry. A module input default is stored on the asset, so it cannot reference anything outside the module.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1007`
<!-- generated:end DFX3044 -->

**原因。** 模块输入的默认值存在资产上，所以它引用不到模块外面的任何东西。

**修复。** 用字面量或枚举项。想让调用方提供，就把它留着，在调用点设。

<a id="dfx3046"></a>

## DFX3046

<!-- generated:begin DFX3046 -->
**严重性** 错误

**消息**

```
'%s' is not a particle attribute DreamFX knows the type of. Write its type at first use, for example `float %s = ...;`.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:623`
<!-- generated:end DFX3046 -->

**原因。** `.dfm` body 碰到了一个 `Particles.*` 属性，它既不是常见的 Niagara 属性，也没在 body 里声明过。给它接的引脚需要一个类型，而猜出来的类型会把引脚接成错误的宽度。

**修复。** 在第一次使用处写上类型，和 `.dfs` 声明新属性一样：`float Particles.Moon.Phase = 0.0;`

<a id="dfx3047"></a>

## DFX3047

<!-- generated:begin DFX3047 -->
**严重性** 错误

**消息**

```
A DynamicInput computes a value; it cannot write '%s'. Move the write into a Module.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:1043`
<!-- generated:end DFX3047 -->

**原因。** dynamic input 是在输入槽里算一个值，它在栈里没有落脚点可以往外写。

**修复。** 把写入挪进一个 `Module`。

<a id="dfx3048"></a>

## DFX3048

<!-- generated:begin DFX3048 -->
**严重性** 错误

**消息**

```
Default override '%s' changes its type from %s to %s. Overrides must keep the base parameter's type.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXEmitterMerge.cpp:84`
<!-- generated:end DFX3048 -->

**原因。** 一个使用 `from` 的 emitter，用不同的类型覆盖了基础 `Defaults` 参数。基础栈仍可能按原来的类型读取该参数。

**修复。** 覆盖参数值时保持基础参数的类型。如果需要不同的类型，请使用单独的参数名。

<a id="dfx3049"></a>

## DFX3049

<!-- generated:begin DFX3049 -->
**严重性** 错误

**消息**

```
MeshRenderer has no Material property. Set OverrideMaterials = [\"/path/to/material\"]; and bOverrideMaterials = true; to override its mesh materials.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:1792`
<!-- generated:end DFX3049 -->

**原因。** `MeshRenderer` 声明了 `Material`，但网格体材质由其覆盖数组和启用开关控制。

**修复。** 把 `Material` 替换为 `OverrideMaterials = ["/path/to/material"];` 并设置 `bOverrideMaterials = true;`。

<a id="dfx3050"></a>

## DFX3050

<!-- generated:begin DFX3050 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:178`
<!-- generated:end DFX3050 -->

**原因。** 系统的 `Parent` 引用无法解析到一个 `.dfs` 源文件。

**修复。** 修正父源路径，并确保被引用的 `.dfs` 文件存在于受支持的源位置。

<a id="dfx3051"></a>

## DFX3051

<!-- generated:begin DFX3051 -->
**严重性** 错误

**消息**

```
Only a System document can inherit a .dfs parent.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:171`、`Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:194`、`Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:201`
<!-- generated:end DFX3051 -->

**原因。** 父源无法解析、没有声明 `System`，或者正被一个非 system 文档继承。

**修复。** 只在 `.dfs` system 文档之间使用 system `Parent` 继承，并修复为父文件报告的任何诊断。

<a id="dfx3052"></a>

## DFX3052

<!-- generated:begin DFX3052 -->
**严重性** 错误

**消息**

```
System inheritance contains a cycle or exceeds 128 sources: %s -> %s.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:184`
<!-- generated:end DFX3052 -->

**原因。** 系统的父链重复访问同一个源文件，或超过了 128 个源的限制。

**修复。** 移除诊断中显示的循环父引用，或缩短继承链。

<a id="dfx3053"></a>

## DFX3053

<!-- generated:begin DFX3053 -->
**严重性** 错误

**消息**

```
Inherited parameter '%s' changes type from %s to %s. An override must preserve the parent's parameter type.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:85`
<!-- generated:end DFX3053 -->

**原因。** 子系统用不同的 Niagara 类型重新声明了一个继承来的参数。

**修复。** 覆盖参数值时保持父级的参数类型，或者给新参数换一个名字。

<a id="dfx3054"></a>

## DFX3054

<!-- generated:begin DFX3054 -->
**严重性** 错误

**消息**

```
Parameter '%s' is declared twice in one inheritance layer. An override must be declared in the child source, not twice in the same source.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:22`、`Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:34`、`Source/DreamFXEditor/Private/Generation/DreamFXSystemInheritance.cpp:45`
<!-- generated:end DFX3054 -->

**原因。** 系统继承链中的某一个源把同一个参数名或 emitter 名声明了不止一次。

**修复。** 每个源文件里每个名字只保留一条声明，继承来的覆盖放进子源里。

<a id="dfx3055"></a>

## DFX3055

<!-- generated:begin DFX3055 -->
**严重性** 错误

**消息**

```
(built at runtime)
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2256`、`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2262`
<!-- generated:end DFX3055 -->

**原因。** 某个 emitter 的 `inherits` 路径解析不到一个已启用继承的独立 Niagara emitter 资产。

**修复。** 修正资产路径并在那个 emitter 上启用继承，或者当预期的父级是 DreamFX 源时改用 `from "...dfe"`。

<a id="dfx3056"></a>

## DFX3056

<!-- generated:begin DFX3056 -->
**严重性** 错误

**消息**

```
Native parent has an unavailable version or a cyclic parent chain: %s.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2275`、`Source/DreamFXEditor/Private/Generation/DreamFXGenerator.cpp:2282`
<!-- generated:end DFX3056 -->

**原因。** 原生 emitter 的父链里有一个不可用的版本、一个循环，或者有未合并的祖先改动。

**修复。** 选一个已存在的版本 GUID，去掉任何父链循环，并在重建前先在 Niagara 里合并并保存待处理的祖先改动。

<a id="dfx3057"></a>

## DFX3057

<!-- generated:begin DFX3057 -->
**严重性** 错误

**消息**

```
Attribute '%s' is declared with conflicting types.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:570`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:577`, `Source/DreamFXEditor/Private/Generation/DreamFXModuleGenerator.cpp:695`
<!-- generated:end DFX3057 -->

**原因。** 一个 `.dfm` 自定义属性类型互相冲突、在自己初始化期间被读取，或者首次初始化发生在一个可能不会执行到的路径上。

**修复。** 新属性要在控制流之前给它一个无条件的整体值初始化器；要更新由更早模块提供的自定义属性，就只声明它的类型而不带初始化器，并保持后续类型一致。

<a id="dfx3058"></a>

## DFX3058

<!-- generated:begin DFX3058 -->
**严重性** 错误

**消息**

```
Patch '%s': unknown topology '%s'. Expected Grid(Columns, Rows), Strip(Columns), Ring(Columns, Rows), Tube(Columns, Rows) or Sphere(Columns, Rows).
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:428`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:438`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:449`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:465`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:476`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:487`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:505`
<!-- generated:end DFX3058 -->

**原因。** 某个 patch 的拓扑调用不是 lowering 能识别的形式：未知拓扑、顶点数个数错误（`Strip` 接受一个，其他接受两个）、计数不是至少为 2 的整数字面量，或选项未知或形式错误（`Copies`、`Material`、`Flip`）。

**修复。** 使用整数顶点数写 `Grid`、`Strip`、`Ring`、`Tube` 或 `Sphere`，然后按名称写选项：`Tube(65, 17, Copies = 3, Flip = true)`。计数是顶点数，因此闭合形状的接缝列也包含在内。

<a id="dfx3059"></a>

## DFX3059

<!-- generated:begin DFX3059 -->
**严重性** 错误

**消息**

```
%s needs an emitter named '%s', which %s already uses. Rename one of them.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1041`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1066`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1125`
<!-- generated:end DFX3059 -->

**原因。** 名为 `X` 的 `DynamicMesh` 会变成用户参数 `User.X`、绘制它的发射器 `X`，以及每个 patch 一个发射器 `X_<Patch>`。这些名称中有一个已被占用——被 `Properties` 占用、被 `Emitter` 块占用、被另一个块占用——或者同一个块的两个 patch 共享一个名称。

**修复。** 重命名该块、该 patch，或任何已经占用该名称的对象。名称是构建后资产的一部分，因此只有在块保持其名称时，设置 `User.X` 的蓝图才能继续工作。

<a id="dfx3060"></a>

## DFX3060

<!-- generated:begin DFX3060 -->
**严重性** 错误

**消息**

```
Material takes a quoted material asset path, e.g. Material = \"/Game/FX/M_Shell.M_Shell\";
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1404`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:274`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:307`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:316`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:392`
<!-- generated:end DFX3060 -->

**原因。** `DynamicMesh` 块的 `Settings` 条目未知、重复书写或形式错误：`TexCoords` 是 0-8 的整数，`Colors` 是 true/false，`Normals` 是 None/Auto/Explicit 之一，`Bounds` 是一个最小值不大于最大值的 `box(...)`，`Duration` 是一个正数。设置了 `Meshes` 或 `MeshBindings` 的 `Renderer` 块也会被拒绝——渲染器只绘制此网格，不绘制其他内容。

**修复。** 使用文档中记录的键和形式；消息会指出期望的形式。

<a id="dfx3061"></a>

## DFX3061

<!-- generated:begin DFX3061 -->
**严重性** 错误

**消息**

```
Material '%s': %s
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:282`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:289`
<!-- generated:end DFX3061 -->

**原因。** `Material`（在块上或在某个 patch 上）未解析为材质资产。引擎会用其默认材质绘制该网格，这绝不是本意。Lint 不会查找资产，因此这由构建来报告。

**修复。** 修正路径。`.dfs` 接受的每种资产写法都有效：`/Game/FX/M_X`、`/Game/FX/M_X.M_X`、`Plugin.Name:Materials/M_X`。

<a id="dfx3062"></a>

## DFX3062

<!-- generated:begin DFX3062 -->
**严重性** 错误

**消息**

```
Input '%s' of DynamicMesh '%s' uses a name the patch prologue already declares (U, V, I, J, NU, NV, K, NK, Rand, T, Index, Theta, Phi, Dir, the outputs, or anything starting with DM_). Rename it.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1088`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1095`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1102`
<!-- generated:end DFX3062 -->

**原因。** `Inputs` 条目无法传给 patch 主体：其名称是生成的序言已声明的名称之一（`U`、`V`、`I`、`J`、`NU`、`NV`、`K`、`NK`、`Rand`、`T`、`Index`、`Theta`、`Phi`、`Dir`、输出，以及以 `DM_` 开头的任何内容），被声明了两次，或没有值。

**修复。** 重命名它，或给它要传入的值：`float Gain = User.Gain;`。输入不是带默认值的模块输入——每个输入都说明每个 patch 接收什么。

<a id="dfx3063"></a>

## DFX3063

<!-- generated:begin DFX3063 -->
**严重性** 错误

**消息**

```
DynamicMesh '%s' has no Patch, so there is nothing to draw. Add one, e.g. `Patch Sheet = Grid(33, 9) { }`.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1115`
<!-- generated:end DFX3063 -->

**原因。** 该块未声明 `Patch`，因此网格将没有顶点。

**修复。** 添加一个 patch，例如 `Patch Sheet = Grid(33, 9) { }`——空主体会绘制拓扑的默认形状。

<a id="dfx3064"></a>

## DFX3064

<!-- generated:begin DFX3064 -->
**严重性** 错误

**消息**

```
Patch '%s' has %lld vertices (%d x %d x %d copies); a patch holds at most %lld, because the generated index arithmetic is exact in float only that far. Split it into more patches.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1198`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:515`
<!-- generated:end DFX3064 -->

**原因。** 某个 patch 的顶点数超过 16,777,216（生成的索引运算只有在不超过该值时才能在 float 中保持精确），或者整个网格——在纹理坐标变通方案将声明的顶点数乘以 `TexCoords` 后——无法容纳进数据接口的 int32 计数。

**修复。** 将该 patch 拆分为多个，降低其分辨率或副本数，或减少 UV 通道数。

<a id="dfx3065"></a>

## DFX3065

<!-- generated:begin DFX3065 -->
**严重性** 错误

**消息**

```
Patch '%s' writes '%s', but DynamicMesh '%s' has Normals = Auto, which evaluates the body four times per vertex -- the write would run four times. Keep particle state out of the body, or use Normals = Explicit and write Normal yourself.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1157`
<!-- generated:end DFX3065 -->

**原因。** 使用 `Normals = Auto` 时，patch 主体会针对每个顶点求值四次（三个采样点相距四分之一个网格步长，然后是该顶点本身），因此其中的 `Particles.*` 属性写入会运行四次，最后一次使用顶点自身的值。累积状态（`+=`、`++`）会完全错误。

**修复。** 将粒子状态移出主体，或切换到 `Normals = Explicit` 并自行写入 `Normal`（拓扑的解析法线是起始值）。

<a id="dfx3066"></a>

## DFX3066

<!-- generated:begin DFX3066 -->
**严重性** 错误

**消息**

```
DynamicMesh '%s' needs a plain name (letters, digits, underscores): it becomes a user parameter, emitter names and generated asset names.
```

**触发位置** `Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1058`、`Source/DreamFXEditor/Private/Generation/DreamFXDynamicMesh.cpp:1133`
<!-- generated:end DFX3066 -->

**原因。** `DynamicMesh` 或 `Patch` 的名称不是普通标识符——通常是用反引号括起来并带空格或标点的名称。这些名称会成为用户参数、发射器名称和生成的模块资产名称，而资产名称不能包含它们。

**修复。** 使用字母、数字和下划线，并以字母开头：`DynamicMesh MyMesh`。

