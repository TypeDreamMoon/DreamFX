---
title: dfx.ps1 命令行
description: headless 驱动的每一个子命令与参数：build、verify、lint、decompile、mirror-diff、asset-diff、coverage、schema、index、corpus。
---

`dfx.ps1` 包着 `UnrealEditor-Cmd.exe <project> -run=DreamFX …`，让一次构建变成一条命令、
一个退出码，而不是几百行引擎启动噪音。

```bash
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 <command> [target] [switches]
```

它在原始 commandlet 之上还做了几件事：从 `.uproject` 的 `EngineAssociation` 解析引擎、
从目标文件向上找项目、去掉重复的日志回声、**报告这次运行写了哪些资产**并按 git 分类
（一次性探针资产 vs 刚被覆盖的受跟踪资产），以及 `-CleanNew`（只删这次运行新建出来的未跟踪资产）。

**警告：**

**写资产的命令要关编辑器**（`build`、`corpus`、`mirror-diff`、`decompile-all`）。
  两个进程存同一批包，谁后存谁赢。看见编辑器进程会警告，但它不知道那个编辑器开的是哪个项目 ——
  所以是警告不是拒绝。

<a id="命令一览"></a>

## 命令一览

| 命令 | 干什么 | 写资产 |
| --- | --- | :--: |
| `build` | 从源码生成资产 | ✓ |
| `verify` | 对照源码检查资产，什么都不写 | |
| `lint` | 只做静态检查，不碰资产 | |
| `decompile` | 一个资产 → 源码 | |
| `decompile-all` | 一整棵内容树 → 源码，一次 boot | ✓ |
| `mirror-diff` | L1 文本 + L2 编译 | ✓ |
| `asset-diff` | 资产事实反射走查（两侧先强制编译） | ✓ |
| `coverage` | 一棵树里有多少能被表达，按特性分桶 | |
| `rename` | `<asset>:<oldName>:<newName>` 改 emitter 名 | ✓ |
| `schema` | 一个模块的真实输入签名 | |
| `list` | 搜索路径上的每个模块（`-DynamicInputs` 换成动态输入） | |
| `index` | 写 `DFX/.dfx-index.json`（给编辑器扩展用） | |
| `graph` | 打印模块依赖图 | |
| `corpus` | 跑 `DreamFX.Corpus` 自动化测试套件 | ✓ |

<a id="build"></a>

## `build`

```bash
dfx.ps1 build DFX/Effects/NS_Hello.dfs        # 一个文件
dfx.ps1 build -All                            # 每个根下的每个源码
dfx.ps1 build DFX/Effects/NS_Hello.dfs -Force # 无视 hash
dfx.ps1 build -All -NoSave                    # 只在内存里建，不写包
```

| 参数 | 作用 |
| --- | --- |
| `-All` | 每个 DFX 根下的每个源码，顺序是模块 → emitter → 系统 |
| `-Force` | 越过 hash 跳过 |
| `-NoSave` | 建但不写包 |
| `-CleanNew` | 删掉这次运行新建的、git 未跟踪的资产 |
| `-Window <n>` | 编译流水线深度；`1` 退回完全串行 |
| `-ForceReflectionBackend` | `.dfm` 走反射后端（见[引擎与后端](../generation/engines.md)） |
| `-NoWriteScope`、`-RebuildOnStructural`、`-RebuildOnSwitch`、`-RebuildPerAdd` | 各自退回一种旧的、慢的做法。A/B 与逃生用 |

<a id="verify--lint"></a>

## `verify` / `lint`

```bash
dfx.ps1 verify -All
dfx.ps1 verify -All -StrictVersions     # 模块版本漂移视为错误
dfx.ps1 lint DFX/Effects/NS_Hello.dfs
```

见[溯源与 verify](../generation/provenance.md)。

<a id="decompile--decompile-all"></a>

## `decompile` / `decompile-all`

```bash
dfx.ps1 decompile /Game/VFX/NS_Explosion                       # 打印
dfx.ps1 decompile /Game/VFX/NS_Explosion -Out DFX/NS_X.dfs     # 写文件
dfx.ps1 decompile /Game/VFX/NS_Explosion -Root Plugin.MoonToon # 指定 Root=
dfx.ps1 decompile /Game/VFX/NS_Explosion -NoDefaults           # 打印每个输入（诊断用）
dfx.ps1 decompile-all -Path=/Game/FX+/Game/Explosions          # 多个路径用 + 分隔
```

<a id="mirror-diff--asset-diff"></a>

## `mirror-diff` / `asset-diff`

```bash
dfx.ps1 mirror-diff -Path /Game/FX                # L1 + L2
dfx.ps1 mirror-diff -Path /Game/FX -NoCompile     # 只比文本
dfx.ps1 asset-diff  -Path /Game/FX -DumpFacts     # 两侧完整事实写到 Saved/DreamFX/*.facts
```

`-DumpFacts` 值得记住：控制台报告把每条事实截到 400 字符，而这恰恰对不上 —— 要追的差异
经常就住在那道线后面，`compiled` 事实族尤其如此。

<a id="schema--list--index"></a>

## `schema` / `list` / `index`

```bash
dfx.ps1 schema GravityForce
dfx.ps1 schema InitializeParticle -Stack ParticleSpawn
dfx.ps1 list
dfx.ps1 list -DynamicInputs
dfx.ps1 index                 # 写 DFX/.dfx-index.json
dfx.ps1 index -NoInputs       # 跳过输入探测，快
dfx.ps1 index -Retry          # 清空隔离名单重来（引擎升级后用）
```

**警告：**

**有些模块图走不动。** `/Niagara/Modules/Masks/ConeMask`（引擎自带内容）会让
  `UNiagaraGraph::ReferencesStaticVariable` 无限递归，栈溢出直接结束进程 —— `dfx schema`
  对它一直是这样。插件没法给引擎代码加 visited 集合，栈溢出也不值得去 catch，
  所以这趟走查是**可续的**：即将探测的模块先记下来，下次启动时还留在记录里的那个被隔离。
  `dfx.ps1` 会重跑直到走完，于是几个坏图的代价是几次额外 boot，而不是这个功能没了。
  被隔离的模块保留名字、路径和栈，只缺输入，而且它会说出来。

<a id="corpus"></a>

## `corpus`

```bash
dfx.ps1 corpus                       # 全部 DreamFX.Corpus 套件
dfx.ps1 corpus DreamFX.Corpus.Decompile   # 一个过滤器
```

语料是**自动化测试**而不是 commandlet 模式，这样同一批断言也能从编辑器的 Session Frontend 跑，
不需要第二套实现。代价是它要 boot 编辑器，并从日志里读判决 —— 自动化控制器不设进程退出码。

**提示：**

判决读的是 `Result={Success}` / `Result={Fail}` 的计数，同时**也**看进程退出码：
  套件中途崩溃的那个测试根本不会留下 Result 行，只数计数会把「9 个测试崩在第 6 个」
  读成「corpus OK (5 passed)」。两个信号一起看才是判决。

<a id="退出码"></a>

## 退出码

退出码是 commandlet 自己的返回值 —— 也就是**错误数**，而不是进程退出码。

原因是量出来的：在这个项目上两者会分歧 —— `verify -All` 稳定返回 0、任何类别都不记错误、
干净关闭，进程码却是 3。逐项排除过（teardown 阶段的 abort、verify 跳过的统计报告、
未存盘的脏包、Angelscript 的警告、verify 路径本身），结论是 commandlet 下游有东西在不吭声地
改掉这个码。所以驱动脚本从日志里读 `finished execution (result N)` 那一行。

这不是在忽略失败：非零仍然是失败，跑不到那一行就沿用进程报的码。它只是把一个代理量
换成了那个代理本来要代表的值。
