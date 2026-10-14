---
title: "DreamFX 诊断"
description: "DreamFX 诊断"
---

<a id="dreamfx-diagnostics"></a>

# DreamFX 诊断

每个 `DFXnnnn` DreamFX 可能发出的诊断。首位数字表示引发它的阶段：

- **DFX1xxx** --- 驱动与文件 I/O --- [DFX1xxx.md](DFX1xxx.md)
- **DFX2xxx** --- 词法与语法 --- [DFX2xxx.md](DFX2xxx.md)
- **DFX3xxx** --- 声明与文档结构 --- [DFX3xxx.md](DFX3xxx.md)
- **DFX4xxx** --- 值、类型与表达式 --- [DFX4xxx.md](DFX4xxx.md)
- **DFX5xxx** --- 生成与资产写入 --- [DFX5xxx.md](DFX5xxx.md)
- **DFX6xxx** --- Niagara 编译 --- [DFX6xxx.md](DFX6xxx.md)
- **DFX7xxx** --- 来源、漂移与 lint --- [DFX7xxx.md](DFX7xxx.md)
- **DFX8xxx** --- 反编译器 --- [DFX8xxx.md](DFX8xxx.md)

由 `.skill/gen-diagnostics.ps1` 生成；添加代码后请运行它。

| 代码 | 严重性 | 消息 |
| --- | --- | --- |
| [DFX1000](DFX1xxx.md#dfx1000) | error | 无法读取源文件 '%s'。 |
| [DFX1001](DFX1xxx.md#dfx1001) | error | 字符串字面量未终止。 |
| [DFX1002](DFX1xxx.md#dfx1002) | error | 块注释未终止。 |
| [DFX1003](DFX1xxx.md#dfx1003) | error | 意外的字符 '%c' (U+%04X)。 |
| [DFX1004](DFX1xxx.md#dfx1004) | error | 原始块未终止：缺少 '}'。 |
| [DFX1005](DFX1xxx.md#dfx1005) | error | 反引号名称未终止。`name` 必须在同一行内闭合。 |
| [DFX2000](DFX2xxx.md#dfx2000) | error | 未知的文档类型 '%s'。应为 System、Emitter、Module 或 DynamicInput。 |
| [DFX2001](DFX2xxx.md#dfx2001) | error | 需要 '{' 来打开原始块，但找到了 '%s'。 |
| [DFX2002](DFX2xxx.md#dfx2002) | error | 需要一个名称，但找到了 %s。 |
| [DFX2003](DFX2xxx.md#dfx2003) | error | 需要一个数字，但找到了 '%s'。 |
| [DFX2004](DFX2xxx.md#dfx2004) | error | 需要一个值，但找到了 %s。 |
| [DFX2005](DFX2xxx.md#dfx2005) | error | '#EndRegion' 没有匹配的 '#Region'。 |
| [DFX2006](DFX2xxx.md#dfx2006) | error | 未知的指令 '#%s'。应为 Region 或 EndRegion。 |
| [DFX2007](DFX2xxx.md#dfx2007) | error | '@' 之后需要一个版本。 |
| [DFX2008](DFX2xxx.md#dfx2008) | error | 模块 '%s' 被赋予了位置参数。模块输入必须写成 'Name = Value'。 |
| [DFX2010](DFX2xxx.md#dfx2010) | warning | '#Region \"%s\"' 从未用 '#EndRegion' 闭合。 |
| [DFX2013](DFX2xxx.md#dfx2013) | error | '%s' 是系统作用域堆栈，必须写在 .dfs 的顶层，而不是发射器内部。 |
| [DFX2015](DFX2xxx.md#dfx2015) | error | 'from' 后面必须跟随一个指向 .dfe 源文件的带引号路径。 |
| [DFX2016](DFX2xxx.md#dfx2016) | error | (在运行时构建) |
| [DFX2018](DFX2xxx.md#dfx2018) | error | Module 或 DynamicInput 必须声明一个 'Body = { }' 块。 |
| [DFX2019](DFX2xxx.md#dfx2019) | error | 头部参数 '%s' 必须是一个带引号的字符串。 |
| [DFX2020](DFX2xxx.md#dfx2020) | error | 未知的曲线键属性 '%s'。应为 Interp、Tangent、Arrive 或 Leave。 |
| [DFX2021](DFX2xxx.md#dfx2021) | error | 文件声明了 '%s'，但其扩展名为 '%s'。请将文件重命名为 '%s' 或更改声明。 |
| [DFX2022](DFX2xxx.md#dfx2022) | error | 文档结束后出现意外的 '%s'。一个 DreamFX 文件只声明一个顶层对象。 |
| [DFX2023](DFX2xxx.md#dfx2023) | error | '%s' 是模块调用，因此不能赋予类型。类型只能写在赋值上。 |
| [DFX2024](DFX2xxx.md#dfx2024) | error | Defaults 声明不能被禁用；默认值不是被执行的堆栈节点。 |
| [DFX2025](DFX2xxx.md#dfx2025) | error | 未知的 OnEvent 参数 '%s'。应为 Source、Event、Mode、SpawnNumber、MaxEventsPerFrame、UpdateAttribut... |
| [DFX2026](DFX2xxx.md#dfx2026) | error | 未知的 Stage 参数 '%s'。应为阶段迭代、enabled、dispatch 或 particle-state 选项；请参阅 th... |
| [DFX2027](DFX2xxx.md#dfx2027) | error | Group(...) 接受一个带引号的名称：Group(\"Name\") { ... }。 |
| [DFX2028](DFX2xxx.md#dfx2028) | error | Group(...) 需要一个非空名称。 |
| [DFX3000](DFX3xxx.md#dfx3000) | error | (在运行时构建) |
| [DFX3001](DFX3xxx.md#dfx3001) | error | (在运行时构建) |
| [DFX3002](DFX3xxx.md#dfx3002) | error | 无法读取模块 '%s' 的输入架构：%s |
| [DFX3003](DFX3xxx.md#dfx3003) | error | 模块 '%s' 没有名为 '%s' 的输入。%s 可用输入：%s |
| [DFX3004](DFX3xxx.md#dfx3004) | error | 未知的渲染器类型 '%s'。应为 SpriteRenderer、MeshRenderer、RibbonRenderer、LightRenderer、De... 之一 |
| [DFX3005](DFX3xxx.md#dfx3005) | error | 发射器 '%s' 被声明了多次。发射器名称是稳定键，必须唯一。 |
| [DFX3006](DFX3xxx.md#dfx3006) | error | '%s' 既不是允许的内联函数 (%s)，也不是动态输入：%s |
| [DFX3007](DFX3xxx.md#dfx3007) | error | 无法读取动态输入 '%s' 的输入架构：%s |
| [DFX3008](DFX3xxx.md#dfx3008) | error | 动态输入 '%s' 没有名为 '%s' 的输入。可用输入：%s |
| [DFX3009](DFX3xxx.md#dfx3009) | error | 动态输入 '%s' 被固定到版本 %s，而其资产不提供该版本。可用版本：%s。 |
| [DFX3010](DFX3xxx.md#dfx3010) | error | 属性 '%s'：%s |
| [DFX3020](DFX3xxx.md#dfx3020) | error | 无法读取 %s 设置 '%s' 的默认值。 |
| [DFX3021](DFX3xxx.md#dfx3021) | error | 用户参数 '%s' 被声明了多次。 |
| [DFX3030](DFX3xxx.md#dfx3030) | error | Module 或 DynamicInput 必须声明 Settings.Usage —— 它决定模块可以放入哪些堆栈。 |
| [DFX3031](DFX3xxx.md#dfx3031) | error | DynamicInput 必须声明 Settings.Output —— 其返回类型无法从主体推断。 |
| [DFX3032](DFX3xxx.md#dfx3032) | error | DynamicInput 的 Usage 必须是 DynamicInput 或非空的堆栈名称数组。 |
| [DFX3033](DFX3xxx.md#dfx3033) | error | 输入 '%s' 被声明了多次。 |
| [DFX3034](DFX3xxx.md#dfx3034) | error | 输入 '%s' 被标记为 [StaticSwitch]，但它是 %s。静态开关必须是 bool、int 或 enum。 |
| [DFX3035](DFX3xxx.md#dfx3035) | error | 输入 '%s' 是一个 [StaticSwitch]，因此其默认值必须是编译时常量。 |
| [DFX3036](DFX3xxx.md#dfx3036) | error | Body 块为空。 |
| [DFX3037](DFX3xxx.md#dfx3037) | error | (在运行时构建) |
| [DFX3038](DFX3xxx.md#dfx3038) | error | '%s' 不是模块可以放入的堆栈。请使用以下之一：%s。 |
| [DFX3039](DFX3xxx.md#dfx3039) | error | DynamicInput 不能返回数据接口；其 Output 必须是值类型。 |
| [DFX3040](DFX3xxx.md#dfx3040) | error | (在运行时构建) |
| [DFX3041](DFX3xxx.md#dfx3041) | error | 无法解析 '%s'；请参阅上面的错误。 |
| [DFX3042](DFX3xxx.md#dfx3042) | error | '%s' 声明了 %s，但 'from' 需要 Emitter 文档。 |
| [DFX3043](DFX3xxx.md#dfx3043) | error | '%s' 读取了此系统未声明的用户参数：%s。请将它们添加到 Properties 块中。 |
| [DFX3044](DFX3xxx.md#dfx3044) | error | 输入 '%s' 的默认值必须是字面量或枚举项。模块输入默认值存储在资... |
| [DFX3046](DFX3xxx.md#dfx3046) | error | '%s' 不是 DreamFX 知道其类型的粒子属性。在首次使用时写出其类型，例如 `float... |
| [DFX3047](DFX3xxx.md#dfx3047) | error | DynamicInput 计算一个值；它无法写入 '%s'。请将写入移到 Module 中。 |
| [DFX3048](DFX3xxx.md#dfx3048) | error | 默认覆盖 '%s' 将类型从 %s 更改为 %s。覆盖必须保持基础参数的类型。 |
| [DFX3049](DFX3xxx.md#dfx3049) | error | MeshRenderer 没有 Material 属性。设置 OverrideMaterials = [\"/path/to/material\"]；并设置 bOverrideMateri... |
| [DFX3050](DFX3xxx.md#dfx3050) | error | （在运行时构建） |
| [DFX3051](DFX3xxx.md#dfx3051) | error | 只有 System 文档可以继承 .dfs 父级。 |
| [DFX3052](DFX3xxx.md#dfx3052) | error | System 继承包含循环或超过 128 个来源：%s -> %s。 |
| [DFX3053](DFX3xxx.md#dfx3053) | error | 继承的参数 '%s' 将类型从 %s 更改为 %s。覆盖必须保留父级的参数类型。 |
| [DFX3054](DFX3xxx.md#dfx3054) | error | 参数 '%s' 在同一继承层中被声明了两次。覆盖必须在子来源中声明... |
| [DFX3055](DFX3xxx.md#dfx3055) | error | （在运行时构建） |
| [DFX3056](DFX3xxx.md#dfx3056) | error | 原生父级具有不可用的版本或循环的父级链：%s。 |
| [DFX3057](DFX3xxx.md#dfx3057) | error | 特性 '%s' 被声明为具有冲突的类型。 |
| [DFX4001](DFX4xxx.md#dfx4001) | error | 输入 '%s' 期望 %s，但写入了一个数字。 |
| [DFX4002](DFX4xxx.md#dfx4002) | error | '%s' 的表达式：向量字面量必须有 2 到 4 个分量。 |
| [DFX4003](DFX4xxx.md#dfx4003) | error | 输入 '%s' 是整数，但写入了 %s。窄化不是隐式的——如果截断是预期的，请写入 int(%s)... |
| [DFX4004](DFX4xxx.md#dfx4004) | error | 输入 '%s' 没有可解析的 Niagara 类型。 |
| [DFX4005](DFX4xxx.md#dfx4005) | error | 属性 '%s'：分量 %d 不是数字。 |
| [DFX4006](DFX4xxx.md#dfx4006) | error | 输入 '%s' 是 %s，其中没有名为 '%s' 的条目。有效条目：%s |
| [DFX4007](DFX4xxx.md#dfx4007) | error | 输入 '%s' 期望 %s。'%s' 既不是该类型的字面量也不是参数引用——参数引用... |
| [DFX4010](DFX4xxx.md#dfx4010) | error | 输入 '%s' 在动态输入 '%s' 上设置了多次。 |
| [DFX4020](DFX4xxx.md#dfx4020) | error | 参数 '%s'：'Object' 需要内部类型，例如 Object<Texture>。 |
| [DFX4021](DFX4xxx.md#dfx4021) | error | 参数 '%s'：没有名为 '%s' 的 UObject 类。Object<> 接受类名，例如 Object<Texture> 或 Object<... |
| [DFX4022](DFX4xxx.md#dfx4022) | error | 无法从 '%s' 的值推断其类型。对新特性的首次赋值必须使用字面量，因此... |
| [DFX4023](DFX4xxx.md#dfx4023) | error | 属性 '%s'：box() 接受 6 个数字——minX、minY、minZ、maxX、maxY、maxZ。 |
| [DFX4024](DFX4xxx.md#dfx4024) | error | 无法为 '%s = %s' 推断类型：此处 '%s' 的类型未知。请在 Properties 中声明它，或先赋值一个字面量... |
| [DFX4025](DFX4xxx.md#dfx4025) | error | '%s' 不是有效的赋值目标。参数名带有命名空间限定，例如 Particles.MyValue 或 E... |
| [DFX4026](DFX4xxx.md#dfx4026) | error | 'Bind %s -> %s'：目标必须是限定参数——要么是像 Particles.SpriteSiz... 这样的命名空间... |
| [DFX4027](DFX4xxx.md#dfx4027) | error | '%s' 是 %s，但 '%s' 是 %s。链接直接绑定参数——不存在转换。请将 '%s' 声明为 %... |
| [DFX4028](DFX4xxx.md#dfx4028) | error | '%s' 的默认值需要声明的类型。默认值没有模块签名可从中推断，因此请写入：`... |
| [DFX4029](DFX4xxx.md#dfx4029) | error | '%s' 的默认值必须是字面量、枚举或另一个参数。动态输入按部件计算一个值... |
| [DFX4030](DFX4xxx.md#dfx4030) | error | '%s' 的 hlsl 块必须是单个表达式：没有语句、没有局部变量、没有 return。将 mul... |
| [DFX4031](DFX4xxx.md#dfx4031) | error | '%s' 不是允许的内联函数。允许：%s。其他任何内容应放在 .dfm 动态输入或 hl... |
| [DFX4032](DFX4xxx.md#dfx4032) | error | '%s' 在 '%s' 的表达式中不是参数。只有命名空间限定的参数（User.X、Particles....） |
| [DFX4033](DFX4xxx.md#dfx4033) | error | 内置 '%s' 接受位置参数，而不是命名参数。 |
| [DFX4034](DFX4xxx.md#dfx4034) | error | 内置 '%s' 接受 %d 个参数，但写入了 %d 个。 |
| [DFX4035](DFX4xxx.md#dfx4035) | error | '%s' 的表达式包含没有 HLSL 形式的值。 |
| [DFX4036](DFX4xxx.md#dfx4036) | error | '%s' 的 hlsl 块为空。 |
| [DFX4037](DFX4xxx.md#dfx4037) | error | '%s' 是 %s；曲线 { } 字面量只适合曲线数据接口输入。 |
| [DFX4038](DFX4xxx.md#dfx4038) | error | '%s' 的曲线没有键。 |
| [DFX4039](DFX4xxx.md#dfx4039) | error | 未知的曲线插值 '%s'。预期为 Auto、Cubic、Linear 或 Constant。 |
| [DFX4040](DFX4xxx.md#dfx4040) | error | 参数 '%s'：'%s' 处没有资产。 |
| [DFX4041](DFX4xxx.md#dfx4041) | error | 参数 '%s'：'%s' 是 %s，不是 %s。 |
| [DFX4042](DFX4xxx.md#dfx4042) | error | 未知的曲线切线模式 '%s'。预期为 Auto、User、Break 或 None。 |
| [DFX4043](DFX4xxx.md#dfx4043) | error | 输入 '%s' 是实例支持的 %s，不能有字面量或资产路径默认值。省略默认值，并... |
| [DFX4044](DFX4xxx.md#dfx4044) | error | int(...) 默认值需要一个数字字面量。运行时转换应放在堆栈表达式中。 |
| [DFX4091](DFX4xxx.md#dfx4091) | error | 输入 '%s'：动态输入、hlsl 块、曲线和内联表达式尚不可用（计划于 P...） |
| [DFX5001](DFX5xxx.md#dfx5001) | error | 堆栈 '%s' 没有 Niagara 脚本用法映射。 |
| [DFX5002](DFX5xxx.md#dfx5002) | warning | 此系统未声明任何发射器，因此不会产生任何内容。 |
| [DFX5003](DFX5xxx.md#dfx5003) | info | '%s' 未在此来源中声明，因此其现有模块保持不变：%s |
| [DFX5004](DFX5xxx.md#dfx5004) | info | 未设置 Material，因此应用了引擎默认值：%s。写入 'Material = \"...\";' 以选择您自己的材质。 |
| [DFX5015](DFX5xxx.md#dfx5015) | error | 发射器 '%s' 在上次构建的计划中没有记录。 |
| [DFX5020](DFX5xxx.md#dfx5020) | error | 此堆栈的上次构建为 %d 个模块记录了 %d 个模块节点，因此其节点无法寻址。 |
| [DFX5027](DFX5xxx.md#dfx5027) | error | 无法读取原生父级默认值：%s。 |
| [DFX5030](DFX5xxx.md#dfx5030) | error | 对 '%s' 的 SavePackage 失败。 |
| [DFX5031](DFX5xxx.md#dfx5031) | error | 发射器 '%s' 声明了多个 OnEvent 块。每个发射器只能表示一个事件处理器；请拆分... |
| [DFX5032](DFX5xxx.md#dfx5032) | error | 发射器 '%s' 声明了两个名为 '%s' 的 Stage 块。Stage 按名称标识；请重命名其中一个。 |
| [DFX5033](DFX5xxx.md#dfx5033) | error | 发射器 '%s' 声明了 Stage 块，但在 CPU 上模拟。模拟阶段是 GPU 功能：设置 `SimT... |
| [DFX5034](DFX5xxx.md#dfx5034) | error | 发射器 '%s' 在两个模块调用上使用了 'as %s'。节点名称在每个发射器中必须唯一；请重命名其中一个。 |
| [DFX5035](DFX5xxx.md#dfx5035) | error | '%s' 仍在 Niagara 脚本编辑器中打开。重建已取消以保护其可编辑副本。关... |
| [DFX5036](DFX5xxx.md#dfx5036) | 警告 | OnEvent 源 '%s' 是一个 ID，本系统中没有任何发射器响应该 ID，因此此处理器永远不会触发。请将其指向... |
| [DFX5093](DFX5xxx.md#dfx5093) | 错误 | 'MaterialParam' 是保留语法，在 v1 中未实现（计划第 7 节）。 |
| [DFX5097](DFX5xxx.md#dfx5097) | 错误 | 目前只能生成 System 文档；此文件声明了一个 %s。 |
| [DFX5098](DFX5xxx.md#dfx5098) | 错误 | 数据接口参数 '%s' 采用带引号的 JSON 对象作为其配置，即反编译器所写入的形式。 |
| [DFX5100](DFX5xxx.md#dfx5100) | 错误 | '%s' 是一个 %s，在 '%s' 处没有生成的资产，并且 %s |
| [DFX5101](DFX5xxx.md#dfx5101) | 错误 | （在运行时构建） |
| [DFX5102](DFX5xxx.md#dfx5102) | 信息 | 输入 '%s' 被标记为 [StaticSwitch]。一级生成（计划 3.3）会将整个 Body 降级为单个 custo... |
| [DFX5103](DFX5xxx.md#dfx5103) | 错误 | 包 '%s' 存在于磁盘上，但无法加载。 |
| [DFX5104](DFX5xxx.md#dfx5104) | 错误 | 包 '%s' 存在，但不包含名为 '%s' 的 Niagara 脚本。拒绝覆盖它。 |
| [DFX5105](DFX5xxx.md#dfx5105) | 错误 | 无法创建包 '%s'。 |
| [DFX5106](DFX5xxx.md#dfx5106) | 错误 | 无法连接模块图。Niagara 架构拒绝了参数映射连接。 |
| [DFX5107](DFX5xxx.md#dfx5107) | 错误 | '%s' 不再与 '%s' 处的模块资产匹配，并且此构建无法重新生成它。请在支持图的地方重新构建它... |
| [DFX6001](DFX6xxx.md#dfx6001) | 错误 | （在运行时构建） |
| [DFX6002](DFX6xxx.md#dfx6002) | 警告 | （在运行时构建） |
| [DFX6003](DFX6xxx.md#dfx6003) | 错误 | （在运行时构建） |
| [DFX6004](DFX6xxx.md#dfx6004) | 警告 | （在运行时构建） |
| [DFX6005](DFX6xxx.md#dfx6005) | 错误 | '%s' 的 Niagara 编译未成功（状态 %s）。 |
| [DFX6006](DFX6xxx.md#dfx6006) | 错误 | Niagara 无法编译 '%s' 的主体：\n%s |
| [DFX6007](DFX6xxx.md#dfx6007) | 信息 | 此源中没有任何内容写入该参数，因此没有图参数可供默认值驻留，并且 Ni... |
| [DFX6008](DFX6xxx.md#dfx6008) | 错误 | '%s' 使用过期的脚本完成了编译：%s。编译后的 VM 未从本次构建的图中重新构建... |
| [DFX7001](DFX7xxx.md#dfx7001) | 错误 | 资产 '%s' 不带有 DreamFX 来源标记：它从未从此源生成，或者它是在... |
| [DFX7002](DFX7xxx.md#dfx7002) | 错误 | 资产 '%s' 已过期：它从此源的不同修订版生成。请运行 DreamFX 构建。 |
| [DFX7003](DFX7xxx.md#dfx7003) | 警告 | 资产 '%s' 由 DreamFX %s 生成；当前生成器为 %s。建议重新构建。 |
| [DFX7004](DFX7xxx.md#dfx7004) | 错误 | 资产 '%s' 不存在：此源从未被构建过。 |
| [DFX7005](DFX7xxx.md#dfx7005) | 错误 | （在运行时构建） |
| [DFX7101](DFX7xxx.md#dfx7101) | 警告 | 发射器 '%s' 在 GPU 上模拟，但未声明 FixedBounds。GPU 发射器无法计算自身的边界... |
| [DFX7102](DFX7xxx.md#dfx7102) | 警告 | 发射器 '%s' 按速率（'%s'）生成且没有上限。设置 AllocationMode = Fixed 并将 PreAllocationCount 设... |
| [DFX7103](DFX7xxx.md#dfx7103) | 警告 | 发射器 '%s' 使用了随机性（'%s'），但未设置 Determinism = true，因此它在每次... 时看起来都会不同 |
| [DFX7104](DFX7xxx.md#dfx7104) | 警告 | 此渲染器没有材质，并且 DreamFX 不知道其类型的默认材质，因此它不会绘制。设置 'Material...' |
| [DFX7105](DFX7xxx.md#dfx7105) | 警告 | MeshRenderer 的 OverrideMaterials 非空，但材质覆盖已禁用。设置 bOverrideMaterials = tr... |
| [DFX7106](DFX7xxx.md#dfx7106) | 信息 | pull：%s.%s 存储在资产上，未在此文本中声明 -- 已跳过。pull 不添加任何结构。 |
| [DFX7107](DFX7xxx.md#dfx7107) | 信息 | pull：'%s' 已包含 '%s' 为其声明的输入存储的每个值（比较了 %d 个，未声明 %d 个...） |
| [DFX7108](DFX7xxx.md#dfx7108) | 错误 | pull 读取 .dfs 及其指定的 Niagara 系统：'%s' 声明了一个 %s 文档。 |
| [DFX7109](DFX7xxx.md#dfx7109) | 警告 | pull：%s：%s。pull 重写双方都认同的结构内的字面量，因此此堆栈中没有任何内容被... |
| [DFX7110](DFX7xxx.md#dfx7110) | 错误 | pull：重写后的文本无法回读（%s），因此 '%s' 保持不变。 |
| [DFX7111](DFX7xxx.md#dfx7111) | 信息 | pull：声明了 %s，但未写回：%s |
| [DFX7112](DFX7xxx.md#dfx7112) | 信息 | pull：%s：%s |
| [DFX7113](DFX7xxx.md#dfx7113) | 警告 | pull：%s：资产具有文本所没有的 %s，而文本具有资产所没有的 %s。这两个列表... |
| [DFX7114](DFX7xxx.md#dfx7114) | 警告 | pull：无法使用基线（%s），因此此文本声明的每个值都从头进行了比较。 |
| [DFX7115](DFX7xxx.md#dfx7115) | 信息 | pull：%s 与此文本不同，但资产自上次应用以来未发生变化，因此文本的值有效。 |
| [DFX7116](DFX7xxx.md#dfx7116) | 信息 | pull：%s：%s -> %s |
| [DFX8000](DFX8xxx.md#dfx8000) | 错误 | 无法反编译空系统。 |
| [DFX8001](DFX8xxx.md#dfx8001) | 错误 | 无法读取发射器：%s |
| [DFX8002](DFX8xxx.md#dfx8002) | 警告 | 跳过发射器 '%s'：%s |
| [DFX8003](DFX8xxx.md#dfx8003) | 错误 | 无法反编译空发射器。 |
| [DFX8004](DFX8xxx.md#dfx8004) | 错误 | 无法创建用于读取发射器的宿主系统：%s |
| [DFX8005](DFX8xxx.md#dfx8005) | 错误 | 无法将发射器 '%s' 复制到宿主系统：%s |
| [DFX8006](DFX8xxx.md#dfx8006) | 错误 | 无法读取发射器 '%s'：%s |
| [DFX8010](DFX8xxx.md#dfx8010) | 错误 | 无法收养 '%s'：'%s' 尚无 DreamFXLang 形式，因此收养会在首次重新构建时销毁它... |
| [DFX8011](DFX8xxx.md#dfx8011) | 错误 | 无法收养 '%s'：源目标或另一源已存在于 '%s'。请改为编辑该文件；Ado... |
| [DFX8012](DFX8xxx.md#dfx8012) | 错误 | '%s' 已被收养，但重新导出重建后的资产无法重现此文件。首个差异位于 %s |
| [DFX8013](DFX8xxx.md#dfx8013) | 错误 | 此文件位于反编译输出目录中，但 Name=\"%s\" 构建了 '%s'，位于 '%s/' 命名空间之外.... |
| [DFX8014](DFX8xxx.md#dfx8014) | 警告 | 发射器 '%s' 继承自 '%s'。导出会扁平化继承：合并后的堆栈被完整携带，... |
| [DFX8015](DFX8xxx.md#dfx8015) | 警告 | 发射器 '%s' 携带 %d 个此导出无法表示的事件处理器%s。重建后的发射器将获得 n... |
| [DFX8016](DFX8xxx.md#dfx8016) | 警告 | 发射器 '%s' 携带 %d 个此导出无法表示的模拟阶段（自定义阶段类或缺少 scr...） |
| [DFX8017](DFX8xxx.md#dfx8017) | 警告 | 发射器 '%s' 使用不受支持的 Lightweight/Stateless 模式。将此发射器保留在 Niagara 中；DreamFX 无法重新... |
| [DFX8018](DFX8xxx.md#dfx8018) | 错误 | '%s' 未保存：重新构建它会丢弃 %d 个此源无法表达的事实，而保存会销毁... |
