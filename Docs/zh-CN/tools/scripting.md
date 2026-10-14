---
title: "Python 与蓝图接口"
description: "UDreamFXEditorLibrary 把每一条编辑器命令暴露给脚本，以及为什么这不是\"顺便加的\"。"
---

`UDreamFXEditorLibrary` 把每一个编辑器命令暴露给 Python 和蓝图，转发到菜单调用的同一批函数体。

```python
import unreal

lib = unreal.DreamFXEditorLibrary

lib.rebuild_all_sources()                   # = Tools ▸ DreamFX ▸ Rebuild DFX
lib.verify_all_sources()                    # = Verify DFX
print(lib.write_workspace_file())           # writes the workspace, does not launch it

lib.export_system(unreal.load_asset("/Game/FX/NS_Spark"))
lib.adopt_system(unreal.load_asset("/Game/FX/NS_Spark"), True)   # True = skip the confirmation
```

> **提示：**
>
> **一个只有人手能点到的命令，是永远得不到回归测试的命令。** Slate 条目没法从脚本里调用，
> 所以每条命令都有一个可调用的入口 —— 这不是顺手加的便利，是让菜单这一层能被自动验证的前提。

<a id="bskipconfirmation-suppresses-the-dialog-only"></a>

## `bSkipConfirmation` 只跳过对话框

它只压掉那个模态确认框，别的什么也不做。两条拒绝照样生效 ——
[DFX8010](../diagnostics/DFX8xxx.md#dfx8010)（有表达不了的东西）和
[DFX8011](../diagnostics/DFX8xxx.md#dfx8011)（已有别的源码声明这个资产）——
只是以 toast 和日志的形式报出来而不是弹窗。

理由很直接：一个没人去点的模态框会把调用线程卡死。

<a id="what-it-is-good-for"></a>

## 能拿它做什么

**批量迁移。** 扫一批资产，对每个能干净反编译的执行 adopt，其余记下来人工看：

```python
import unreal

lib = unreal.DreamFXEditorLibrary
registry = unreal.AssetRegistryHelpers.get_asset_registry()

for data in registry.get_assets_by_path("/Game/VFX", recursive=True):
    asset = data.get_asset()
    if isinstance(asset, unreal.NiagaraSystem):
        lib.export_system(asset)          # export everything; the originals are never touched
```

**在别的工具链里搭一道门。** 把 `verify_all_sources()` 的返回值喂给你自己的发布检查。

**在编辑器里做定时重建。** 换分支之后统一跑一次 `rebuild_all_sources()`，
而不是让 watcher 一次面对上百个改动（见[日常循环](../getting-started/workflow.md)的批量闸门）。

<a id="the-command-line-is-the-headless-path"></a>

## 命令行才是无编辑器路径

Python 接口跑在**编辑器进程里**。要 headless，用
[`dfx.ps1`](headless-compilation.md) —— 它跑 commandlet，一样的管线，一样的诊断码，而且不需要 UI。

| 想要 | 用 |
| --- | --- |
| 编辑器里自动化一串操作 | Python / 蓝图 |
| CI、批处理、代理 | `dfx.ps1` / `ci.ps1` |
