<#
.SYNOPSIS
    DreamFX CI gate.

.DESCRIPTION
    Runs the three checks that together mean "the text and the assets agree", in the order that
    gives the most useful first failure:

      1. lint    -- static checks over source. No asset access, so it fails fastest.
      2. build   -- every .dfs generates and its Niagara compile is clean.
      3. verify  -- every generated asset carries a provenance stamp matching its source.

    Step 3 is the one that catches the case nobody notices: someone edited a .dfs, did not rebuild,
    and committed both. Build alone would pass, because build fixes it.

    Exit code 0 means all three passed. Anything else is the first failing step's code.

.EXAMPLE
    ./ci.ps1

.EXAMPLE
    ./ci.ps1 -SkipBuild
    Check only, for a gate that must not write to the working tree.
#>
[CmdletBinding()]
param(
    # The .uproject. Defaults to the nearest one above this script.
    [string]$Project,

    # Engine root. Defaults to the EngineAssociation lookup.
    [string]$Engine,

    # Do not run the build step. verify then reports any source that has not been built.
    [switch]$SkipBuild,

    # Delete assets the build newly created. For a gate that should leave no trace.
    [switch]$CleanNew
)

$ErrorActionPreference = 'Stop'
$driver = Join-Path $PSScriptRoot 'dfx.ps1'

function Invoke-Step {
    param([string]$Name, [string[]]$Arguments)

    Write-Host ''
    Write-Host "=== $Name ===" -ForegroundColor Cyan

    & pwsh -NoProfile -File $driver @Arguments
    $exit = $LASTEXITCODE

    if ($exit -ne 0) {
        Write-Host ''
        Write-Host "ci: FAILED at '$Name' (exit $exit)" -ForegroundColor Red
        exit $exit
    }
}

$common = @()
if ($Project) { $common += @('-Project', $Project) }
if ($Engine) { $common += @('-Engine', $Engine) }

Invoke-Step -Name 'lint' -Arguments (@('lint', '-All') + $common)

if (-not $SkipBuild) {
    $buildArguments = @('build', '-All', '-Force') + $common
    if ($CleanNew) { $buildArguments += '-CleanNew' }
    Invoke-Step -Name 'build' -Arguments $buildArguments
}

# Skipped after -CleanNew: the assets it would check were just deleted on purpose.
if (-not $CleanNew) {
    Invoke-Step -Name 'verify' -Arguments (@('verify', '-All') + $common)
}

Write-Host ''
Write-Host 'ci: OK' -ForegroundColor Green
exit 0
