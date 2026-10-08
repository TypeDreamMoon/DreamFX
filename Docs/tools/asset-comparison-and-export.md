# Asset comparison and export coverage

`asset-diff` compares the original Niagara systems with their `Decompiled/` mirrors. Its exit status is nonzero when any pair differs, a mirror or original cannot be loaded, a content root cannot be resolved, or either side fails compilation. Failed comparisons are counted separately and never reported as `SAME`. By default both sides are recompiled before comparison; `-NoCompile` explicitly compares their currently available data without validating a new compilation.

System facts include authored runtime settings such as fixed tick timing, warmup timing and count, bounds, rendering flags and scalability settings. Editor-only metadata, transient state, graph ownership and derived compilation caches are excluded from this configuration comparison. The existing script and emitter facts remain independent checks.

Decompiler coverage gaps mean that the exported source cannot reproduce the entire asset. Lightweight/Stateless emitters produce `DFX8017` and a gap in the exported header; their unsupported contents are not replaced with an ordinary empty emitter block. Adopt rejects gap-bearing exports before rebuilding.

User parameter descriptions escape quotes, backslashes, line breaks, carriage returns and tabs so that parsing the exported description preserves its original text. Renderer material user bindings, including entries inside mesh `OverrideMaterials`, export stable parameter names. Their renderer-defined material type comes from the fresh renderer or array element constructor rather than a process-local Niagara type registry index. A binding with a different or unreadable type is reported as a coverage gap.
