# 双向写回：DreamGUI / DUI 的做法，以及 DreamFX 可以照搬的部分

> 2026-10-08 记录。起因：虚梦指出参考 DreamGUI（LGUI fork）的 `.dui` 双向同步 ——
> **以文本为主，Niagara/编辑器只是文本的一个前端，保存时自动写回**。
> 本文是读 DreamGUI 源码后的第一手结论 + 到 DreamFX 的映射，供作者决策。

## 1. DreamGUI 怎么做（源码：`Plugins/DreamGUI/Source/DreamGUIEditor/Private/Text/`）

核心类是 `FDreamUITextWriteBack`（`DreamUITextWriteBack.cpp`），围绕"文档 ↔ 活对象树"做增量同步：

| API | 作用 |
|---|---|
| `Create(文件, host, 错误)` | 打开文档（`FDreamUIDocumentHandle::Open`），登记双向监听 |
| `ProduceText(现有文本, 活对象树, 输出文本, 诊断, 输出 Edits)` | **核心**：拿"当前文本 + 活对象"算出**新文本** |
| `Flush()` / `FlushTree(活对象树, 错误)` | 把算出来的文本**一次写盘**，返回成功/失败与写入计数 |
| `NoteDirtyProperty(树, 节点Id, 目标, 组件下标, 属性名)` / `ClearDirtyProperties` / `NumDirtyProperties` | **脏集合**：只记录被用户动过的属性 |
| `CollectEdits` / `CollectStructuralEdits` / `CollectResourceEdits` | 把改动分类成"属性编辑 / 结构增删 / 资源引用"三类 |
| `RequestRebuild(原因, 是否延迟)` / `ProcessDeferredRebuild()` / `IsRebuildPending()` | 文本变了之后的重建请求，**可延迟合并** |
| `GetWriteCount()` / `GetLastEditCount()` | 写盘次数 / 上次写了多少条（测试就靠它断言批处理） |

### 七条设计要点（都能在测试里找到对应断言）

1. **按行改，不整篇重建** —— `ProduceText` 的输入是"现有文本"，输出只改动到的那几行；
   测试 `DreamGUI.Text.ViewModel.WriteBack.AnEditBesideTheViewModelSyntaxRewritesItsOwnLineAndNothingElse`
   就是在断这件事。
2. **脏集合驱动** —— 不写"所有与对象不同的地方"，只写"这次被动过的地方"
   （`AFlushWritesWhatWasTouchedWhenAnythingSaidSo`）。
3. **写回不触发重建** —— 文本变更是通过 `RequestRebuild(deferred)` 排队的，撤销还会**延迟**重建
   （`DreamUIWriteBackUndoDefersItsRebuildTest`：撤销后 `IsRebuildPending()` 为真，`ProcessDeferredRebuild()` 才兑现）。
4. **一批编辑 = 一次写盘** —— `DreamUIWriteBackOneFlushIsOneUndoStepTest` 断言
   "three properties were written **in a single write**"；同时"一次 flush = 一个 undo 步骤"（`FScopedTransaction`）。
5. **无操作写回必须逐字节不变** —— `...ComesBackFromANoOpWriteBackByteForByte`（两个测试，语法层与视图模型层各一）；
   另有 `NormalisationIsNotAChange`：只有格式化差异时**一个字都不写**（`GetWriteCount() == 0`）。
6. **写不进去就报错，不自己发明一行** —— `DUI7004`（`Docs/DuiLanguage.md` §"What the designer writes back"）：
   没有可写行的东西（比如某类 `@slot` 组合）明说不行，而不是编一个位置塞进去。
7. **文本→编辑器方向有 watcher** —— `DreamUISourceWatcher.cpp`；蓝图编辑器侧通过
   `GetTextWriteBackFilePath()` / `SyncTextWriteBackToSource()` 挂钩。

## 2. 映射到 DreamFX

| DreamGUI 的做法 | DreamFX 现状 | 要做什么 |
|---|---|---|
| `ProduceText(旧文本, 活对象)` 行级改写 | `decompile` 是**整篇重新生成** | 写回要改成行级编辑，否则手工改的东西落不回文本 |
| **脏集合**只写被改的属性 | 无 | build 里加"只写被改字段"的路径 |
| 写回**不触发重建**，重建延迟合并 | watcher 一保存就 build | 写回期间挂起 watcher；配合引擎的编译抑制窗口 |
| 一批 = 一次写盘、一次重建、一次编译 | 一次 build 一次编译（打上引擎补丁后） | 引擎侧已有 `SetSuppressCompileRequests`（Moon-Engine）；补丁已随插件分发 |
| 无操作写回逐字节不变 | 有 `mirror-diff` / `Corpus.RoundTrip` | 已实测：`/AtlasFX` 两个模板 **L1/L2 全 PASS** |
| 写不进去就报错（DUI7004） | 只有 **Adopt** 有这道闸（`DFX8010`），而它依赖的是 `UnsupportedFeatures`，即读侧**自己承认**表达不了的项 | **build 上还没有** ⇒ 资产里存在、文本不会生成的东西会被静默冲掉。实测的当前例子（[write-back-coverage.md](write-back-coverage.md) §3）：① `NS_ScanScene` 整篇 build 不进去（21 × DFX4007，而 `coverage` 对同一个资产报 `ok`）；② 模块输入的 rapid-iteration 字面量根本不进文本（`破空灰尘` 37+4 条、`TR刀光_System` 61+9 条）；③ `MeshYaw` 原资产 −90 / 镜像 0 这种「L1/L2 全绿但值不同」的分歧。三条都是"资产里有、文本里没有"，`DFX8010` 一条都拦不到 |
| 一次 flush 一个 undo | 无 | 一次 build 一个事务 |

> **2026-10-08 订正（实测）**：上表「写不进去就报错（DUI7004）」那一行里"材质参数绑定被冲掉"这个**举例已经不成立** —— `asset-diff` 显示
> `Sheet ← User.Atlas.ResolvedTexture` 现在两侧**逐字符相同**（`MaterialParameters` 一致、`MaterialParamValidMask = 4095`），
> 也就是说在打过引擎补丁的 5.8.2 上这条绑定**能导出、也能写回**。白块事故是在引擎侧能力就位**之前**观察到的；
> "当初被冲掉是否因为缺少引擎侧能力"属**推测，未证明**。
>
> 但安全闸的结论**不变**：build 侧仍然没有闸，而 `DFX8010` 依赖的 `UnsupportedFeatures` **覆盖不到**上面实测的三类
> （那三个资产一条都没报）。证据与建议见 [write-back-coverage.md](write-back-coverage.md) §3、§6.1。

## 3. 建议的实现顺序（给作者）

1. **先补 build 侧的安全闸**（投入最小、收益最大）：build 打开目标资产时顺带扫"资产里有、文本不会生成"的项，
   发现就**列出清单并拒绝覆盖**，要盖得显式 `-Force`。等于把 Adopt 已有的那道闸搬到 build 上。
2. **`pull`（资产 → 文本）**：只改"已有调用里已有输入的字面量"，不增删结构。比 `decompile` 精确得多
   （不需要把内联表达式重建成 hlsl 块），因为映射是确定性的：`(emitter, stack, 模块序号, 输入名)`。
3. **行级写回 + 脏集合**（长期目标，等于 DUI 的 `ProduceText`）。
4. **与编译抑制配合**：`Moon-Engine` 已有 `UNiagaraSystem::SetSuppressCompileRequests` /
   `DeferRequestCompile`，状态更新用 `if (RequestCompileStatus == None)` 保护、不降级更强的状态；
   债由 `WaitForCompilationComplete` / `PollForCompilationComplete` 兑现。

## 4. 验收标准（两条硬的）

- **无操作写回逐字节不变**：`decompile → build → decompile` 两次结果一致（DFX 的 `mirror-diff` 就是干这个的）。
- **一批编辑只编译一次**：`LogDreamFX Verbose` 数 `PHASE RequestCompile issued` 的次数 —— 打引擎补丁前
  最坏系统 260 次 / 8.3 秒，补丁后应为 1 次。

## 5. 参考位置

- DreamGUI 源码：`Plugins/DreamGUI/Source/DreamGUIEditor/Private/Text/DreamUITextWriteBack.cpp`
  （单测在 `Source/DreamGUITests/Private/Editor/DreamUI*WriteBack*Tests.cpp`）
- 语言文档：`Plugins/DreamGUI/Docs/DuiLanguage.md`（§"What the designer writes back"，以及 DUI7004）
- DreamFX 现有同类机制：`dfx.ps1 mirror-diff`、`Corpus.RoundTrip`、`DFX8010`（Adopt 拒绝）
- DreamFX 实测缺口（同日、59 个系统）：[write-back-coverage.md](write-back-coverage.md) ——
  可写回 / 不可写回清单、固定点抽样结果，以及 build 侧安全闸该拦什么
