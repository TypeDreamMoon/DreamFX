# DreamFX

用文本源码编写 Niagara 特效，编译生成标准 Niagara 资产。DreamShader 对材质做的事，DreamFX 对 VFX 再做一遍。

**文本是唯一真相，`.uasset` 是构建产物，不手改。**

| 后缀 | 内容 | 生成资产 |
| --- | --- | --- |
| `.dfs` | System：user 参数、system stack、emitter 列表 | `UNiagaraSystem` |
| `.dfe` | 可复用 Emitter（被 `.dfs` 以 `from` 拷入，自身不生成资产） | —— |
| `.dfm` | Module / Dynamic Input | `UNiagaraScript` |

## 快速开始

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Effects/NS_Hello.dfs
```

完整一遍：[Docs/getting-started.md](Docs/getting-started.md)。语法参考：[Docs/language/](Docs/language/README.md)。
诊断码逐条：[Docs/diagnostics/](Docs/diagnostics/README.md)。

## 能力清单

### 已通

| 能力 | 说明 |
| --- | --- |
| 文本 → Niagara 资产 | `.dfs` 全量重建，包路径与 user 变量名跨重建稳定（plan 4.5 身份契约） |
| 诊断回映射 | 133 个 `DFXnnnn`，每条带文件、行、列 |
| 全部值模式 | 字面量 / linked / enum / dynamic input（含嵌套链）/ `hlsl { }` / `curve { }` 带切线 |
| Renderer | schema 驱动的通用属性赋值（L8），任意 renderer 类型无需专门语法；`Bind` 走 binding 自己的 setter |
| 内联表达式 | L6 白名单内降为单个 HLSL 表达式 |
| user 参数 | `Properties` 段，含资产与 DI 类型声明 |
| `.dfm` 生成 | Module 支持多语句 body；DynamicInput 单表达式。**stock 引擎同样能生成**：`FGraphSurgeon` 在引擎不导出那五个声明时用公开面重建同样的图手术，形状对不上就拒绝生成并点名。产物与直调路径 schema 逐行一致(含跨引擎) —— [Docs/language/dfm.md](Docs/language/dfm.md) |
| 反编译 | 资产 → 源码，逐字节幂等；默认值抑制走探针基线。表达不了的东西**逐条写进文件头注释**，绝不静默丢 |
| Decompiled 命名空间 | 导出件的 `Name=` 落 `Decompiled/<原目录>/<资产名>` ⇒ 重建的是镜像，**结构上碰不到原资产**。整棵 `DFX/Decompiled/` 因此是一等源码：存盘即重编、lint / build / CI 一视同仁（plan-v4 V1） |
| 编辑器集成 | Tools 菜单 / 关卡工具栏 / Content Browser 右键（System 两态 + Emitter Export .dfe）/ Niagara 系统编辑器工具栏 / VSCode workspace；全部走既有管线，`-NoDreamFXEditor` 一键关掉 —— [Docs/tools/editor-integration.md](Docs/tools/editor-integration.md) |
| 接管外来资产 | 右键 **Adopt**：反编译落到真源码根 → 重建 → 打戳 → 再导出逐字节比对。有丢失就拒绝，因为接管的意思是「文本从此是唯一真相」 |
| 溯源戳 | 源 hash + 生成器版本 + 模块版本 GUID 清单（R7） |
| 静态输入写入 | 走 override pin —— 引擎自己的 stack UI 就是这么写的（静态参数不是 rapid-iteration 候选，必然走这条分支）。外部编辑 API 永远拒绝静态输入（静态标志参与类型相等、却无法从 `UScriptStruct` 复原），所以这不是绕行而是正路，**任何引擎都成立**。两个位置都覆盖：模块自身声明的开关（pin 在函数调用节点上）、静态类型的普通参数（pin 在 override map-set 节点上，`GetOrCreateStackFunctionInputOverridePin` 已导出且找-或-建整链全包）。**MoonEngine 为此加的引擎豁免已于 2026-08-10 退役，触面 4 → 3** |
| `@版本` 选择 | 不只是记录：`@` 指定的版本会真的选中（模块与 dynamic input 都走 `ChangeScriptVersion` + `RefreshFromExternalChanges`，少了后者拿到的是半新半旧的引脚集）。跳过 Python 升级脚本是有意的 —— 所有输入紧接着从源码重写，remap 出来的东西会被立刻覆盖。R1b；该 API 在 stock 引擎同样导出，不依赖 MoonEngine |
| 漂移检测 | `-Verify`：改了源没重建、手改了资产、依赖的模块换了版本，三种都报 |
| lint | GPU 无 FixedBounds、spawn rate 无上限、用随机没开 Determinism 等 |
| file watcher | 保存即重编；打开生成资产 + 保存文本 = 实时预览 |
| 覆盖率工具 | 扫全部已挂载 content root（`-Path` 可给多个，`+` 分隔），按特性分桶报告可 round-trip 比例；镜像资产不计入 |
| 批量导出 / 镜像比对 | `dfx decompile-all -Path=...` 一次导出整个目录；`dfx mirror-diff` 拿镜像与原资产各自反编译逐行比对（L1）并编译镜像（L2） |
| 安全改名 | `dfx rename`，保住 emitter handle 与 rapid iteration 别名（R4） |
| CI gate | lint → build → verify → corpus 四步 |
| 语料库 | 46 个 automation 测试：诊断码 + 行列、黄金拓扑、反编译幂等 |
| 4 个 skill | `dream-fx-{create,verify,diagnose,decompile}` |

### 降级

| 项 | 现状 | 原因 |
| --- | --- | --- |
| `.dfm` 生成 | 引擎形状变了才降级 | 五个声明在 stock 引擎无导出宏，但**导出与可达是两回事** —— public 数据成员本就不需要导出宏，public virtual 走 vtable，剩下的私有字段都是 UPROPERTY。反射后端据此重建，启动自检确认每个依赖的形状；只有形状真的挪了才落 DFX5100 / DFX5107，且诊断点名是哪一项。**产物从来不受限**：生成出的是标准 `UNiagaraScript`，任何引擎都能加载、引用、cook、运行 |
| `.dfm` 的 `[StaticSwitch]` | 降为普通输入 + DFX5102 | 档一把整个 body 塞进一个 CustomHlsl 节点，没有可供 switch 选择的分支 |
| DI 参数配置 | 只声明不配置，注释保全 | 全库 8 处命中全在引擎插件内容（HairStrands / VRM4U / Water），项目自有内容 0 处 —— 等真有需求再做（plan §3.5） |
| `#Region` | 只进文本 | 外部编辑 API 没有 stack note 读写函数 |
| `[Group]` / `[SortPriority]` | 只进文本 + DFX5099 | user 变量结构体没有对应元数据字段 |
| `MaterialParam` | 保留语法未实现 | 见 plan §7 |
| Emitter 继承 | `from` 是拷贝不是继承（R3） | 真继承需要 merge manager，另立项 |

### 未做

Event handler 栈、具名 Simulation Stage、Scratch Pad、模块内部图 lowering（档二）、GPU/CPU 条件分支、
Scalability 条件逻辑、独立实时预览、workspace 面板。

前两项的语法段位已保留、解析已支持；**2026-08-07 覆盖率实测：全项目 20 个 NS 资产，用到 event handler
或 simulation stage 的是 0 个**，所以继续挂起有据可依 —— 见
[Docs/coverage-2026-08-07.md](Docs/coverage-2026-08-07.md)。

## CI

```bash
pwsh -File Plugins/DreamFX/.skill/ci.ps1
```

| 步 | 抓什么 |
| --- | --- |
| `lint` | 源码静态检查，不碰资产，失败最快 |
| `build` | 每个 `.dfs` / `.dfm` 都能生成且 Niagara 编译干净 |
| `verify` | **改了源没重建就一起提交了** —— 单跑 build 一定通过，因为 build 会把它修好 |
| `corpus` | 行为变了（不是坏了）：诊断位置漂移、拓扑变化、反编译丢幂等 |

`-SkipCorpus` 跳过第四步（它要起编辑器，比前三步加起来还贵）；`-StrictVersions` 把 R7 版本漂移
从 warning 升成 error，给发布分支用。

**跑 CI 前先关编辑器**：`build` 和 `corpus` 都写 `.uasset`，编辑器开着同一个工程也在写，
谁后存谁赢，而且两边都不吭声。commandlet 启动时探到编辑器进程会警告一句，但它分不清那个编辑器
开的是不是本工程，所以只是提醒、不是门禁。

构建耗时的实测与结论见 [Docs/performance-2026-08-08.md](Docs/performance-2026-08-08.md)。

## 结构

```
Plugins/DreamFX/
├── DFX/                        源文件：Samples / Emitters / Modules
├── Docs/                       getting-started · language/ · tools/ · diagnostics/
├── Plan/                       plan.md · plan-v2…v7.md（本地工作笔记，gitignore）
├── Tests/Corpus/               Parse / Generate / RoundTrip 语料
├── .skill/                     dfx.ps1 · ci.ps1 · gen-diagnostics.ps1 · 4 个 skill
└── Source/
    ├── DreamFX/                Runtime：词法、语法、AST、诊断
    └── DreamFXEditor/          Editor：adapter · schema · 生成 · 反编译 · commandlet · watcher · 测试
```

设计与逐期实测记录见 [Plan/plan.md](Plan/plan.md)，收尾工作项见 [Plan/plan-v2.md](Plan/plan-v2.md)，
编辑器集成与完整反编译见 [Plan/plan-v3.md](Plan/plan-v3.md)，Decompiled 命名空间与内容包
等价往返见 [Plan/plan-v4.md](Plan/plan-v4.md)，四包跑通（v4-V3 续篇）见
[Plan/plan-v5.md](Plan/plan-v5.md)，生成性能与工作区入库见 [Plan/plan-v6.md](Plan/plan-v6.md)，
预编译引擎支持见 [Plan/plan-v7.md](Plan/plan-v7.md)。
`Plan/` 在本仓库 gitignore 中，只存在于磁盘上；`Docs/` 随插件入库。
