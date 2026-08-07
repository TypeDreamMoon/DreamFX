# DreamFX

用文本源码编写 Niagara 特效，自动生成标准 `UNiagaraSystem` / `UNiagaraEmitter` / `UNiagaraScript` 资产。

DreamShader 对材质做的事，DreamFX 对 VFX 再做一遍。

| 后缀 | 内容 | 生成资产 |
| --- | --- | --- |
| `.dfs` | System：user 参数、system stack、emitter 列表 | `UNiagaraSystem` |
| `.dfe` | 可复用 Emitter | `UNiagaraEmitter` |
| `.dfm` | Module / Dynamic Input | `UNiagaraScript` |

## 当前状态

**设计阶段 —— 尚无代码。** 目录骨架已建，`.uplugin` 的 `Modules` 为空数组，插件当前是惰性的，不会影响编辑器启动。每落地一个模块再往 `Modules` 里加一条。

设计、可行性依据、风险与路线图见 `Docs/plan.md`（本地维护，不纳入版本管理）。
