---
title: "更新日志"
description: "DreamFX 每个版本涵盖的内容，以及 1.0.0 的验证快照与已知问题。"
---

版本号跟着插件走（`VersionName` 中的 `DreamFX.uplugin`）。本页与仓库的
[CHANGELOG.md](https://github.com/TypeDreamMoon/DreamFX/blob/main/CHANGELOG.md) 保持一致。

<a id="unreleased"></a>

## 未发布

<a id="fixed"></a>

### 修复

- **5.6 / 5.7：由静态开关揭示的输入重新可见**
  （[#1](https://github.com/TypeDreamMoon/DreamFX/issues/1)）。没有外部编辑 API 的引擎走
  `Compat/` 层，而它在第一个 `UNiagaraStackFunctionInput` 处就停止枚举模块输入。模块输入是一个**层级**：
  开关揭示的输入是该开关的**子节点**，而不是它的兄弟节点 —— `InitializeParticle` 的 `Lifetime` 挂在 `LifetimeMode` 下，
  `UniformSpriteSize` 挂在 `SpriteSizeMode` 下。因此探测只看到 13 个顶层输入，写入开关
  看起来像没有效果，它揭示的每个实参都被报告为拼写错误
  （[DFX3003](../diagnostics/DFX3xxx.md#dfx3003)）。现在它遵循引擎自己的两条规则：向下
  进入输入的子节点，在 Dynamic 输入处停止。写入路径使用同一个解析器，因此
  条件输入也可寻址。5.8 和 MoonEngine 使用引擎自带的 API，从不走
  这条路径。
- **5.6 / 5.7：输入的可见性和可编辑性现在使用引擎的三道门。** `GetIsHidden` +
  VisibleCondition + EditCondition，与 `GetStackInputTopology` 逐字一致。先前的
  `GetShouldShowInStack` / `GetIsEnabled` 在这些引擎上分别回答“只显示已修改”过滤条件
  以及宿主节点的启用状态，而两者对所有内容都返回 true —— 在只收集顶层输入时无害，
  一旦收集条件子节点就不再无害，因为未选中的分支是一个存在且被隐藏的子节点。反编译器会跳过
  `!bVisible || !bEditable` 上的输入，而它正是会被骗到的读取方。
- **保存失败不再让编辑器崩溃。** 四处 `UPackage::SavePackage` 位置（适配器的写入、模块生成、模块库、Phase 0 commandlet）使用了
  `FSavePackageArgs` 的默认 `Error = GError` —— 而 GError 上的任何消息都是致命错误，因此“文件
  正被另一个程序使用”（两个编辑器的监视器同时重建同一个 `.dfs`、只读文件、
  杀毒软件占用它）会直接杀掉编辑器，而
  [DFX5030](../diagnostics/DFX5xxx.md#dfx5030) 路径从未到达。`SaveArgs.Error = GWarn` 将
  该失败重新放回构建错误中。
- [DFX7102](../diagnostics/DFX7xxx.md#dfx7102) 不再把 `SpawnParticlesInGrid` 视为基于速率的
  生成：与 `SpawnBurst` 一样，它是每循环一次的爆发（X·Y·Z），受循环限制。列表保留
  `SpawnRate` / `SpawnPerUnit` / `SpawnPerFrame`。

<a id="documentation"></a>

### 文档

- 从 `.dfm` 主体调用 `DI<X>` 输入的函数
  （`Wind.SampleWind(Particles.Position, V, G)`）需要 `out` 实参为**未初始化的
  局部变量**：`float G = 1.0;` 会以 `out/inout parameters must be lvalues` 使 CPU VM 编译失败
  （[DFX6006](../diagnostics/DFX6xxx.md#dfx6006)）—— hlslcc 会在检查 out 形参之前
  将已初始化的局部变量常量折叠进实参列表。GPU 两者都接受；请写未初始化的
  形式，以便同一份主体能在两个目标上编译。参见 [`.dfm`](../language/dfm.md)。首个外部
  插件数据接口（DreamWind 的 `UNiagaraDataInterfaceDreamWind`）端到端可用 —— 反射
  解析经由 `DI<DreamWind>`、模块输入接线，以及从 `.dfs` 传入的 `User.Wind` ——
  且无需任何 DreamFX 改动。

<a id="100--2026-08-13"></a>

## 1.0.0 — 2026-08-13

首个版本。文本源（`.dfs` / `.dfe` / `.dfm`）→ 标准 Niagara 资产
（`UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript`），反方向的完整反编译，
以及一套建立在“每个声明都带有测量”这一纪律之上的验证系统。

<a id="language-and-generation"></a>

### 语言与生成

- **完整的 `.dfs` 系统定义**：用户参数（包括**带
  其 JSON 配置的数据接口参数**）、六个系统/发射器栈、**OnEvent 处理器**、**具名
  模拟阶段**（迭代源、绑定、`ExecuteBehavior`、`NumIterations`/`Enabled` 作为
  值*或*参数）、渲染器属性赋值与 `Bind`、Settings（包括
  `bFixedTickDelta` 与系统级 `FixedBounds`）。
- **所有值模式**：字面量 / 链接 / 枚举 / 嵌套动态输入 / `hlsl { }` / `curve { }`
  （带有 `Interp` 与 **`Tangent=Auto|User|Break|None`**；可读形式无法表达的曲线接口
  回退为逐字 JSON —— 便利性不能以丢失数据为代价）。
- 静态开关通过 override pin 进行，与引擎自己的 stack UI 使用的路径相同，且在两种
  引擎上都是如此；**链接形式会针对模块自身的 DefaultBinding 进行解析** —— 相等即为无操作，
  不等则被拒绝，调用点没有链接机制。
- `.dfe` 可复用发射器被复制进来；`.dfm` 模块与动态输入生成，**在
  原版引擎上通过反射后端也可工作**。
- `@version` 真正用于选择；R7 溯源标记（源哈希、生成器版本、模块版本
  GUID）。

<a id="decompilation-and-mirrors"></a>

### 反编译与镜像

- 任意 `UNiagaraSystem` → `.dfs`，逐字节幂等；任何无法表达的内容都会写入
  文件头作为显式缺口，绝不静默处理。
- `Decompiled/<original path>` 命名空间：导出即一等源，保存时重建，
  且在结构上无法触碰原始资产。
- 编辑器集成：菜单、上下文菜单、工具栏、VSCode 工作区，以及 Adopt（在任何丢失时都会拒绝）。

<a id="the-four-verification-layers"></a>

### 四层验证

- L1 `mirror-diff` 逐行；L2 镜像的编译；L3 运行时等价性（固定步长 SimCache
  每帧粒子数，并以 A-vs-A 自比较来决定可判定性）；以及**资产级
  事实比较 `asset-diff`**（一次从不触碰导出器的反射遍历，包括
  `compiled` 事实族，且两侧都先强制编译）。55 个用例的语料库还会
  比较“由夹具构建的资产”与“由其导出重建的资产” —— 对称丢失自 2026-08-12 起就
  已被看见。
- 四步 CI：lint → build → verify → corpus；`gen-diagnostics.ps1 -Check` 防范文档
  漂移；**143 个诊断代码，每个都带有文件、行和列**。

<a id="two-engines"></a>

### 两种引擎

- MoonEngine（源 5.8.1）与已安装的原版 5.8.1，来自**同一源码树且零 `#if` 分支**。
- 1.0.0 这一轮在原版引擎上修复了四个长进程缺陷：UObject 计数 GC 门、收集与实时编辑面之间的顺序
  安全性、引擎在用户参数没有默认值时将其丢弃的兜底，以及迭代期间修改的致命错误。外加宿主项目内容
  插件对齐要求（参见[限制](../diagnostics/limitations.md)）。

<a id="verification-snapshot-2026-08-13-commit-4e23c55"></a>

### 验证快照（2026-08-13，commit `4e23c55`）

| 门 | Moon（源 5.8.1） | 原版（已安装 5.8.1） |
| --- | --- | --- |
| ci（lint→build→verify→corpus） | OK，55 已验 / 语料库 55 | 语料库 55/55（单独运行） |
| decompile-all | 45/45 | 24/24 |
| build -All -Force | 55/0/0 | 30/0/0（连续两次） |
| mirror-diff | L1 45/45，L2 45/45 | L1 24/24，L2 24/24 |
| asset-diff | 6 相同 / 39 不同（全部为已声明的族） | — |
| L3 运行时等价性 | 45 对：7 精确 / 0 分阶段 / **0 不同** / 38 不可判定 | — |
| 诊断文档 `-Check` | 通过 | — |

<a id="known-issues"></a>

### 已知问题

参见[限制](../diagnostics/limitations.md)。
