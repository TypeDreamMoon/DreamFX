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

.EXAMPLE
    ./dfx.ps1 build DFX/Samples/NS_Spark.dfs -Force

.EXAMPLE
    ./dfx.ps1 build -All

.EXAMPLE
    ./dfx.ps1 verify -All

.EXAMPLE
    ./dfx.ps1 schema GravityForce
#>
[CmdletBinding()]
param(
    # build  — generate assets from source
    # verify — check assets against source without writing anything
    # schema — print one module's input signature
    # list   — print every module (or, with -DynamicInputs, every dynamic input) on the search paths
    [Parameter(Mandatory, Position = 0)]
    [ValidateSet('build', 'verify', 'schema', 'list')]
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

    # list: show dynamic inputs instead of modules.
    [switch]$DynamicInputs,

    # The .uproject. Defaults to the nearest one at or above the target / working directory.
    [string]$Project,

    # Engine root (the directory containing Engine/Binaries). Defaults to the association lookup.
    [string]$Engine,

    # Delete the assets this run created that git reports as untracked. Tracked assets are
    # never touched -- they are reported instead.
    [switch]$CleanNew,

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

# ---------------------------------------------------------------- argument assembly

$arguments = @($uproject, '-run=DreamFX')

switch ($Command) {
    'build' {
        if (-not $All) {
            if (-not $Target) { throw "build needs a source file, or -All." }
            $arguments += "-File=$((Resolve-Path -LiteralPath $Target).Path)"
        }
        if ($Force) { $arguments += '-Force' }
        if ($NoSave) { $arguments += '-NoSave' }
    }
    'verify' {
        if (-not $All) {
            if (-not $Target) { throw "verify needs a source file, or -All." }
            $arguments += "-File=$((Resolve-Path -LiteralPath $Target).Path)"
        }
        $arguments += '-Verify'
    }
    'schema' {
        if (-not $Target) { throw "schema needs a module name." }
        $arguments += "-Schema=$Target"
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

if ($Raw) {
    $output | ForEach-Object { $_ }
}
else {
    # Every DreamFX line is emitted twice: once raw and once re-wrapped through LogInit.
    $seen = [System.Collections.Generic.HashSet[string]]::new()
    foreach ($line in $output) {
        $text = "$line"
        if ($text -notmatch 'LogDreamFX') { continue }
        $stripped = ($text -replace '^\[[^\]]*\]\[\s*\d+\]', '') -replace '^LogInit: Display: ', ''
        if (-not $seen.Add($stripped)) { continue }

        $colour = if ($stripped -match ': Error:| error DFX') { 'Red' }
                  elseif ($stripped -match ': Warning:| warning DFX') { 'Yellow' }
                  else { 'Gray' }
        Write-Host $stripped -ForegroundColor $colour
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
            $status = & git -C $projectRoot status --porcelain --untracked-files=all -- $relative 2>$null

            # Untracked shows as '??'. Anything else means git already knows the file, so the run
            # overwrote content someone may want to keep -- that is the author's call, not ours.
            if ($status -match '^\?\?') {
                Write-Host "  $relative  [NEW (untracked)]" -ForegroundColor Green
                if ($CleanNew) {
                    Remove-Item -LiteralPath $path -Force
                    Write-Host '    deleted (-CleanNew)' -ForegroundColor DarkGray
                }
            }
            elseif ($status) {
                Write-Host "  $relative  [TRACKED AND MODIFIED]" -ForegroundColor Red
                Write-Host "    restore with: git -C `"$projectRoot`" checkout -- `"$relative`"" -ForegroundColor DarkGray
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
