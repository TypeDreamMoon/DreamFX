# `pull`：把资产里的现值写回文本

> 2026-10-09 记录。分支 `feat/pull`。工具：`dfx pull`（命令实现在
> `Source/DreamFXEditor/Private/WriteBack/DreamFXPull.cpp`）。
>
> 分工声明：`write-back-coverage.md` 记的是**实测缺口**、`write-back-from-dreamgui.md` 记的是
> **DreamGUI/DUI 的做法与可借鉴之处**；本篇只写 `pull` 这一个命令的语义、边界与实测，不改前两篇。

---

## 0. 一句话

**`pull` 把资产 rapid-iteration store 里的值，写回文本里已经写着的那个字面量。**

工作流是「结构写文本、数值在编辑器里调」。问题出在最后一步：调好的值活在资产里，
文本里那个输入要么根本没写（重建时按模块默认值回来，调好的值没了），要么写的是**读路径能看见的值**
而不是**store 里存的值**（`NS_Effects1_Mesh` 的 `Sprite_Atlas_Size.MeshYaw`：store 里 `-90`，
每一次读 pin 都是 `0.0`）。`pull` 把 store 里的值搬进文本，于是「调好的值」变成文本的一部分。

---

## 1. 用法

```powershell
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# dry run：只打印打算改哪一行，一个字节都不写（默认）
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Engine $eng

# 写：把值写进文本（写前备份）
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Apply -Engine $eng

# 从「另一个资产」读值（导出文本指向的是镜像，值在原资产里）
pwsh -File $dfx pull DFX/Decompiled/Game/FX/NS_Spark.dfs -Asset /Game/FX/NS_Spark -Apply -Engine $eng
```

| 参数 | 含义 |
|---|---|
| `pull <file.dfs>` | 要写回的那份文本。只能是一个 `.dfs`（系统文档） |
| `-Apply` | 真正写文件。**不带就是 dry run** |
| `-Asset=<包路径>` | 从**这个**资产读值，而不是文本 `Name=` 指的那个 |

`-Asset` 不是可选的便利：**导出文本在结构上不可能指向它被导出的那个资产**
（plan-v4 V1 把所有导出都写进 `Decompiled/` 命名空间，正是为了让重建碰不到原件）。
所以「把原资产里调好的值拉进这份导出」只有这一种写法 —— 文本的 `Name=` 仍然决定 `build` 写谁，
`-Asset` 只决定 `pull` 读谁。

### 产物落在哪里

| 内容 | 路径 |
|---|---|
| 写出前的原文（每个文件一份） | `Saved/DreamFX/Pull/<文本相对工程根的路径>`（例：`Saved/DreamFX/Pull/DFX/Decompiled/Game/FX/NS_Spark.dfs`） |
| 本次报告（dry run 也写） | 同上，扩展名换成 `.report.txt` |

备份和报告都落在 `Saved/` 下，**不污染 `DFX/`**。「资产里有、文本没声明」的清单是**逐条**打的
（真实资产动辄几十条，`破空灰尘` 是 94 条），不截断：这些名字只在这里出现，
而每一条都对应「重建时这个输入要靠模块默认值兜底」的风险，截断掉就没人看得到。

---

## 2. `-Apply` 的语义

- **不带 `-Apply`**：读资产、解析文本、算出每处要写的字面量，打印
  `文件(行,列): info DFX7105: pull: <模块>.<输入>: <旧字面量> -> <新值>`，然后停手。
  文本一个字节都不动（报告文件除外）。
- **带 `-Apply`**：先备份，再按**字面量自己的字节区间**做替换，然后**必须**能重新解析通过才落盘；
  解析不过就报 `DFX7110` 并放弃（文件保持原样）。
- **幂等**：第二次跑 `-Apply` 不写任何字节（见 §5 案例 1d）。判定是**按值**比的，不是按字符串：
  文本里的字面量会用 `build` 用的那个 `FValueLowering::Lower` 降下来，再和 store 的字节逐字节比。
  所以 `0` 与 `0.0` 是同一个值，不会被「改成另一种写法」。
- **pull 永远不写资产**。系统只是被读（`LoadObject` + 只读 API），没有任何 `Save` 路径。

---

## 3. 与 `decompile`、build 安全闸的分工

三个功能各自只有一半，拼起来才是双向写回：

| | 方向 | 读什么 | 干什么 |
|---|---|---|---|
| `decompile` | 资产 → 文本 | 活输入的 **pin** | 写出结构；`-NoDefaults` 把**每个输入的名字**都写进文本（值取 pin） |
| **`pull`** | 资产 → 文本 | rapid-iteration **store** | 只改文本里**已经写着**的字面量的值 |
| `build` | 文本 → 资产 | 文本 | 写资产；**安全闸 DFX8017** 拦住「重建会丢掉文本表达不了的事实」的保存 |

关系是三句话：

1. **`decompile` 给键，`pull` 给值。** 文本里没有这个输入，`pull` 不会替你加上 ——
   那不是「写回」，那是改结构。要先把键弄进文本，最省事的办法就是 `decompile -NoDefaults`
   （`write-back-coverage.md` §6.4 把它定义成「写回路径的读取模式」，本命令依赖这一点）。
2. **`pull` 和安全闸是同一枚硬币。** 安全闸拦的是「结构两侧都在、某条事实的值变了、而文本没声明这个输入」；
   `pull` 做的正是**让文本声明它**。所以 `NS_Effects1_Mesh` 那条：文本指向原资产 → 安全闸拦下 2 条
   `MeshYaw: 0000B4C2 -> 00000000`（§6.8 第 6 行）；`pull` 把 `-90.0` 写进文本；再 `build` 就是干净通过 ——
   值变了但**文本写了**，规则 3 放行。
3. **文本没声明、store 里有的那些，`pull` 只报告不动手**（`DFX7106`）。
   里面哪些是「重建真的会丢」由安全闸（`DFX8017`）判定：值恰好等于模块默认值的那些，
   重建时会按默认值补回来，事实相同、不会丢（§6.8 的「读数注意」实测：37 条里只有 8 条真丢）。
   `pull` 不去复算模块默认值 —— 那是第二个探测系统、第二个默认值表，会和安全闸的口径漂移。

---

## 4. 能改什么、不能改什么

### 只改：已有模块调用里、已有输入的实参，且实参本身是字面量

四条定位规则，全部是**验证**而不是猜：

1. **节点顺序**：文本里某条语句，对应资产里同一 emitter、同一 stack、**同一序号**的节点。
2. **模块身份**：那条语句的模块资产（`FModuleLibrary::FindModule` 解出来的 `UNiagaraScript`）
   必须和资产那个节点跑的脚本**逐路径相同**。
3. **节点名**：语句带 `as <名字>` 时，资产节点的名字必须就是它。
4. **折叠赋值**：连续赋值语句（L2 会折成一个 Set Parameters 模块）当作**一个**节点，
   它落到的资产节点必须正好是 `bIsSetParameters` 的那个。

任何一条对不上 ⇒ **整个 stack 拒绝对待**（`DFX7109`，明细里把两侧的节点名单都列出来），
而不是「尽力而为」：错位一位的对应关系去改字面量，比不改危险得多。

值这一侧：

- 文本里的实参用 `FValueLowering::Lower` 按**该输入的真实类型**降下来；降不下来 / 降出来不是字面量
  （链接、动态输入、`hlsl {}`、`curve {}`、DI、对象引用）⇒ 报 `DFX7111`，不改。
  **改值模式是编辑，不是写回。**
- store 里的值是数据接口/对象引用 ⇒ 同上，不改。
- 名字歧义：DSL 把空格/连字符/大小写抹平，`Scale RGB`（Vector3）和门控它的内联条件
  `ScaleRGB`（bool）在语言里是**同一个标识符**。哪个才是这条实参的意思，**由值的类型决定**
  （和生成器的做法一致）；如果它其实是**另一个**输入，这里什么也不说 —— 那条 store 值不归它管。
  如果多个候选都接受这个值 ⇒ 报 `DFX7111` 拒绝，不猜。

### 不改

| 不改 | 报什么 |
|---|---|
| 不新增实参、不新增/删除模块、不动任何结构 | `DFX7106`（store 里有、文本没声明的那条） |
| 事件栈 `OnEvent`、模拟阶段 `Stage` | `DFX7111`（v1 不寻址：它们要经 focus slice 读，是另一套机制） |
| 渲染器属性、系统/发射器 `Settings` | 不在范围内 —— 它们不是「调用的实参」。这类事实由安全闸（`DFX8017`）覆盖 |
| 赋值语句的值（Set Parameters 入口、`Defaults = {}` 块） | v1 不寻址 |
| `from "..."` 引进来、但本文件没写的 stack | 只读本文件写得出来的 stack（`Emitter.FromPath` 时记一条 Verbose） |
| 模块输入之外的东西（`di` / `compiled` / `user` 事实族） | 不读、不报 |

### 与文本行尾/缩进/注释的关系

替换只覆盖字面量自己的字节区间（`FValue::StartOffset/EndOffset`，由词法器记录，
见 §6）。值后面的注释、缩进、行尾风格一律原样保留；写回后还要能解析通过。

---

## 5. 实测（2026-10-09，本轮全部原始日志在 `Saved/pull-probe/runs/`）

四个用例都用真实资产，文本一律是 `Saved/pull-probe/` 下的副本 —— 工程 `DFX/` 树里一个字节都没动。
两个被瞄准的**原资产**也没有：`NS_Effects1_Mesh.uasset` 与 `破空灰尘.uasset` 的 SHA256
与安全闸那一轮留的 `Saved/DreamFX/gate-probe/asset-backup/` 逐字节相同，
mtime 分别是 10-08 17:38 与 03-04 22:47（都早于本轮）。

### 案例 1：`NS_Effects1_Mesh` 的 `Sprite_Atlas_Size.MeshYaw`（store 里 `-90`，pin 读出来是 `0.0`）

```powershell
# 文本：-NoDefaults 导出。正常导出里根本没有 MeshYaw 这一行（那是案例 2 的形态）
pwsh -File $dfx decompile /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
# 值：读原资产（这份文本的 Name= 指向镜像，镜像的 store 里是 0）
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh
pwsh -File $dfx pull Saved/pull-probe/NS_Effects1_Mesh.dfs -Asset /Game/GameActor2D/Misaka/Material/Sk1/NS_Effects1_Mesh -Apply
pwsh -File $dfx build Saved/pull-probe/NS_Effects1_Mesh.dfs
pwsh -File $dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
```

（本节数字见下。）

### 案例 1 的实测

```text
$ dfx decompile <原资产> -NoDefaults -Out Saved/pull-probe/NS_Effects1_Mesh.dfs
  106: MeshYaw = 0.0                     ← 这一行来自 pin；store 里存的是 -90
  导出 SHA256 = 3E3A1DC4…

$ dfx pull <文本> -Asset <原资产>          # dry run
  NS_Effects1_Mesh.dfs(106,27): info DFX7105: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  pull: 1 literal(s) would be written and nothing was: this is a dry run.
  === DreamFX pull: 14 value(s) compared, 1 to write, 5 not declared, 1 not writable, 0 stack(s) not addressed | dry run ===
  SHA256 不变

$ dfx pull <文本> -Asset <原资产> -Apply
  pull: wrote 1 literal(s) into '…NS_Effects1_Mesh.dfs'. The file it was is at
        'C:/CrossingVoid/Saved/DreamFX/Pull/Saved/pull-probe/NS_Effects1_Mesh.dfs'.
  NS_Effects1_Mesh.dfs(106,27): info DFX7105: pull: Sprite_Atlas_Size.MeshYaw: 0.0 -> -90.0
  === DreamFX pull: 14 value(s) compared, 1 written, 5 not declared, 1 not writable, 0 stack(s) not addressed | applied ===

  整文件 Compare-Object（备份 vs 写回后）：只有一行
      => MeshYaw = -90.0
      <= MeshYaw = 0.0

$ dfx build <文本>                       # 文本指向镜像 → 重建镜像
  === DreamFX done: 1 built, 0 up to date, 0 failed | 0 error(s), 0 warning(s) ===
  日志里**没有任何 build safety 行**：安全闸无话可说（文本写了这个输入 ⇒ 规则 3 放行）

$ dfx asset-diff -Path /Game/GameActor2D/Misaka/Material/Sk1 -NoCompile -DumpFacts
  DIFF … original 186 fact(s), mirror 186; 1 only-original, 1 only-mirror
    仅剩的一处差异是 system FixedBounds（原 ±100 / 镜像 ±300），MeshYaw 两侧都已是 0000B4C2

$ dfx pull <文本> -Asset <原资产> -Apply    # 第二次
  info DFX7107: … already holds every value … (14 compared, 5 not declared, 1 not writable). No bytes written.
  === DreamFX pull: 14 value(s) compared, 0 to write, … | applied ===
  SHA256 与 mtime 都不变（幂等）
```

四点读法：

- **`pull` 只动了那一行**（这份 -NoDefaults 导出 123 行，写回前后都是 123 行，
  唯一的差异就是 `MeshYaw` 那一行）—— 缩进、其它字面量、注释、行尾全部原样。
- **`build` 干净通过**，而且是真的在重建（`1 built`，不是 `up to date`）：
  这正是「`pull` + 安全闸」配合的证据 —— 值从 `0` 变成 `-90`，但**文本声明了它**。
- **`asset-diff` 里 `MeshYaw` 那一项不再有差异**（两侧都是 `0000B4C2`），
  唯一剩下的 `system FixedBounds` 差异是**本轮之前就存在的**：
  安全闸那一轮的 `Saved/DreamFX/BuildSafety/NS_Effects1_Mesh.{before,after}.facts` 里
  镜像就已经是 ±300，两侧 `bFixedBounds = False`（这个值根本没启用），而文本从来不声明
  `FixedBounds` —— 重建不动它，安全闸也就看不到变化。**不是本轮引入的，也不由 `pull` 负责**
  （如果要去掉，属于「文本该不该声明系统 `FixedBounds`」的问题）。
- **幂等**：第二次 `-Apply` 报 `DFX7107` 且不写任何字节。

顺带一条真实的 `DFX7111`（同一个文件）：
`SpawnBurst_Instantaneous.Age is declared but was not written back: the text writes it as a linked parameter, not as a literal, and changing the value MODE is an edit rather than a write-back`
—— 文本里 `Age = Emitter.Age` 是链接，store 里却有常量，`pull` 报出来但不改。

### 案例 2：`破空灰尘` —— store 里有、文本没声明的那些

原始导出（**不**加 `-NoDefaults`）里，`破空灰尘` 有 8 条事实是「镜像没有、原资产有」的模块输入常量
（§3.4 / §6.8 第 7 行）：4 × `ScaleColor.Scale RGBA`（2 emitter × spawn/update 两个脚本）
+ 4 × `EmitterState.Min/MaxDistance`。

`pull` 的判语（原文，`Saved/pull-probe/runs/c2-pull-apply.log`）：

```
（下面是节选，实际每个「有值但没声明」的输入各一行）
info DFX7106: pull: ScaleColor.ScaleRGBA is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MaxDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
info DFX7106: pull: EmitterState.MinDistance is stored on the asset and not declared in this text -- skipped. pull adds no structure.
...
info DFX7107: ... (19 compared, 94 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 19 value(s) compared, 0 to write, 94 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

- **6 行报告 = 上面那 8 条事实**：同一个名字在两个脚本（`ParticleSpawnScriptInterpolated` /
  `ParticleUpdateScript`）里各一份，而两份**值相同**，`pull` 按名字合并成一行（两份值不同就拒绝，见 §6）。
  另外注意这两个脚本走的是 `ParticleSpawn` / `ParticleUpdate` 两条 stack —— 每行都带自己的 `(行,列)`。
- `ScaleColor.ScaleRGBA` 是 `Scale RGBA` 的写法化名字（`ToInputIdentifier`）。
- **文件 SHA256 前后相同**（dry run 与 `-Apply` 都一样）—— 这就是「不新增结构」的证据；
  dry run 与 `-Apply` 的差别只在最后那行汇总的 `| dry run` / `| applied`。
- 94 条「没声明」里绝大多数是引擎在模块默认值上物化出来的常量（§6.8「读数注意」：
  37 条里只有 8 条是真会丢的）；哪些会丢是安全闸要回答的问题，不是 `pull` 的。

### 案例 3：本来就一致的文本（`/AtlasFX/Templates/NS_Atlas2D_Mesh` 的干净镜像）

文本是镜像自己的导出（`Name=` 指向 `/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh`），
`pull -Apply` 的判语：

```
info DFX7107: pull: '…NS_Atlas2D_Mesh.dfs' already holds every value
  '/AtlasFX/Decompiled/Templates/NS_Atlas2D_Mesh' stores for the input(s) it declares
  (2 compared, 18 not declared, 0 not writable). No bytes written.
=== DreamFX pull: 2 value(s) compared, 0 to write, 18 not declared, 0 not writable, 0 stack(s) not addressed | applied ===
```

SHA256 前后相同（**无改动**）。那 18 条「没声明」全是引擎物化出来的默认值
（`MeshSize` / `MinSize` / `UniformScale` / `MeshYaw = 0` 等 —— 这个模板的 `MeshYaw` 本来就是 0，
见 §2.1），`pull` 不去动它们。

### 案例 4：门禁

```text
$ dfx lint -All
  === DreamFX done: 0 built, 0 up to date, 0 failed | 0 error(s), 89 warning(s) ===
  dfx: OK (exit 0)                      # 89 条 warning 是内容侧既有的 lint 提示，不是本轮引入
$ dfx corpus
  dfx: corpus OK (55 passed)            # 与安全闸那一轮的 55 passed 基线相同
$ Build.bat CrossingVoidEditor Win64 Development -MaxParallelActions=2 -WaitMutex
  Result: Succeeded                     # 只剩 `DreamFXCorpusTest.cpp:572` 那条既有的 C4996
```

原始输出同样在 `Saved/pull-probe/runs/`（`c4-lint.log` / `c4-corpus.log`）。

---

## 6. 已知限制（都写清楚，别当成没问题）

1. **`pull` 读的是 store，不是 pin。** 这正是它存在的理由，但要知道它的含义：
   `MeshYaw` 这类输入，pin 与 store 是两个值（§3.5）。`pull` 站 store 这一边，
   与 `asset-diff` 和安全闸**同一口径**。若某个输入 store 里根本没有条目（例如它一直是链接），
   `pull` 不会凭空造一个值 —— 它只写「资产里确实存着的」。
2. **同一个名字存在多份、值不同 ⇒ 拒绝**（`DFX7111`）。哪一份是引擎真正读的，`pull` 判断不了，
   猜一个就会把资产里并不存在的数字写进文本。
3. **位置对应关系是硬前提。** 文本被手工改过（多一个/少一个模块）之后，那个 stack 会被整段拒绝。
   这是有意的：此时正确动作是 `build` 让文本和资产对齐，再 `pull`。
4. **节点名从资产读，不要求文本写出来。** `as <节点名>` 只在文本必须自己标明身份时才有意义
   （`Output.<节点>.<值>` 链接）；`pull` 用的是资产那边的名字，所以 `Foo_1` 这类引擎编号不需要文本参与。
   代价是：对应关系完全落在「同 stack、同序号、同模块资产」上 —— 而这**正是 `build` 自己的对应关系**
   （语句顺序 = 添加顺序 = 节点顺序），所以 `pull` 在序号 i 写下的值，就是一次重建会在序号 i 应用的值，
   两者不会各说各话。
5. **`-NoDefaults` 的导出是「键的来源」，不是「值的来源」。** 它把每个输入都写出来
   （值的部分是 pin，所以 `MeshYaw` 那一行是 `0.0`），`pull` 负责把这些值改成 store 里的。
   本轮实测这份文本**能干净重建**（`1 built, 0 error, 0 warning`），但那是这一次的结论，不是承诺：
   若某份导出因此带上 `build` 不接受的输入，那是 `decompile -NoDefaults` 与写侧的问题，
   与 `pull` 无关。普通导出 + 手工补一行 `<输入> = <现值>` 同样可行，而且更小。
6. **`-Apply` 之后文本的 `Name=` 决定 `build` 写谁。** 用 `-Asset` 读原资产时，
   `build` 仍然写文本自己指的那个资产（通常是镜像）—— 别把 `-Asset` 当成「写回原资产」。
7. **`GetEmitterInfo` 的 out 参数是追加语义**（不是覆盖）：同一个 `FEmitterInfo` 实例给两个 emitter 用，
   第二个 emitter 会拿到第一个的 stack。`pull` 每个 emitter 新建一个实例；
   这个陷阱值得在适配器里堵掉（本轮没动它，因为它是共享路径）。
8. **只在六条固定 stack 里寻址**；事件栈/模拟阶段栈、渲染器属性、设置块都不在 v1 范围内（§4）。

---

## 7. 复现步骤

```powershell
# 0) 确认没有 UnrealEditor 在跑；每条命令一次编辑器启动
$dfx = 'C:\CrossingVoid\Plugins\DreamFX\.skill\dfx.ps1'
$eng = 'D:\UnrealEngine-5.8.2'

# 本轮全部四个用例（日志落 Saved/pull-probe/runs/）
pwsh -File C:\CrossingVoid\Saved\pull-probe\run-pull-cases.ps1

# 门禁
pwsh -File $dfx lint -All -Engine $eng
pwsh -File $dfx corpus -Engine $eng
& 'D:\UnrealEngine-5.8.2\Engine\Build\BatchFiles\Build.bat' CrossingVoidEditor Win64 Development `
    -Project='C:\CrossingVoid\CrossingVoid.uproject' -MaxParallelActions=2 -WaitMutex
```
