<#
.SYNOPSIS
    DreamFX CI gate.

.DESCRIPTION
    Runs the checks that together mean "the text and the assets agree", in the order that
    gives the most useful first failure:

      0. checks  -- diagnostic reference is current and driver regressions pass.
      1. lint    -- static checks over source. No asset access, so it fails fastest.
      2. build   -- every .dfs and .dfm generates and its Niagara compile is clean.
      3. verify  -- every generated asset carries a provenance stamp matching its source.
      4. tests   -- all DreamFX suites, including corpus, language, workspace and engine probes.

    Step 3 is the one that catches the case nobody notices: someone edited a .dfs, did not rebuild,
    and committed both. Build alone would pass, because build fixes it.

    Step 4 is the one that catches a behaviour changing rather than breaking. Almost everything
    DreamFX knows about Niagara was established by experiment, not guaranteed by a type; the corpus
    is what makes a quiet change in any of it fail.

    Exit code 0 means all selected checks passed. Anything else is the first failing step's code.

    RUN THIS WITH THE EDITOR CLOSED. Steps 2 and 4 write .uasset files, and an editor open on the
    same project writes them too -- whichever saves second wins, and neither says so. The build warns
    when it sees an editor running, but it cannot tell which project that editor has open, so the
    warning is advice rather than a gate.

.EXAMPLE
    ./ci.ps1

.EXAMPLE
    ./ci.ps1 -SkipBuild -SkipCorpus
    Check only: both build and automation suites can write assets.
#>
[CmdletBinding()]
param(
    # The .uproject. Defaults to the nearest one above this script.
    [string]$Project,

    # Engine root. Defaults to the EngineAssociation lookup.
    [string]$Engine,

    # Do not run the build step. verify then reports any source that has not been built.
    [switch]$SkipBuild,

    # Do not run the DreamFX automation suites. They boot the editor; skipping is for a quick
    # local check, never for the complete gate. The switch name is kept for existing callers.
    [switch]$SkipCorpus,

    # Delete assets the build newly created. For a gate that should leave no trace.
    [switch]$CleanNew
)

$ErrorActionPreference = 'Stop'
$driver = Join-Path $PSScriptRoot 'dfx.ps1'

function Invoke-Step {
    param([string]$Name, [string[]]$Arguments, [string]$Script = $driver)

    Write-Host ''
    Write-Host "=== $Name ===" -ForegroundColor Cyan

    & pwsh -NoProfile -File $Script @Arguments
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

Invoke-Step -Name 'diagnostics' -Script (Join-Path $PSScriptRoot 'gen-diagnostics.ps1') -Arguments @('-Check')
Invoke-Step -Name 'driver regressions' -Script (Join-Path $PSScriptRoot '../Tests/Tools/Test-Driver.ps1') -Arguments @()
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

if (-not $SkipCorpus) {
    Invoke-Step -Name 'DreamFX automation' -Arguments (@('corpus', 'DreamFX') + $common)
}

Write-Host ''
Write-Host 'ci: OK' -ForegroundColor Green
exit 0
