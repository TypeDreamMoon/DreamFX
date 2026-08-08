<#
.SYNOPSIS
    Everything the 2026-08-08 round still owes, in one run. Requires the editor CLOSED.

.DESCRIPTION
    Five changes landed after the last full measurement, and none of them are in the binary that
    produced it. Reading the old numbers as if they still applied is how the last two wrong turns
    started, so this re-establishes all of them together rather than one at a time:

      * NiagaraFluids is now enabled, which closed all 23 gaps in NS_Spawn_Ninja_Root
      * the engine fix that lets a data-interface user parameter be created at all
      * the prune pass that drops user parameters the source no longer declares (UNVALIDATED --
        this run is its first real test)
      * the switch-aware defaults probe was reverted, so exports are back to the pristine baseline
      * the evidence behind the last L1 diagnosis was taken with that reverted binary and is void

    Steps 1-4 need the editor closed because they write packages: two processes saving the same
    package means the second one silently wins. Step 5 needs the editor OPEN, so it is not run here
    -- the script prints how to run it instead.

.EXAMPLE
    ./.skill/close-round.ps1
    ./.skill/close-round.ps1 -SkipTree    # just re-export Ninja_Root and build it
#>
[CmdletBinding()]
param(
    # Skip the whole-tree build and mirror-diff. Leaves step 1-2 and the corpus.
    [switch]$SkipTree
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSCommandPath
$dfx = Join-Path $here 'dfx.ps1'
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $here))
$ninjaSource = Join-Path $repoRoot 'DFX/Decompiled/Game/_LevelUpSpawn/NS/NS_Spawn_Ninja_Root.dfs'

$editor = Get-Process UnrealEditor -ErrorAction SilentlyContinue
if ($editor) {
    Write-Host "The Unreal editor is running (pid $($editor.Id))." -ForegroundColor Red
    Write-Host "Close it first: this writes packages, and whichever process saves second wins." -ForegroundColor Red
    exit 1
}

function Step([string]$Title) {
    Write-Host ''
    Write-Host "=== $Title ===" -ForegroundColor Cyan
}

# ---------------------------------------------------------------- 1. Ninja_Root, with Fluids on

Step '1/4  Re-export NS_Spawn_Ninja_Root now that NiagaraFluids is enabled'

# Verified read-only while the editor was open: 23 gaps -> 0, and the export grew 576 -> 724 lines
# with the 19 Grid3D / SetFluidSourceAttributes modules that used to be dropped.
& $dfx decompile '/Game/_LevelUpSpawn/NS/NS_Spawn_Ninja_Root' -Out $ninjaSource
$gaps = @(Select-String -Path $ninjaSource -Pattern '^//   - ').Count
Write-Host ("  gaps in the re-exported source: {0} (was 23)" -f $gaps)

Step '2/4  Build it -- it used to fail on Grid3D_GAS_CONTROLS_SPAWN.Gravity being read before set'
& $dfx build $ninjaSource -Force
Write-Host ("  exit {0}" -f $LASTEXITCODE)

if ($SkipTree) {
    Step 'Skipping the tree (-SkipTree)'
}
else {
    Step '3/4  Whole tree, re-measured against every change since the last reading'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $dfx build -All -Force
    Write-Host ("  build -All took {0:N1} min" -f $watch.Elapsed.TotalMinutes)

    Step '4/4  mirror-diff -- L1 text, L2 compile'
    & $dfx mirror-diff
}

Step 'Corpus'
& $dfx corpus

# ---------------------------------------------------------------- what still needs the editor

Write-Host ''
Write-Host '=== Still owed, and it needs the editor OPEN ===' -ForegroundColor Yellow
Write-Host '  L3 runtime equivalence -- the only check that notices a mirror which builds'
Write-Host '  cleanly and simulates differently. Open the editor and run:'
Write-Host ''
Write-Host '      py "Plugins/DreamFX/.skill/l3_equivalence.py"'
Write-Host ''
Write-Host '  It writes Saved/DreamFX/l3-report.md.'
