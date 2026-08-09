<#
.SYNOPSIS
    The three measurements that close out a round. Requires the editor CLOSED.

.DESCRIPTION
    Nothing here is new; the point is that all three run against the *same* binary. Reading a build
    number taken before a fix next to an L1 number taken after it is how two of this project's wrong
    turns started, so these go together or not at all:

      build -All -Force   every source rebuilt, so no asset is carrying an earlier binary's output
      mirror-diff         L1, does a mirror re-export to the same text; L2, does it still compile
      corpus              the automation tests

    Steps 1 and 2 write packages, which is why the editor has to be closed: two processes saving the
    same package means the second one silently wins. L3 is the opposite -- it needs the editor OPEN,
    because a commandlet's null RHI does not simulate GPU emitters -- so it is not run here and the
    script prints how to run it instead.

    L1 and L2 answer different questions and a round is not closed on one of them. A mirror can
    compile perfectly and still simulate differently, and only L3 sees that.

.EXAMPLE
    ./.skill/close-round.ps1

.EXAMPLE
    ./.skill/close-round.ps1 -SkipTree    # mirror-diff and corpus against whatever is already built
#>
[CmdletBinding()]
param(
    # Skip the rebuild. Only honest when nothing has changed since the last one.
    [switch]$SkipTree
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $PSCommandPath
$dfx = Join-Path $here 'dfx.ps1'

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

if ($SkipTree) {
    Step 'Skipping the rebuild (-SkipTree)'
}
else {
    Step '1/3  build -All -Force'
    $watch = [Diagnostics.Stopwatch]::StartNew()
    & $dfx build -All -Force
    Write-Host ("  took {0:N1} min" -f $watch.Elapsed.TotalMinutes)
}

Step '2/3  mirror-diff -- L1 text, L2 compile'
& $dfx mirror-diff

Step '3/3  corpus'
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
