<#
.SYNOPSIS
    Headless driver for the DreamFX commandlet.

.DESCRIPTION
    Wraps `UnrealEditor-Cmd.exe <project> -run=DreamFX …` so an agent can build and inspect
    DreamFXLang sources without opening the editor, and gets a clean verdict back instead of
    several hundred lines of engine boot spam.

    On top of the raw commandlet it adds:
      * engine resolution from the .uproject's EngineAssociation (no hard-coded path),
      * project discovery by walking up from the source file or the working directory,
      * de-duplication of the doubled `LogInit: Display: LogDreamFX:` echo,
      * a report of every asset the run wrote, classified against git so a throw-away probe
        asset is distinguishable from a tracked asset the run just overwrote,
      * `-CleanNew`, which deletes only the assets this run created that were untracked.

    Exit code is the commandlet's own: 0 when there were no errors, otherwise the error count.

    Any command that writes assets -- `build` above all -- wants the editor CLOSED. Both processes
    save the same packages otherwise, and the one that saves second silently wins. The run warns when
    it sees an editor running; it cannot tell which project that editor has open, so it is a warning
    and not a refusal.

.EXAMPLE
    ./dfx.ps1 build DFX/Samples/NS_Spark.dfs -Force

.EXAMPLE
    ./dfx.ps1 build -All

.EXAMPLE
    ./dfx.ps1 verify -All

.EXAMPLE
    ./dfx.ps1 schema GravityForce

.EXAMPLE
    ./dfx.ps1 decompile-all -Path '/Game/FX+/Game/Explosions'
    Export every Niagara system under either path into DFX/Decompiled/, in one editor boot.

.EXAMPLE
    ./dfx.ps1 mirror-diff -Path /Game/FX
    Check each mirror against the asset it was exported from: same text, and it compiles.
#>
[CmdletBinding()]
param(
    # build  — generate assets from source
    # verify — check assets against source without writing anything
    # schema — print one module's input signature
    # list   — print every module (or, with -DynamicInputs, every dynamic input) on the search paths
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('build', 'verify', 'lint', 'decompile', 'decompile-all', 'mirror-diff', 'asset-diff', 'coverage', 'rename', 'graph', 'schema', 'list', 'corpus')]
    [string]$Command,

    # build/verify: path to a .dfs (absolute, or relative to the working directory).
    # schema: a module name such as GravityForce or /Niagara/Modules/Update/Forces/GravityForce.
    [Parameter(Position = 1)]
    [string]$Target,

    # Process every source under every DFX root instead of one file.
    [switch]$All,

    # Bypass the provenance-hash skip. Without it an unchanged source reports "up to date".
    [switch]$Force,

    # Build in memory without writing packages.
    [switch]$NoSave,

    # build: rebuild the edit context for every write, as the generator did before the write scope.
    # Slower by design -- it exists so a benchmark can measure both halves on one binary.
    [switch]$NoWriteScope,

    # build: drop the edit context after every structural write, as the generator did before P3.
    # Slower by design -- same reason as above, plus it is the escape hatch if an engine version
    # stops refreshing the stack group it just changed.
    [switch]$RebuildOnStructural,

    # build: how many systems may sit between compile request and finalize (the pipeline depth).
    # 1 restores the fully serial build; 0 keeps the commandlet default.
    [int]$Window = 0,

    # build: drop the edit context after every static-switch write, as before the engine's
    # synchronous module refresh. Slower by design -- the switch-refresh round's A/B and escape hatch.
    [switch]$RebuildOnSwitch,

    # build: build .dfm graphs through the reflection backend even on an engine that exports the
    # declarations directly. This is how the stock-engine path gets exercised on MoonEngine, and how
    # the two backends' output can be diffed against each other on one machine.
    [switch]$ForceReflectionBackend,

    # build: pay the engine's per-add stack refresh again instead of one batch refresh per stack.
    # Slower by design -- the AddModule-batching round's A/B and escape hatch.
    [switch]$RebuildPerAdd,

    # list: show dynamic inputs instead of modules.
    [switch]$DynamicInputs,

    # verify: treat an R7 module-version mismatch as an error rather than a warning. For a release
    # gate, where assets built against different modules than the text describes must not ship.
    [switch]$StrictVersions,

    # decompile: write the source here instead of printing it.
    [string]$Out,

    # decompile: the Root="..." to stamp on the output. Defaults to Game.
    [string]$Root,

    # coverage / decompile-all / mirror-diff: the content path(s) to scan, several separated by '+'.
    # Defaults to every project mount point.
    [string]$Path,

    # mirror-diff: skip the L2 compile check and report text equivalence only. Much faster, and the
    # right thing when the mirrors were built in the same session that is now diffing them.
    # decompile: print every input, including ones equal to a pristine module's. Diagnostic only --
    # the result is not meant to be maintained, it answers "what did the baseline hide?".
    [switch]$NoDefaults,

    # mirror-diff: skip L2. asset-diff: describe both sides as loaded instead of compiling them
    # first. Diagnostic only -- without the compile the `compiled` fact family reports each side's
    # compile history rather than its content, which is not a comparison of anything.
    [switch]$NoCompile,

    # schema: which stack to probe the module in. Defaults to trying each in turn.
    [string]$Stack,

    # The .uproject. Defaults to the nearest one at or above the target / working directory.
    [string]$Project,

    # Engine root (the directory containing Engine/Binaries). Defaults to the association lookup.
    [string]$Engine,

    # Delete the assets this run created that git reports as untracked. Tracked assets are
    # never touched -- they are reported instead.
    [switch]$CleanNew,

    # asset-diff: write both sides' full fact lists to Saved/DreamFX/*.facts. The console report
    # truncates every fact to 400 characters, which is exactly wrong for chasing a difference that
    # lives past that mark -- and the compiled-fact family routinely does.
    [switch]$DumpFacts,

    # Echo the full commandlet output instead of just the DreamFX lines.
    [switch]$Raw
)

$ErrorActionPreference = 'Stop'

# ---------------------------------------------------------------- project & engine

function Resolve-Uproject {
    param([string]$Explicit, [string]$StartAt)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) { throw "No .uproject at '$Explicit'." }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    # Walk up from the target file first, then from the working directory: a source file under
    # DFX/ sits inside the project, so this finds the right one even when the driver is invoked
    # from somewhere else entirely.
    $roots = @()
    if ($StartAt -and (Test-Path -LiteralPath $StartAt)) {
        $item = Get-Item -LiteralPath $StartAt
        $roots += if ($item.PSIsContainer) { $item.FullName } else { $item.DirectoryName }
    }
    $roots += (Get-Location).Path

    foreach ($root in $roots) {
        $dir = $root
        while ($dir) {
            $found = @(Get-ChildItem -LiteralPath $dir -Filter '*.uproject' -File -ErrorAction SilentlyContinue)
            if ($found.Count -gt 0) { return $found[0].FullName }
            $dir = Split-Path -Parent $dir
        }
    }

    throw "Could not find a .uproject at or above '$($roots -join "', '")'. Pass -Project."
}

function Resolve-EngineRoot {
    param([string]$Explicit, [string]$UprojectPath)

    if ($Explicit) { return (Resolve-Path -LiteralPath $Explicit).Path }
    if ($env:UE_ENGINE_ROOT) { return (Resolve-Path -LiteralPath $env:UE_ENGINE_ROOT).Path }

    $association = (Get-Content -LiteralPath $UprojectPath -Raw | ConvertFrom-Json).EngineAssociation
    if (-not $association) { throw "The .uproject has no EngineAssociation. Pass -Engine." }

    # A source build registers its root under a GUID association; an installed build uses a
    # version string that lives in a different key.
    $key = 'HKCU:\SOFTWARE\Epic Games\Unreal Engine\Builds'
    if (Test-Path $key) {
        $entry = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).$association
        if ($entry) { return (Resolve-Path -LiteralPath $entry).Path }
    }

    $installed = "HKLM:\SOFTWARE\EpicGames\Unreal Engine\$association"
    if (Test-Path $installed) {
        $entry = (Get-ItemProperty -Path $installed -ErrorAction SilentlyContinue).InstalledDirectory
        if ($entry) { return (Resolve-Path -LiteralPath $entry).Path }
    }

    throw "EngineAssociation '$association' is not registered. Pass -Engine <root> or set UE_ENGINE_ROOT."
}

$uproject = Resolve-Uproject -Explicit $Project -StartAt $Target
$projectRoot = Split-Path -Parent $uproject
$engineRoot = Resolve-EngineRoot -Explicit $Engine -UprojectPath $uproject
$editorCmd = Join-Path $engineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'

if (-not (Test-Path -LiteralPath $editorCmd)) {
    throw "UnrealEditor-Cmd.exe not found at '$editorCmd'. Is '$engineRoot' the directory containing Engine/?"
}

Write-Host "dfx: $Command  project=$(Split-Path -Leaf $uproject)  engine=$engineRoot" -ForegroundColor DarkGray

# ---------------------------------------------------------------- corpus
#
# The corpus suites are automation tests, not a commandlet mode, so that the same assertions run from
# the editor's Session Frontend without a second implementation. That means booting the editor instead
# of `-run=`, and reading the verdict out of the log: the automation controller does not set a process
# exit code, so a run whose exit code is 0 tells you nothing on its own.

if ($Command -eq 'corpus') {
    $filter = if ($Target) { $Target } else { 'DreamFX.Corpus' }

    # -stdout and -FullStdOutLogOutput are load-bearing: without them UnrealEditor-Cmd writes only to
    # the log file, this script reads an empty stream, and a run that failed looks exactly like a run
    # that passed. -TestExit is what makes the process leave once the queue drains.
    $corpusArguments = @(
        $uproject,
        "-ExecCmds=Automation RunTests $filter",
        '-TestExit=Automation Test Queue Empty',
        '-unattended', '-nopause', '-nosplash', '-nullrhi', '-NoLiveCoding',
        '-stdout', '-FullStdOutLogOutput'
    )

    $output = & $editorCmd @corpusArguments 2>&1
    $editorExit = $LASTEXITCODE
    $lines = @($output | ForEach-Object { "$_" })

    # The controller writes Result={Success} / Result={Fail}; the process exit code says nothing about
    # whether any test passed, so these two counts are the verdict. The exit code DOES say whether the
    # process survived: a crash mid-suite leaves the crashed test with no Result line at all, and the
    # counts alone then read "n passed" as if the suite were n tests long. That is how an assert in
    # test 6 of 9 once reported "corpus OK (5 passed)".
    $passed = @($lines | Where-Object { $_.Contains('Result={Success}') }).Count
    $failed = @($lines | Where-Object { $_.Contains('Result={Fail}') }).Count

    if ($editorExit -ne 0 -and $failed -eq 0) {
        foreach ($line in ($lines | Select-Object -Last 40)) {
            Write-Host ($line -replace '^\[[^\]]*\]\[\s*\d+\]', '') -ForegroundColor DarkGray
        }
        Write-Host ''
        Write-Host "dfx: corpus CRASHED (editor exit $editorExit after $passed passed); the log tail above is the crash site" -ForegroundColor Red
        exit 1
    }

    foreach ($line in $lines) {
        if ($Raw) { Write-Host $line; continue }
        if ($line -match 'Result=\{Fail\}|LogAutomationController: Error:') {
            Write-Host ($line -replace '^\[[^\]]*\]\[\s*\d+\]', '') -ForegroundColor Red
        }
    }

    Write-Host ''
    if ($passed + $failed -eq 0) {
        Write-Host "dfx: corpus ran no tests for filter '$filter'. Is DreamFXEditor built and enabled?" -ForegroundColor Red
        exit 1
    }
    if ($failed -gt 0) {
        Write-Host "dfx: corpus FAILED ($failed failed, $passed passed)" -ForegroundColor Red
        exit $failed
    }
    Write-Host "dfx: corpus OK ($passed passed)" -ForegroundColor Green
    exit 0
}

# ---------------------------------------------------------------- argument assembly

$arguments = @($uproject, '-run=DreamFX')

# Read by FGraphSurgeon::Create, not by the commandlet, so it is not part of any one command.
if ($ForceReflectionBackend) { $arguments += '-DreamFXForceReflectionBackend' }

switch ($Command) {
    'build' {
        if (-not $All) {
            if (-not $Target) { throw "build needs a source file, or -All." }
            $arguments += "-File=$((Resolve-Path -LiteralPath $Target).Path)"
        }
        if ($Force) { $arguments += '-Force' }
        if ($NoSave) { $arguments += '-NoSave' }
        if ($NoWriteScope) { $arguments += '-NoWriteScope' }
        if ($RebuildOnStructural) { $arguments += '-RebuildOnStructural' }
        if ($RebuildOnSwitch) { $arguments += '-RebuildOnSwitch' }
        if ($RebuildPerAdd) { $arguments += '-RebuildPerAdd' }
        if ($Window -gt 0) { $arguments += "-Window=$Window" }
    }
    'verify' {
        if (-not $All) {
            if (-not $Target) { throw "verify needs a source file, or -All." }
            $arguments += "-File=$((Resolve-Path -LiteralPath $Target).Path)"
        }
        $arguments += '-Verify'
        if ($StrictVersions) { $arguments += '-StrictVersions' }
    }
    'lint' {
        if (-not $All) {
            if (-not $Target) { throw "lint needs a source file, or -All." }
            $arguments += "-File=$((Resolve-Path -LiteralPath $Target).Path)"
        }
        $arguments += '-Lint'
    }
    'decompile' {
        if (-not $Target) { throw "decompile needs an asset path, e.g. /Game/FX/NS_Spark." }
        $arguments += "-Decompile=$Target"
        if ($Out) { $arguments += "-Out=$Out" }
        if ($Root) { $arguments += "-Root=$Root" }
        if ($NoDefaults) { $arguments += '-NoDefaults' }
    }
    'decompile-all' {
        $arguments += '-DecompileAll'
        if ($Path) { $arguments += "-Path=$Path" }
    }
    'mirror-diff' {
        $arguments += '-MirrorDiff'
        if ($Path) { $arguments += "-Path=$Path" }
        if ($NoCompile) { $arguments += '-NoCompile' }
    }
    'asset-diff' {
        $arguments += '-AssetDiff'
        if ($Path) { $arguments += "-Path=$Path" }
        if ($DumpFacts) { $arguments += '-DreamFXDumpFacts' }
        if ($NoCompile) { $arguments += '-NoCompile' }
    }
    'coverage' {
        $arguments += '-Coverage'
        if ($Path) { $arguments += "-Path=$Path" }
    }
    'rename' {
        if (-not $Target) { throw "rename needs <asset>:<oldName>:<newName>, e.g. /Game/FX/NS_Spark:Sparks:Embers" }
        $arguments += "-Rename=$Target"
    }
    'graph' {
        $arguments += '-Graph'
    }
    'schema' {
        if (-not $Target) { throw "schema needs a module name." }
        $arguments += "-Schema=$Target"
        if ($Stack) { $arguments += "-Stack=$Stack" }
    }
    'list' {
        $arguments += if ($DynamicInputs) { '-ListDynamicInputs' } else { '-ListModules' }
    }
}

$arguments += @('-unattended', '-nopause', '-nosplash', '-nullrhi')

# ---------------------------------------------------------------- snapshot for -CleanNew

function Get-AssetSnapshot {
    param([string]$Root)
    $map = @{}
    foreach ($dir in @((Join-Path $Root 'Content'), (Join-Path $Root 'Plugins'))) {
        if (-not (Test-Path -LiteralPath $dir)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $dir -Filter '*.uasset' -File -Recurse -ErrorAction SilentlyContinue) {
            $map[$file.FullName] = $file.LastWriteTimeUtc
        }
    }
    return $map
}

$before = if ($CleanNew -or $Command -eq 'build') { Get-AssetSnapshot -Root $projectRoot } else { @{} }

# ---------------------------------------------------------------- run

$output = & $editorCmd @arguments 2>&1
$exit = $LASTEXITCODE

# The commandlet's own return value, taken from the line the engine prints when it honours the exit
# request:  "Engine exit requested (reason: Commandlet DreamFXCommandlet_0 finished execution
# (result 0))".  That number is what the commandlet returned, which is the error count -- the thing
# a CI gate is actually asking about.
#
# It is preferred over the process exit code because on this project the two disagree: `verify -All`
# reliably returns 0, logs no error in any category, shuts down cleanly (LogExit: Exiting., log file
# closed) and still leaves the process at 3.  Ruled out by measurement, not assumed: an abort during
# teardown (the shutdown is clean and byte-identical to a run that exits 0), a side effect of the
# stats report verify skips (BuildReport only reads), unsaved dirty packages (`build -All -NoSave`
# exits 0), Angelscript's warnings (`lint -All` carries the same ten and exits 0), and the verify
# path itself (a single-file verify exits 0).  Something downstream of the commandlet corrupts the
# code without saying so.
#
# This is not a way of ignoring failures: a non-zero result still fails, and a run that never reaches
# the line keeps whatever the process reported.  It replaces a proxy with the value the proxy was
# standing in for.
# The line is written to the project log, not to stdout, so it is read back from there.
$engineLog = Join-Path $projectRoot ('Saved/Logs/' + [IO.Path]::GetFileNameWithoutExtension($uproject) + '.log')
if (Test-Path -LiteralPath $engineLog) {
    $reported = Select-String -LiteralPath $engineLog -Pattern 'finished execution \(result (\d+)\)' |
        Select-Object -Last 1
    if ($reported) {
        $commandletResult = [int]$reported.Matches[0].Groups[1].Value
        if ($commandletResult -ne $exit) {
            Write-Host "dfx: process exit $exit, commandlet returned $commandletResult -- using the commandlet's" -ForegroundColor DarkYellow
        }
        $exit = $commandletResult
    }
}

if ($Raw) {
    $output | ForEach-Object { $_ }
}
else {
    # Output is grouped into records before anything is filtered, because a UE_LOG of a multi-line
    # message writes the category prefix on the FIRST line only. Filtering line by line therefore
    # kept the first line of a diagnostic and threw away every line after it -- so DFX6006, whose
    # whole value is the translator's own error text, arrived ending in a bare colon and saying
    # nothing. That cost an afternoon of looking for the missing message in the compiler.
    #
    # A record starts at a line that opens with a log category, optionally behind the engine's
    # [timestamp][frame] stamp. Anything else continues the record above it.
    $records = @()
    $current = $null
    foreach ($line in $output) {
        $text = "$line"
        if ($text -match '^(\[[^\]]*\]\[\s*\d+\])?[A-Za-z_][A-Za-z0-9_]*:\s') {
            if ($null -ne $current) { $records += , $current }
            $current = @($text)
        }
        elseif ($null -ne $current) {
            $current += $text
        }
        # A continuation before any record has started belongs to nothing and is dropped, which is
        # what happens to the engine's pre-log banner.
    }
    if ($null -ne $current) { $records += , $current }

    # Every DreamFX record is emitted twice: once raw and once re-wrapped through LogInit, always
    # adjacent. Collapsing only *consecutive* duplicates is what makes this safe -- a global
    # seen-set also eats lines that legitimately repeat, such as the same module appearing in the
    # dependency listing of two different source files.
    $previous = $null
    foreach ($record in $records) {
        if ($record[0] -notmatch 'LogDreamFX') { continue }

        $stripped = @($record | ForEach-Object { $_ -replace '^\[[^\]]*\]\[\s*\d+\]', '' })
        $stripped[0] = $stripped[0] -replace '^LogInit: Display: ', ''

        $joined = $stripped -join "`n"
        if ($joined -eq $previous) { continue }
        $previous = $joined

        $colour = if ($stripped[0] -match ': Error:| error DFX') { 'Red' }
                  elseif ($stripped[0] -match ': Warning:| warning DFX') { 'Yellow' }
                  else { 'Gray' }
        foreach ($outputLine in $stripped) {
            Write-Host $outputLine -ForegroundColor $colour
        }
    }
}

# ---------------------------------------------------------------- asset report

if ($before.Count -gt 0) {
    $after = Get-AssetSnapshot -Root $projectRoot
    $touched = @()
    foreach ($path in $after.Keys) {
        if (-not $before.ContainsKey($path) -or $before[$path] -ne $after[$path]) { $touched += $path }
    }

    if ($touched.Count -gt 0) {
        Write-Host ''
        Write-Host 'Assets written to disk by this run:' -ForegroundColor DarkGray

        foreach ($path in $touched | Sort-Object) {
            $relative = [System.IO.Path]::GetRelativePath($projectRoot, $path) -replace '\\', '/'

            # Query from the asset's own directory, not the project root: plugins are frequently
            # their own repositories, and asking the project root about a plugin file returns
            # "fatal: not a git repository" -- which reads identically to "tracked and unchanged"
            # and would make -CleanNew skip exactly the files it exists to remove.
            $assetDir = Split-Path -Parent $path
            $repoRoot = & git -C $assetDir rev-parse --show-toplevel 2>$null

            if (-not $repoRoot) {
                Write-Host "  $relative  [written, not under version control]" -ForegroundColor Green
                if ($CleanNew) {
                    Remove-Item -LiteralPath $path -Force
                    Write-Host '    deleted (-CleanNew)' -ForegroundColor DarkGray
                }
                continue
            }

            # --ignored=matching reports individual ignored files rather than collapsing them into
            # their ignored parent directory.
            $status = & git -C $assetDir status --porcelain --untracked-files=all --ignored=matching -- $path 2>$null

            $ignored = $false
            if (-not $status) {
                & git -C $assetDir check-ignore -q -- $path 2>$null
                $ignored = $LASTEXITCODE -eq 0
            }

            if ($status -match '^\?\?' -or $status -match '^!!' -or $ignored) {
                $label = if ($ignored -or $status -match '^!!') { 'NEW (gitignored)' } else { 'NEW (untracked)' }
                Write-Host "  $relative  [$label]" -ForegroundColor Green
                if ($CleanNew) {
                    Remove-Item -LiteralPath $path -Force
                    Write-Host '    deleted (-CleanNew)' -ForegroundColor DarkGray
                }
            }
            elseif ($status) {
                # git already knows this file, so the run overwrote content someone may want to
                # keep -- that is the author's call, not ours.
                Write-Host "  $relative  [TRACKED AND MODIFIED]" -ForegroundColor Red
                Write-Host "    restore with: git -C `"$repoRoot`" checkout -- `"$path`"" -ForegroundColor DarkGray
            }
            else {
                Write-Host "  $relative  [rewritten, byte-identical]" -ForegroundColor DarkGray
            }
        }
    }
}

Write-Host ''
if ($exit -eq 0) {
    Write-Host "dfx: OK (exit 0)" -ForegroundColor Green
}
else {
    Write-Host "dfx: FAILED (exit $exit)" -ForegroundColor Red
}
exit $exit
