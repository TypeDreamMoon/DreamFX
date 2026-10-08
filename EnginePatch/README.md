# Niagara 快速编辑补丁（Niagara fast-edit patch）

把 Moon-Engine 分支 `dev/5.8-moon` 里已经实现好的 **Niagara 批量编辑能力**抽出来，做成一份可以直接打在 **原版 UE 5.8.2 引擎**上的补丁。

打上之后，DreamFX 的构建期能力探测会把 `DREAMFX_HAS_NIAGARA_FAST_EDIT` 置为 **1**，`DREAMFX_SOURCE` → Niagara 的写入走"批量写、一次编译"的快速路径。

---

## 1. 这个补丁解决什么问题

DreamFX 把 `.dfm` 文本写成 Niagara 系统时，是**一次写很多个属性**。原版引擎的问题是：**每写一次就重新发起一次编译**。

`UNiagaraSystem::RequestCompile()` 里有这么一段逻辑（引擎原文："we don't want to stack compilations"）：如果已经有一次编译在排队或进行中，它会**先中止掉**，再重新发起。于是一批 N 次写入 = N 次编译启动，**前 N-1 次全部做到一半被丢掉，成果为零**。

作者实测（最坏的那个系统）：

> **260 次编译启动，8.3 秒 —— 全部白跑。**

补丁加上三件事：

| 能力 | 作用 |
|---|---|
| `UNiagaraSystem::SetSuppressCompileRequests(bool)` | 开一个"窗口"，窗口内不发起编译 |
| `UNiagaraSystem::DeferRequestCompile()` | 静默地记下"欠一次编译"这笔账，不启动 |
| `UNiagaraExternalEditUtilities::AddModule(..., bDeferStackRefresh)` | 连续加模块时，把每次都要做的 stack 刷新推迟到最后做一次 |
| `UNiagaraExternalEditUtilities::ClearScriptStack` / `RefreshScriptStack` | 一次清空整个 stack（而不是逐个删模块）、批量结束后补一次刷新 |
| `EModuleTopologyDetail::HeaderOnly` | 只要模块名，不要那 18 个 input 的拓扑（实测省掉单次调用 62 ms 中的 18 ms） |
| `GetEmitterParameterDefaults` / `SetEmitterParameterDefault` / `CleanUpStaleEmitterParameters` | emitter 参数默认值的读写与清理 |

作者那边的口径：全部 55 个系统的重建从约 **17 分钟降到约 9.2 分钟**；单个资产重建本来就只有几秒，差别不明显。

---

## 2. 适用版本

- **原版 UE 5.8.2**（本补丁的基线就是这个版本，逐行核对过）。
- 补丁从 **Moon-Engine** 仓库抽出：
  - 仓库：`https://github.com/TypeDreamMoon/Moon-Engine`
  - 分支：`dev/5.8-moon`
  - 提交：`74b7651a6fb5d59450eb40dc3dbb8b6fe239d097`
  - 子树：`Engine/Plugins/FX/Niagara`
- **其它引擎版本未经验证。** 补丁是以 diff 形式给的，上下文行对不上就打不上（`git apply` 会直接拒绝，不会打歪）。如果目标引擎的 Niagara 已经和 5.8.2 有差异，需要手工重放，见第 6 节。
- 前置条件：这份补丁**只覆盖 Niagara 子树**，但注意 5.8.2 原版就已经带了外部编辑头（`NiagaraExternalSystemEditorUtilities.h`），本补丁是在它之上做增补，**不是**从零引入这套 API。

---

## 3. 怎么打

补丁里的路径是**引擎根相对**形式（`a/Engine/Plugins/FX/Niagara/...`），所以在**引擎根目录**执行：

### 3.1 git 引擎（推荐）

```bash
cd /path/to/UnrealEngine-5.8.2          # 引擎根目录，能看到 Engine/ 的那一层
git apply --check  EnginePatch/Niagara-fast-edit.patch   # 先干跑，确认能打
git apply          EnginePatch/Niagara-fast-edit.patch
```

> **注意：`git apply` 必须在 git 仓库里跑。** 不在仓库里时它**不会报错**——只会打印
> `Skipped patch '...'`，然后 **退出码仍然是 0，什么都不改**。看到 `Skipped patch`
> 就是没打上，别被退出码骗了。非 git 引擎请走 3.2。

### 3.2 非 git 引擎（Perforce / 源码 zip 解压）

用 GNU `patch`：

```bash
cd /path/to/UnrealEngine-5.8.2          # 引擎根目录
patch -p1 --dry-run < EnginePatch/Niagara-fast-edit.patch   # 先干跑
patch -p1           < EnginePatch/Niagara-fast-edit.patch
```

`-p1` 表示剥掉一层 `a/`。本补丁的 `---` / `+++` 是标准的 `a/` `b/` 前缀形式，`-p1` 正确。

> 本机（Windows）没有装 `patch.exe`，上面的命令**未在本机实测**；`git apply` 路径已实测通过（见第 4 节）。

### 3.3 换行符说明（重要）

引擎源码是 **LF** 换行；补丁本身也是 **UTF-8 无 BOM + LF**。

- GitHub 的引擎仓库在 Windows 上常被 `core.autocrlf=true` 检出成 **CRLF**，这会和补丁的 LF 上下文行对不上。
- 如果 `git apply` 报上下文不匹配，先试：

  ```bash
  git -c core.autocrlf=false apply --check EnginePatch/Niagara-fast-edit.patch
  ```

- 或者用 `patch`（它对换行不那么敏感）：

  ```bash
  patch -p1 --binary < EnginePatch/Niagara-fast-edit.patch
  ```

---

## 4. 怎么验证生效

分三步，从"补丁在不在"到"运行时真的只编译一次"。

### 4.1 补丁应用成功（静态）

```bash
git apply --check EnginePatch/Niagara-fast-edit.patch   # 退出码 0，且没有 Skipped patch
```

本补丁在本机对原版 5.8.2 实测的结果是 **13 个文件全部 "Applied patch ... cleanly"**，
且打完之后 13 个文件的内容与 Moon-Engine 侧**逐字节一致**（SHA-256 核对过）。

### 4.2 能力探测生效：`DREAMFX_HAS_NIAGARA_FAST_EDIT` 应为 `1`

DreamFX 的探测在 `Plugins/DreamFX/Source/DreamFXEditor/DreamFXEditor.Build.cs` 里，
是**读引擎头文件里的正则**，不是链接测试。打上补丁后应当变成：

```
DREAMFX_HAS_CUSTOMHLSL_WRITE     = 1
DREAMFX_HAS_NIAGARA_FAST_EDIT    = 1
DREAMFX_HAS_NIAGARA_EXTERNAL_EDIT = 1
```

查找方法 —— 看 UBT 生成的 Definitions 文件：

```
Plugins/DreamFX/Intermediate/Build/Win64/x64/UnrealEditor/Development/DreamFXEditor/Definitions.DreamFXEditor.h
```

在里面找 `DREAMFX_HAS_NIAGARA_FAST_EDIT`。

**打补丁前**（本机当前状态，已核实）：

```
#define DREAMFX_HAS_CUSTOMHLSL_WRITE 0
#define DREAMFX_HAS_NIAGARA_FAST_EDIT 0
#define DREAMFX_HAS_NIAGARA_EXTERNAL_EDIT 1
```

> **这个文件只在 DreamFXEditor 模块被重新编译时才会刷新。**
> 如果你只打了引擎补丁、没重建，它还会是旧的 `0`。UBT 认的是 `Build.cs` 的时间戳，
> 所以最省事的办法是 **touch 一下 `DreamFXEditor.Build.cs`**（或者改一个字符再改回来），
> 然后重建 DreamFXEditor 模块。**必须先编译引擎**，否则探测读到的是源码头文件（会变成 1），
> 但链接的还是旧的 Niagara DLL（没有这些符号），会在链接期炸掉。

本机对"打了补丁之后"的树跑过 DreamFX 的全部探测正则，**12 项全 PASS**：

```
RequiredExports (5 项)   原版=0/5  打补丁后=5/5
FastEditExports (7 项)   原版=0/7  打补丁后=7/7
```

也就是说这个补丁**同时**把 `.dfm` 生成（`DREAMFX_HAS_CUSTOMHLSL_WRITE`）也打开了 ——
它不仅提供 `SetSuppressCompileRequests`，还补齐了 `SetCustomHlsl` /
`InitAsCustomHlslDynamicInput` / `RequestNewTypedPin` / `AddParameter` 四个
`NIAGARAEDITOR_API` 导出。

### 4.3 运行时：数 `PHASE RequestCompile issued` 的次数

打开 Verbose 日志：

```
-LogCmds="LogDreamFX Verbose"
```

然后跑一次会写很多属性的系统构建，在日志里数这一行出现的次数：

```
LogDreamFX: Verbose: PHASE RequestCompile issued '<SystemName>'
```

| | 次数 |
|---|---|
| 打补丁前（慢路径） | **260** |
| 打补丁后（快速路径） | 应为 **1** |

> 这一行由 `DreamFXNiagaraAdapter.cpp` 的 `RequestCompileAsync()` 打印，每次
> `System->RequestCompile()` 调用打一条。所以数它就是在数"真正发起了几次编译"。
> 快速路径下整个批量只有收尾那一次，即 1。

---

## 5. 怎么回退

```bash
cd /path/to/UnrealEngine-5.8.2
git apply -R EnginePatch/Niagara-fast-edit.patch      # git 引擎
patch -p1 -R < EnginePatch/Niagara-fast-edit.patch    # 非 git 引擎
```

`git apply -R` 已在本机实测通过（退出码 0）。

回退后请**重新编译引擎**，并 **touch `DreamFXEditor.Build.cs`** 再重建 DreamFXEditor，
否则 `DREAMFX_HAS_NIAGARA_FAST_EDIT` 会停在 `1`，而 Niagara DLL 里已经没有那些符号 —— 链接期报错。

---

## 6. 风险与限制

1. **引擎升级需要重放补丁。** 这是打引擎源码的补丁，不是插件。UE 每升一个小版本，
   Niagara 的上下文行都可能漂移，`git apply` 会直接拒绝（这是好事，失败比打歪安全）。
   重放的锚点见第 7 节。
2. **不打补丁也不会坏。** DreamFX 的能力探测是兜底：没有补丁时
   `DREAMFX_HAS_NIAGARA_FAST_EDIT=0`，代码走 `#else` 分支的慢路径，
   **功能完全正确，只是慢**。这正是这个设计的意义 —— 引擎补丁永远不会变成硬依赖。
3. **必须先编引擎，再编插件。** 探测是读**源码头文件**的，而符号是**链接到 Niagara DLL** 的。
   只打补丁不编引擎 = 探测说"有"、链接说"没有" = 链接期报错。
4. **`SetSuppressCompileRequests(true)` 之后不能再等编译完成。** 抑制期间
   `RequestCompile` 只记账不启动，此时调 `WaitForCompilationComplete()`
   会等一个永远不会来的编译。DreamFX 用的是 RAII scope（`FCompileSuppressionScope`），
   等待放在 scope 外面，所以安全；但**自己写调用方时要遵守这个约定**。
5. **抑制期间 `bForce=true` 也会被吞掉。** 补丁里的检查在函数最前面，
   `RequestCompile(bForce=true)` 在窗口内同样只记账、返回 `false`。
   作者的注释说窗口结束时"任何显式 RequestCompile 都会结账"，但如果那次
   `RequestCompile` 仍在窗口内，它不会真正启动 —— 结账靠的是**窗口关闭之后**的调用，
   或者 `WaitForCompilationComplete` / `PollForCompilationComplete`（它们会先看
   `NeedsRequestCompile()` 再补发）。**不要把 `bForce` 当成穿透开关。**
6. **`NIAGARAEDITOR_API` 变化会改变被导出符号。** 补丁给
   `UNiagaraNodeCustomHlsl::SetCustomHlsl` / `InitAsCustomHlslDynamicInput`、
   `UNiagaraNodeWithDynamicPins::RequestNewTypedPin`、`UNiagaraGraph::AddParameter`
   加上了 `NIAGARAEDITOR_API`（原本没有导出）。引擎内其它模块引用了这几个符号的话
   需要一并重编。**实测引擎里唯一的外部调用方是 `NiagaraToolsets` 插件，它不引用这 4 个符号**
   （它对 `UNiagaraExternalEditUtilities` 的 50 多处调用也不涉及本补丁改动的签名）。
7. **公开函数签名有增补（带默认值，源码兼容）。**
   `GetModuleTopology` / `AddModule` / `AddSetParametersModule` 末尾各加了参数，
   默认值是 `EModuleTopologyDetail::WithInputs` 和 `bDeferStackRefresh = false`，
   **与旧行为逐字等价**，所以既有的 `NiagaraToolsets` 调用点不用改。
   只有取函数指针/`decltype` 硬绑签名的地方会受影响。
8. **补丁带 4 处行尾空白。** `git apply` 会因此打印
   `warning: 4 lines add whitespace errors`（仅 `NiagaraExternalSystemEditorUtilities.cpp` 内），
   **不影响应用**。若你的 git 配了 `apply.whitespace=error`，加 `--whitespace=nowarn`。
9. **补丁全自足，不依赖 Niagara 之外的文件。** 见第 7 节的自足性结论。

---

## 7. 自足性结论

**结论：这份补丁是自足的 —— 只打 Niagara 子树就能用，不需要任何 Niagara 之外的文件。**

核查依据：

| 核查项 | 结果 |
|---|---|
| 补丁涉及文件数 | **13 个，全部在 `Engine/Plugins/FX/Niagara/Source/` 之下**，无新增文件 |
| 新增 `#include` | 只有 2 个，且都在 Niagara 插件内：`NiagaraDataInterfaceCurveBase.h`（Niagara 模块 Public）、`NiagaraExternalSystemEditorUtilities.h`（NiagaraEditor 模块 Public） |
| 自定义宏（`MOONENGINE_*` / `DREAM*`） | **没有**。补丁里 5 个 `#define`（`MOON_ADD_STEP` / `MOON_SET_STEP` / `MOON_VM_STEP` / `MOON_REFRESH_STEP` / `MOON_LOCAL_STEP`）都在**同一个 .cpp 内定义并使用**，不跨文件。 |
| 新增的类/枚举/结构体 | `EModuleTopologyDetail`、`FNiagaraExternalEditStepStats`、`FMoonScopedStep`、`FNiagaraExt_ParameterDefault` —— **全部在补丁自己的头文件里定义** |
| `UE_API` 从哪来 | `NiagaraExternalSystemEditorUtilities.h` 第 21 行自己 `#define UE_API NIAGARAEDITOR_API`（原版就有，非补丁引入），不依赖 UBT |
| 对引擎其它部分的依赖 | 没有新增模块依赖；`NiagaraEditor` 本来就依赖 `Niagara`、`NiagaraCore` |
| 编译期缺失符号 | **静态排查未发现** |

> 说明：本机**没有编译引擎**（按约定不编译）。上面是**静态排查**的结论 ——
> 逐个核对了补丁新增的每个类型、宏、`#include` 的定义来源。
> 没有做真实编译验证，这一点请知悉。

---

## 8. 关键 API 一览（便于 review）

### 8.1 `Source/Niagara/Classes/NiagaraSystem.h`（+34 行）

```cpp
// 新增公开方法
void DeferRequestCompile();                        // 记账：欠一次编译，不启动
void SetSuppressCompileRequests(bool bSuppress);   // 开/关抑制窗口
bool GetSuppressCompileRequests() const;

// 新增 protected 成员（紧挨 RequestCompileStatus）
bool bSuppressCompileRequests = false;             // 注意：不是 UPROPERTY，不参与序列化
```

### 8.2 `Source/Niagara/Private/NiagaraSystem.cpp`（+13 行）

`UNiagaraSystem::RequestCompile()` 开头插入抑制判断：窗口内把
`RequestCompileStatus` 置为 `RequestPending`（仅当当前是 `None`，**不降级**已有的更强状态），
打一条 `LogNiagara` Verbose 日志，然后 `return false`。

### 8.3 `Source/NiagaraEditor/Public/NiagaraExternalSystemEditorUtilities.h`（+126 / -3）

```cpp
enum class EModuleTopologyDetail : uint8
{
    HeaderOnly,   // 只要 name / enabled / script / set-parameters 标志
    WithInputs,   // 额外把每个 input 铺进 OutTopology.Inputs  ← 默认值，等同旧行为
};
```

```cpp
UE_API static void GetModuleTopology(..., EModuleTopologyDetail Detail = EModuleTopologyDetail::WithInputs);
UE_API static void AddModule(..., EModuleTopologyDetail Detail = EModuleTopologyDetail::WithInputs,
                                  bool bDeferStackRefresh = false);
UE_API static void AddSetParametersModule(..., EModuleTopologyDetail Detail = EModuleTopologyDetail::WithInputs,
                                              bool bDeferStackRefresh = false);

UE_API static void ClearScriptStack(...);          // 一次清空整个 stack
UE_API static void RefreshScriptStack(...);        // 批量结束后补一次刷新
UE_API static void GetEmitterParameterDefaults(...);
UE_API static void SetEmitterParameterDefault(...);
UE_API static void CleanUpStaleEmitterParameters(...);
```

### 8.4 `Source/NiagaraEditor/Private/NiagaraExternalSystemEditorUtilities.cpp`（+464 / -12）

上面这些静态函数的实现，外加一套 `FNiagaraExternalEditStepStats` 计时统计
（`MOON_ADD_STEP` / `MOON_SET_STEP` 埋点，`FNiagaraExternalEditStepStats::Add`）。
另新增 `#include "NiagaraDataInterfaceCurveBase.h"`。

### 8.5 其余文件

| 文件 | 改动 | 内容 |
|---|---|---|
| `NiagaraEditor/Public/NiagaraNodeCustomHlsl.h` | +5 / -2 | `SetCustomHlsl`、`InitAsCustomHlslDynamicInput` 加 `NIAGARAEDITOR_API` |
| `NiagaraEditor/Public/NiagaraNodeWithDynamicPins.h` | +3 / -1 | `RequestNewTypedPin` 加 `NIAGARAEDITOR_API` |
| `NiagaraEditor/Public/NiagaraGraph.h` | +3 / -1 | `AddParameter` 加 `NIAGARAEDITOR_API` |
| `NiagaraEditor/Private/NiagaraNodeParameterMapGet.h` | +1 / -1 | UCLASS 加 `MinimalAPI` |
| `NiagaraEditor/Private/NiagaraNodeParameterMapSet.h` | +1 / -1 | UCLASS 加 `MinimalAPI` |
| `NiagaraEditor/Public/ViewModels/NiagaraEmitterHandleViewModel.h` | +16 / -0 | 延迟 stack 初始化的成员（`bStackInitializationDeferred`、`DeferredStackSystemViewModel`） |
| `NiagaraEditor/Private/ViewModels/NiagaraEmitterHandleViewModel.cpp` | +35 / -1 | 上述延迟初始化的实现 |
| `NiagaraEditor/Private/ViewModels/NiagaraSystemViewModel.cpp` | +70 / -4 | `MOON_VM_STEP` / `MOON_REFRESH_STEP` 埋点，构造期延迟解析 |
| `NiagaraEditor/Private/ViewModels/Stack/NiagaraStackFunctionInput.cpp` | +42 / -2 | `MOON_LOCAL_STEP` 埋点，`NiagaraSystem.DeferRequestCompile()` 调用 |

---

## 9. 补丁本身的信息

| 项 | 值 |
|---|---|
| 路径 | `Plugins/DreamFX/EnginePatch/Niagara-fast-edit.patch` |
| 大小 | 65,485 字节（1,322 行） |
| 编码 | UTF-8 **无 BOM** |
| 换行 | **LF**（无 CR） |
| 涉及文件 | **13** 个 |
| 增 | **+813** 行 |
| 删 | **-28** 行 |
| 基线 | 原版 UE 5.8.2 的 `Engine/Plugins/FX/Niagara` |
| 目标 | Moon-Engine `dev/5.8-moon` @ `74b7651a` 的同一子树 |

生成方式：以恢复成原版的引擎树为 "a" 侧、以 Moon-Engine 检出为 "b" 侧，
把 b 侧工作区的 CRLF **按字节归一成 LF**（Moon-Engine 仓库 `core.autocrlf=true`，
但 git blob 本体是 LF，归一后与 blob 一致）再做 `git diff --no-index`，
最后把三行路径头规整成 `a/Engine/...` / `b/Engine/...`。

> 为什么必须归一：Moon-Engine 的工作区是 CRLF、我们的引擎是 LF，
> 直接把两个目录做 diff 会得到 **1773 个文件"全改"**的假象
> （实际只有 13 个文件真变了，其余 1760 个只差换行符）。
