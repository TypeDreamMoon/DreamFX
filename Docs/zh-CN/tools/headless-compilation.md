---
title: "无头 Niagara 编译"
description: "无头 Niagara 编译"
---

<a id="headless-niagara-compilation"></a>

# 无头 Niagara 编译

一次系统构建（包括 `dfx.ps1 build <system.dfs> -Force`）会请求 Niagara 编译，并等待生成的可执行数据，然后才会给资产打戳或保存。CPU 脚本必须包含 VM 字节码以及与当前源码匹配的有效编译 ID。正在进行的或排队的编译、过期的结果、缺失的字节码，以及渲染器绑定刷新后第二次编译产生的错误，都会让构建失败。之前成功的编译状态不满足这些检查。

Niagara 会将引用的模块内联到系统可执行文件中。在更改并重新构建某个 `.dfm` 之后，请重新构建每一个使用它的 `.dfs`（或重新构建源码树）。系统戳会包含所选模块的编译输入，包括通过原生父发射器到达的模块，因此即使没有 `-Force`，模块内容的更改也会使普通构建失效。只更新模块资产并不会更新已保存系统的可执行文件。

引擎的 `Compiling System ...` 日志会在着色器工作进程实际编译任务时出现。派生数据缓存命中可以在没有该日志的情况下安装当前的 VM 数据；该行缺失并不是失败判据。`-NoShaderCompile` 和 `-PrecompiledShadersOnly` 会阻止 Niagara 编译，不得用于会写入包的 DreamFX 构建。

无头 Null RHI 会检查 CPU VM 编译和 GPU 转换数据。它们不能证明 GPU 效果渲染正确；若要验证这一点，请在带有图形 RHI 的编辑器中运行生成的 GPU 系统。

`DreamFX.Regression.Compile.ModuleChangeUpdatesVm` 会更改实际被引用的模块的 HLSL，在没有 `-Force` 的情况下重新构建同一个系统，并检查新编译的常量、字节码/字面量数据以及直接调用和原生继承栈的源 ID。它会检查未更改的模块仍然允许缓存命中，然后移除字节码同时保留成功状态，以验证完成门会拒绝不完整的可执行文件。该测试会在唯一的 `/Game` 路径下创建未保存的资产。

<a id="incremental-builds"></a>

## 增量构建

命令行构建默认是完整重建：CI 和验证门想要的是完整重建给出的答案，而不是取决于资产之前状态的答案。`-Incremental` 会选择启用编辑器在保存时使用的[增量构建](incremental-builds.md)：

```powershell
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Samples/NS_Spark.dfs -Incremental
UnrealEditor-Cmd.exe <Project>.uproject -run=DreamFX -File=<file> -Incremental
```

构建会将新计划与上次构建存储在资产中的摘要进行比较，只写入差异，当无法信任该摘要时会回退到完整重建。每个文件记录一行日志：

```text
incremental: 0 emitter(s) and 1 stack(s) rebuilt, 3 input(s) written, 1 reset, nothing to recompile
full rebuild (the asset changed since its last build)
```

增量构建会请求一次普通的、非强制的 Niagara 编译，它只会重新编译写入所触及图中涉及的那些脚本——只落在快速迭代参数上的值写入通常不会触及任何脚本。完整重建仍然会强制编译。无论哪种方式，构建之后都会用上述检查验证编译状态，并在任何内容过期时强制重新编译。

在一个 1230 行的系统上编辑一个值，写入约需 0.2 秒；commandlet 运行约需 3 秒，其中大部分是进程启动和一次冷模块探测，而正在运行的编辑器会缓存该探测。规划在每个进程中复用一个模块库，因此这些探测每次运行只做一次，而不是每次构建一次。

不要在门中使用 `-Incremental`。`verify` 和 `ci.ps1` 不需要它，而用来构建的门应当进行完整构建。
