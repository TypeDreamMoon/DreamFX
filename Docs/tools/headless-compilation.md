# Headless Niagara compilation

A system build, including `dfx.ps1 build <system.dfs> -Force`, requests Niagara compilation and waits
for the resulting executable data before stamping or saving the asset. CPU scripts must contain VM
bytecode and a valid compile ID matching the current source. Active or queued compilation, stale
results, missing bytecode, and errors from a second compile after renderer-binding refresh all fail
the build. A previously successful compile status does not satisfy those checks.

Niagara inlines referenced modules into system executables. After changing and rebuilding a `.dfm`,
rebuild every consuming `.dfs` (or rebuild the source tree). System stamps include the selected
modules' compile inputs, including modules reached through native parent emitters, so a module
content change invalidates an ordinary build even without `-Force`. Updating only the module asset
does not update a saved system's executable.

The engine's `Compiling System ...` log appears when shader workers actually compiled jobs. A derived
data cache hit can install current VM data without that log; the absence of the line is not a failure
criterion. `-NoShaderCompile` and `-PrecompiledShadersOnly` prevent Niagara compilation and must not be
used for package-writing DreamFX builds.

Headless Null RHI checks CPU VM compilation and GPU translation data. They do not prove that a GPU
effect renders correctly; run the generated GPU system in an editor with a graphics RHI for that check.

`DreamFX.Regression.Compile.ModuleChangeUpdatesVm` changes an actual referenced module's HLSL,
rebuilds the same system without `-Force`, and checks the new compiled constant, bytecode/literal
data, and source ID for direct calls and native inherited stacks. It checks that an unchanged module
still allows a cache hit, then removes bytecode while preserving the successful status to verify that the completion gate
rejects an incomplete executable. The test creates unsaved assets under a unique `/Game` path.

## Incremental builds

A command-line build is a full rebuild by default: CI and the verify gate want the full rebuild's
answer, not one that depends on what the asset held before. `-Incremental` opts in to the
[incremental build](incremental-builds.md) the editor uses on save:

```powershell
pwsh -File Plugins/DreamFX/.skill/dfx.ps1 build DFX/Samples/NS_Spark.dfs -Incremental
UnrealEditor-Cmd.exe <Project>.uproject -run=DreamFX -File=<file> -Incremental
```

The build compares the new plan with the digest the last build stored in the asset and writes only the
difference, falling back to the full rebuild when it cannot trust the digest. Each file logs one line:

```text
incremental: 0 emitter(s) and 1 stack(s) rebuilt, 3 input(s) written, 1 reset, nothing to recompile
full rebuild (the asset changed since its last build)
```

An incremental build requests an ordinary, non-forced Niagara compile, which recompiles only the
scripts whose graphs the writes reached — value writes that only land on rapid-iteration parameters
usually reach none. A full rebuild still forces the compile. Either way the build verifies the compiled
state afterwards, with the checks above, and forces a recompile if anything is stale.

A one-value edit on a 1230-line system writes in ~0.2 s; the commandlet run takes ~3 s, most of it
process startup and a cold module probe that a running editor caches. Planning reuses one module
library per process, so those probes are made once per run rather than once per build.

Do not use `-Incremental` in a gate. `verify` and `ci.ps1` do not need it, and a gate that builds
should build in full.
