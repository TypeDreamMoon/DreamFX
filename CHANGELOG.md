# Changelog

## Unreleased

### 不兼容变更

- 生成器版本升至 `1.5`：已有生成资产在下一次 build 时全部重新生成（依赖感知的源码指纹、继承、VM 完成校验与修正后的 `.dfm` 写回语义）。
- `.dfm` Body 中新声明的粒子属性（`float Particles.X = ...;`）必须在顶层无条件初始化，且同一属性不能以不同类型声明，否则报 `DFX3057`。此前条件分支内的首次声明可以编译，但未执行分支时输出未初始化。只读取上游模块写入的属性时，先写不带初始化的类型声明。
- `asset-diff` 把缺失镜像、无法解析的内容根和编译失败计入退出码；系统级可编辑运行设置参与比较。
- Adopt 不再覆盖已存在的规范源码路径（`DFX8011`），Bridge 的 Adopt 同样拒绝。
- **构建安全闸（`DFX8018`）**：重建会丢掉文本无法表达的事实（未声明的渲染器属性、被清空的材质参数绑定等）时拒绝保存并逐条列出（[#48](https://github.com/TypeDreamMoon/DreamFX/pull/48)）。编辑器内的保存重建、Adopt、Bridge 没有覆盖开关；命令行 `-Force` 同时覆盖该闸并以警告记录丢失项。被拒绝的构建不写溯源记录；编辑器会话中会从磁盘重新加载该资产，避免 Save All / 自动保存写入被拒绝的结果。

### 新增

- **增量构建**：每次成功的系统构建把「计划摘要」（`DreamFX.PlanDigest`）写进资产的包元数据，与溯源记录并列。摘要是展平后的构建计划（`.dfe` 合并、继承与模块版本已解析）的哈希树，加上构建给每个模块起的节点名，以及资产构建安全事实的哈希。下一次增量构建拿新计划与它比较，只写差异：改变的参数值被写入；动态输入链形状改变的参数先重置再整组重写；源码不再设置的字面量/枚举参数、曲线等数据接口值的变化，以及模块、顺序、版本、`as` 名、`disabled`、Set Parameters 条目或静态开关变了的堆栈单独清空重建（按完整重建的顺序：先清空、再清理残留常量、再添加）；新增的发射器、父级（`inherits`）或 `SimTarget` 变了的发射器、以及其堆栈与另一个被重建的堆栈共用模块的发射器整个重建，被删的发射器被移除；发射器设置、渲染器、事件处理器、模拟阶段、参数默认值、用户参数和系统设置只在变化时重新应用。结果必须与对同一文本做完整重建得到的资产相同，完整重建仍是对照基准。1230 行的系统（`NS_Magic_Explosion`）改一个值：编辑器内完整重建每次存盘 17 秒（MoonEngine 构建）/ 56 秒（原版预编译 UE 5.8，写入 API 更慢），增量构建约 1 秒（原版 5.8 约 1.7 秒），大部分是 Niagara 对系统的重编译；单独重建两个堆栈约 2.5 秒；原版预编译引擎上增量只覆盖改值和增删发射器，其余结构性改动走完整重建（那个引擎每次编辑都发起编译，被隐藏输入的常量最终取值取决于编译的先后顺序，局部重建与整体重建落不到同一个资产上）；命令行整个 commandlet 约 3 秒，大部分是进程启动和编辑器会缓存的冷模块探测。增量构建请求 Niagara 做普通（非强制）编译，只重编需要重编的脚本；完整重建仍强制编译，构建结束后照旧校验编译状态、发现过期则强制重编。以下情况自动退回完整重建，Output Log 会写明原因（如 `full rebuild (the asset changed since its last build)`）：资产是新的；没有存储的计划（旧版 DreamFX 构建或 Adopt 得到）；存储的计划格式、生成器版本或写入路径（MoonEngine 快速编辑 / 原版引擎）不同；资产的事实与上次构建记录的不符，即在 DreamFX 之外被改过（Niagara 编辑器里手改、源码管理回退）；发射器被重排；数据接口按位置引用发射器或渲染器（自引用 token），而发射器或渲染器集合变了；增量写入本身失败（同一次构建里改跑完整重建并报告它自己的诊断）。详见 `Docs/tools/incremental-builds.md`。
- 编辑器里增量构建默认只用于源码监视器的存盘触发重建，设置项为 *Project Settings ▸ Plugins ▸ DreamFX ▸ Workspace ▸ Incremental Build on Save*（`bIncrementalBuildOnSave`，默认开）。*Rebuild DFX*、*Rebuild from Source* 和批量积压的「Rebuild them now」始终完整重建，是退路。
- 命令行增量构建默认关闭（CI 和闸门要的是完整重建的答案）：`dfx.ps1 build <源码> -Incremental` / `UnrealEditor-Cmd <uproject> -run=DreamFX -File=<源码> -Incremental` 开启。commandlet 与监视器每个文件记一行，如 `incremental: 0 emitter(s) and 1 stack(s) rebuilt, 3 input(s) written, 1 reset, nothing to recompile` 或 `full rebuild (<原因>)`；生成器动手前也会记下要改动的范围，如 `incremental build -- Sparks ParticleSpawnScript values(+0 ~1 -0)`。闸门不要用该开关。
- 规划阶段每个进程复用同一个模块库：构建所需的模块探测在一次编辑器会话 / 一次 commandlet 运行里只做一次，不再每次构建都做。
- `dfx.ps1 pull <源码>`：把资产中实际存储的值写回文本已声明的字面量（默认试运行，`-Apply` 写入，`-Structure` 写回可唯一定位的结构差异，`-Asset` 指定读取的资产）。只改动值所占的字符，保留注释、换行和文件编码；写前备份到 `Saved/DreamFX/Pull/`（`DFX7106`–`DFX7116`）（[#48](https://github.com/TypeDreamMoon/DreamFX/pull/48)）。
- 系统 `Settings` 新增 `UseFixedBounds`（`bFixedBounds`）：声明 `FixedBounds` 时默认开启，可显式写 `false` 保留不生效的包围盒；反编译在需要时写出该项，往返保持一致。
- `System(..., Parent="../Base.dfs")` 源码级系统继承：Settings / Properties / 系统栈 / 发射器按名称合并，父链及其 `.dfe` 参与指纹与监听（[#5](https://github.com/TypeDreamMoon/DreamFX/issues/5)）。
- `Emitter X inherits "/Game/NE_Parent" version "GUID"` 保留 Niagara 原生父发射器关联，重建保留 handle GUID，支持属性、栈、渲染器、事件组和默认值覆盖（[#5](https://github.com/TypeDreamMoon/DreamFX/issues/5)）。
- 表达式与默认值支持 `int(...)` 显式截断转换（`DFX4044`）（[#36](https://github.com/TypeDreamMoon/DreamFX/issues/36)）。
- Stage 支持 generic stage 的调度、线程组覆盖与粒子状态过滤参数（[#27](https://github.com/TypeDreamMoon/DreamFX/issues/27)）；`disabled` 可修饰赋值（[#20](https://github.com/TypeDreamMoon/DreamFX/issues/20)）。

### 修复

- **构建安全事实（`DescribeSystemFacts`，供构建安全闸与 `asset-diff` 使用）只统计从系统经非瞬态引用可达的数据接口**：重建后遗留在内存里的接口（被移除的节点、`Invalidated_*` 缓存默认值、瞬态占位）不再被报告。此前同一份资产在垃圾回收或保存前后会给出不同的描述。
- **事件处理器的源不再"要么是名字、要么建不出来"**：反编译器解析不出源发射器时写 `` `<unresolved GUID>` ``（复制系统没重映射 handler id 的典型形状——多个系统会共用同一个陈旧 id），无效 id 写 `` `<none>` ``，而写侧只接受发射器名 ⇒ 读侧产出写侧拒绝的 token，这类文本永远重建不了（与 `Docs/tools/write-back-coverage.md` §3.3 同型）。现在两种 token 都按原样回写，资产保持它原有的 id；悬空 id 额外报 `DFX5036` 警告——那类处理器本来就不触发（运行时用 id 的字符串形式建查找名、再按名字取事件集，`NiagaraEmitterInstanceImpl.cpp:418-419`、`:1307`），但这是资产的事实，不该是拒绝构建的理由。新增往返语料 `Tests/Corpus/RoundTrip/EventHandlerUnresolvedSource.dfs`。

- 源码位置按 DFX 根与根内相对路径判断：同一源码树在另一台机器、CI 或测试宿主的检出路径下不再被当成"已移动"而全量重建、重存资产；溯源记录的绝对路径不存在时，Open Source / Rebuild 按根与相对路径找到源码（[#37](https://github.com/TypeDreamMoon/DreamFX/issues/37)）。
- 源码监听只递归监听 DFX 根；根的上级目录与外部依赖目录改为只报告直接子项的浅监听，不再递归监听整个工程目录（Saved / Intermediate / DDC）乃至盘符根。被整体替换的已监听目录会重新订阅（[#38](https://github.com/TypeDreamMoon/DreamFX/issues/38)、[#44](https://github.com/TypeDreamMoon/DreamFX/issues/44)）。
- MoonEngine 直连后端：`.dfm` 属性的 HLSL 符号按引擎引脚唯一化规则（忽略 FName 数字后缀）避让，`Particles.A.B` 与 `Particles.A_B` 等同名碰撞不再因引擎重命名引脚而编译失败（[#43](https://github.com/TypeDreamMoon/DreamFX/issues/43)）。
- 表达式浮点常量取能精确还原的最短写法（`0.1` 不再写成 `0.10000000000000001`）（[#34](https://github.com/TypeDreamMoon/DreamFX/issues/34)）。
- 命令行 `decompile -Out` 统一写 UTF-8（无 BOM）；此前含非 ASCII 名称或说明时写成 UTF-16LE。
- `dfx.ps1` 只在 `-CleanNew` 时要求资产快照完整，普通 build 不再因无法读取的子目录失败；每次运行的独立日志只保留最近 20 份。
- 导出镜像被拒绝时，通知重新附上该镜像已有源码的链接。
- 无头构建校验 VM 编译结果；模块及原生父发射器的编译输入参与增量指纹（[#6](https://github.com/TypeDreamMoon/DreamFX/issues/6)）。MeshRenderer 的无效 `Material` 报错、未启用的 `OverrideMaterials` 警告（`DFX7105`）（[#7](https://github.com/TypeDreamMoon/DreamFX/issues/7)）；DI / 实例类型默认值在加载资产前报告（`DFX4043`）（[#8](https://github.com/TypeDreamMoon/DreamFX/issues/8)）。
- Adopt 在读取资产前和确认后都检查目标冲突，并以不覆盖目标的方式发布源码（[#29](https://github.com/TypeDreamMoon/DreamFX/issues/29)）；Bridge decompile 返回真实导出状态与路径（[#31](https://github.com/TypeDreamMoon/DreamFX/issues/31)）；反编译转义用户参数说明（[#35](https://github.com/TypeDreamMoon/DreamFX/issues/35)）；Lightweight / Stateless 发射器报告覆盖缺口（`DFX8017`）（[#40](https://github.com/TypeDreamMoon/DreamFX/issues/40)）。
- `.dfm` Body 扫描跳过注释（[#39](https://github.com/TypeDreamMoon/DreamFX/issues/39)）、正确识别 `return`（[#42](https://github.com/TypeDreamMoon/DreamFX/issues/42)）、条件写入保留旧值（[#41](https://github.com/TypeDreamMoon/DreamFX/issues/41)），`++` / `--` 与复合赋值写回属性（[#16](https://github.com/TypeDreamMoon/DreamFX/issues/16)）。
- `.dfe` 内容参与宿主增量构建和 verify；监视器会重建引用它的宿主。`from` 按 Stage 名称合并，并按参数名合并 `Defaults`，类型冲突会报错。
- `.dfm` 分量写入会保留未写分量并接入 Parameter Map Set；DynamicInput 的 Usage 数组通过 lint。移除系统或 emitter Settings 后，重建恢复引擎默认值。
- 反编译保留禁用赋值、材质用户参数绑定和完整的 generic Stage 调度与粒子状态过滤设置；5.6 / 5.7 兼容后端保留 emitter 启用状态。
- Bridge 全量 verify 同步执行只读验证；编辑器构建统一关闭并重开目标模块编辑器，取消关闭时中止。监视器发现新 DFX 根目录，生成资产警告等待编辑器初始化后注册。
- `-CleanNew` 仅清理本次新建资产，扫描或 Git 检查失败时保留资产；驱动只读取本次独立日志。SimCache 采集失败或零帧不再误报等价。
- CI 检查诊断文档、驱动回归和全部 DreamFX automation suites；新增生成、往返和工作区回归测试。
- **5.6 / 5.7:静态开关暴露的条件输入现在看得见了**([#1](https://github.com/TypeDreamMoon/DreamFX/issues/1))。没有外部编辑 API 的引擎走 `Compat/` 那一层,
  而它枚举模块输入时在第一个 `UNiagaraStackFunctionInput` 就停手。模块输入是一棵**层级**:开关揭示的输入是
  该开关的**子节点**,不是它的兄弟——`InitializeParticle` 的 `Lifetime` 挂在 `LifetimeMode` 下,
  `UniformSpriteSize` 挂在 `SpriteSizeMode` 下。于是探测只看得到 13 个顶层输入,写开关像是没生效,
  它揭示的每个实参都被当成拼写错误(DFX3003)。改成引擎自己那两条规则:进入输入的子节点,
  在 Dynamic 输入处停下(那是动态输入链,归 `GetDynamicInputChain`)。写入路径同一个解析器,
  所以条件输入现在也能寻址。5.8 与 MoonEngine 用引擎自带 API,从来不走这条路,不受影响。
- **5.6 / 5.7:输入的可见 / 可编辑改用引擎的三条门。** `GetIsHidden` + VisibleCondition + EditCondition,
  与 `GetStackInputTopology` 逐字一致。原先的 `GetShouldShowInStack` / `GetIsEnabled` 在这两个引擎上
  分别只回答「只显示已修改」过滤器和宿主节点的启用状态,一律报 true——顶层输入没有东西会隐藏它们,
  所以此前无害;一旦开始收集条件子节点就不再无害,未被选中的分支也是存在且隐藏的子节点。
  反编译器按 `!bVisible || !bEditable` 跳过输入,是会被骗到的那个读者。
- **保存失败不再让编辑器崩溃。** 四处 `UPackage::SavePackage`(adapter 落盘、模块生成、模块库、
  Phase0 commandlet)都用了 `FSavePackageArgs` 的默认 `Error = GError`——GError 上的任何一条消息都是
  fatal,于是「另一个程序正在使用此文件」(两个编辑器的源码监视器同时重建同一个 `.dfs`、只读文件、
  杀软占用)直接把编辑器杀掉,`SavePackage failed for ...` 那条错误路径从来到不了。改为
  `SaveArgs.Error = GWarn`,失败回到构建错误。
- DFX7102 不再把 `SpawnParticlesInGrid` 当作按速率生成:它和 `SpawnBurst` 一样是每循环一次的
  burst(X·Y·Z),上限由循环决定。列表只留 `SpawnRate` / `SpawnPerUnit` / `SpawnPerFrame`。
- `dfx.ps1` 解析 `EngineAssociation` 时,注册表里没有该引擎就改查 Epic Games Launcher 的安装清单(`LauncherInstalled.dat`),launcher 安装的 `5.8` 不再需要手设 `UE_ENGINE_ROOT`。

### 文档

- `.dfm` Body 里调用 `DI<X>` 输入的函数(`Wind.SampleWind(Particles.Position, V, G)`)时,`out` 实参必须是
  **未初始化的局部变量**:`float G = 1.0;` 会让 CPU VM 编译报
  `out/inout parameters must be lvalues`(DFX6006)——hlslcc 先把带初值的局部常量折叠进实参再查 out
  形参。GPU 两种写法都过;按未初始化写,一份 Body 两端通。见 `Docs/language/dfm.md`。
  首个外部插件 DI(DreamWind 的 `UNiagaraDataInterfaceDreamWind`)经 `DI<DreamWind>` 反射解析、
  模块输入接线、`.dfs` 里 `User.Wind` 传参全线打通,无需 DreamFX 改动。

## 1.0.0 — 2026-08-13

首个正式版本。文本源码(`.dfs` / `.dfe` / `.dfm`)→ 标准 Niagara 资产
(`UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript`),反方向的完整反编译,
以及一套把「每个声明都带测量」当作纪律的验证体系。

### 语言与生成

- `.dfs` 全量系统定义:user 参数(含 **DI 参数带 JSON 配置**)、system/emitter 六固定栈、
  **OnEvent 事件处理器**、**具名 Simulation Stage**(迭代源 / 绑定 / `ExecuteBehavior` /
  `NumIterations`·`Enabled` 值或参数双形态)、renderer 通用属性赋值与 `Bind`、
  Settings(含 `bFixedTickDelta`、系统级 `FixedBounds`)。
- 全部值模式:字面量 / linked / enum / 嵌套 dynamic input / `hlsl { }` /
  `curve { }`(带 `Interp` 与 **`Tangent=Auto|User|Break|None`**;可读形式表达不了的
  曲线 DI 自动退回逐字 JSON——便利没有资格丢数据)。
- 静态开关走 override pin(引擎 stack UI 的同一条路),两个家都覆盖;
  **链接形态按模块自身 DefaultBinding 解析**——相等即无操作,不等即 DFX5021,
  调用点没有链接机制,曾经的进程死已封死。
- `.dfe` 可复用 emitter 拷入;`.dfm` 模块/动态输入生成,**stock 引擎经反射后端同样可用**。
- `@版本` 真实选中;R7 溯源戳(源 hash + 生成器版本 + 模块版本 GUID)。

### 反编译与镜像

- 任意 NiagaraSystem → `.dfs`,逐字节幂等;表达不了的东西逐条写进文件头缺口注释,绝不静默。
- `Decompiled/<原目录>` 命名空间:导出即一等源码,存盘即重编,结构上碰不到原资产。
- 编辑器集成:菜单 / 右键 / 工具栏 / VSCode workspace / Adopt 接管(有丢失就拒绝)。

### 验证体系(四层)

- L1 `mirror-diff` 文本逐行;L2 镜像编译;L3 运行时等价(SimCache 固定步长逐帧粒子数,
  A-vs-A 自对照裁决可判定性);**资产级事实对比 `asset-diff`**(不经导出器的反射走查,
  含 `compiled` 事实族,两侧先强制编译)。corpus 55 条同时比「夹具建的资产」与
  「其导出重建的资产」——对称丢失自 2026-08-12 起不再隐形。
- CI 四步 lint → build → verify → corpus;`gen-diagnostics.ps1 -Check` 防文档漂移;
  143 个诊断码全带文件/行/列。

### 双引擎

- MoonEngine(源码 5.8.1)与 stock installed 5.8.1 **同一份源码零 `#if` 分叉**;
  MoonEngine 触面 3 条全记账。
- 1.0.0 发布轮在 stock 上根治四个长进程缺陷:UObject 计数 GC 门、收集与活编辑面的
  时序安全、无默认值 user 参数被引擎静默丢弃的兜底、迭代中创建对象的致命修改;
  外加宿主项目内容插件对齐要求(见 README「运行环境要求」)。

### 发布验证快照(2026-08-13,commit `4e23c55`)

| 门 | Moon(源码 5.8.1) | stock(installed 5.8.1) |
| --- | --- | --- |
| ci(lint→build→verify→corpus) | OK,55 verified / corpus 55 | —(corpus 55/55 单跑) |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0(连续两跑) |
| mirror-diff | L1 45/45,L2 45/45 | L1 24/24,L2 24/24 |
| asset-diff | 6 same / 39 different(全部为已宣告族) | — |
| L3 运行时等价 | 45 对:7 exact / 0 phased / **0 differ** / 38 undecidable(无种子随机,自对照判) | — |
| 诊断文档 `-Check` | 绿 | — |

### 已知问题(1.0.0)

1. 嵌套动态输入节点上的**链接**宣言开关(深度>1)未防护——引擎缺陷可达但语料零命中;
   计划 1.0.x 扩守卫覆盖深度;
2. emitter 级 `FixedBounds` 未携带(系统级已带);
3. `bGpuAlwaysRunParticleUpdateScript` 未携带;
4. DI 配置为原样 JSON 承载(可读语法挂起);裸 `DI<T>` + 链接形态的默认实例残骸计数不稳
   (判据噪音而非数据丢失);
5. UE 5.8.1 refPath 导入回归——引擎 bug,已绕过(`NormalizeObjectReferences`),
   5.8.2 复测后拆除;
6. L4 画面对比为人工可选步骤(OBS 配方已验证,未脚本化);
7. 长尾照 README 未做栏(自定义 C++ stage 类、参数驱动迭代次数等,缺口头点名,语料无实例)。

宿主项目内容插件对齐(如 NiagaraFluids)是**环境要求**而非缺陷,见 README「运行环境要求」。
