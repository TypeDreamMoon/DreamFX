# Headless Niagara compilation

A system build, including `dfx.ps1 build <system.dfs> -Force`, requests Niagara compilation and waits
for the resulting executable data before stamping or saving the asset. CPU scripts must contain VM
bytecode and a valid compile ID matching the current source. Active or queued compilation, stale
results, missing bytecode, and errors from a second compile after renderer-binding refresh all fail
the build. A previously successful compile status does not satisfy those checks.

Niagara inlines referenced modules into system executables. After changing and rebuilding a `.dfm`,
rebuild every consuming `.dfs` with `-Force` (or rebuild the source tree). Updating only the module
asset does not update a saved system's executable.

The engine's `Compiling System ...` log appears when shader workers actually compiled jobs. A derived
data cache hit can install current VM data without that log; the absence of the line is not a failure
criterion. `-NoShaderCompile` and `-PrecompiledShadersOnly` prevent Niagara compilation and must not be
used for package-writing DreamFX builds.

Headless Null RHI checks CPU VM compilation and GPU translation data. They do not prove that a GPU
effect renders correctly; run the generated GPU system in an editor with a graphics RHI for that check.

`DreamFX.Regression.Compile.ModuleChangeUpdatesVm` changes an actual referenced module's HLSL,
rebuilds the same system, and checks the new compiled constant, bytecode/literal data, and source ID.
It also removes bytecode while preserving the successful status to verify that the completion gate
rejects an incomplete executable. The test creates unsaved assets under a unique `/Game` path.
