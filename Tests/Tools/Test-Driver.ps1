$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
. (Join-Path $repo '.skill/dfx-lib.ps1')

function Assert-Equal($Actual, $Expected, [string]$Because) {
    if ($Actual -cne $Expected) { throw "$Because : expected '$Expected', got '$Actual'" }
}

$success = 'Engine exit requested (reason: Commandlet DreamFXCommandlet_0 finished execution (result 0))'
Assert-Equal (Get-DreamFXExitCode 7 @()) 7 'Startup failure without a current log'
Assert-Equal (Get-DreamFXExitCode 7 @('Commandlet OtherCommandlet_0 finished execution (result 0)')) 7 'Other commandlets cannot supply the verdict'
Assert-Equal (Get-DreamFXExitCode 7 @($success, 'LogExit: Exiting.')) 7 'Unexpected process failure cannot be hidden'
Assert-Equal (Get-DreamFXExitCode 3 @($success)) 3 'Exit 3 without clean shutdown fails'
Assert-Equal (Get-DreamFXExitCode 3 @($success, 'LogExit: Exiting.')) 0 'Known clean shutdown anomaly'
Assert-Equal (Get-DreamFXExitCode 3 @($success, 'Fatal error: crash', 'LogExit: Exiting.')) 3 'Fatal shutdown cannot pass'
Assert-Equal (Get-DreamFXExitCode 0 @('Commandlet DreamFXCommandlet_0 finished execution (result 2)')) 2 'Diagnostic failures are propagated'

# Execute the actual driver's asset-report branch with controlled snapshots and
# external commands. No fake .uasset files are created and Remove-Item is intercepted.
$tokens = $null
$parseErrors = $null
$driver = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $repo '.skill/dfx.ps1'), [ref]$tokens, [ref]$parseErrors)
Assert-Equal $parseErrors.Count 0 'Driver syntax'
$snapshot = $driver.Find({ param($node)
    $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-AssetSnapshot'
}, $false)
if (-not $snapshot) { throw 'Production snapshot function not found' }
& {
    . ([scriptblock]::Create($snapshot.Extent.Text))
    function Test-Path { param($LiteralPath) return $true }
    function Get-ChildItem {
        [CmdletBinding()]
        param($LiteralPath, $Filter, [switch]$File, [switch]$Recurse)
        Write-Error 'Simulated incomplete asset scan'
    }
    $failed = $false
    try { $null = Get-AssetSnapshot -Root $repo }
    catch { $failed = $_.Exception.Message -match 'Simulated incomplete asset scan' }
    Assert-Equal $failed $true 'An incomplete snapshot must abort before cleanup'
}
$report = @($driver.EndBlock.Statements | Where-Object {
    $_ -is [System.Management.Automation.Language.IfStatementAst] -and
    $_.Clauses[0].Item1.Extent.Text -eq '$trackAssets'
})
Assert-Equal $report.Count 1 'Locate the production asset report'
$reportBlock = [scriptblock]::Create($report[0].Extent.Text)
function Get-AssetSnapshot { param($Root) return $script:after }
function git {
    $global:LASTEXITCODE = 0
    if ($args -contains 'rev-parse') {
        if ($script:gitMode -eq 'none') { $global:LASTEXITCODE = 128; return 'fatal: not a git repository' }
        return $projectRoot
    }
    if ($args -contains 'status') {
        if ($script:gitMode -eq 'error') { $global:LASTEXITCODE = 128; return 'fatal: permission denied' }
        return $script:gitMode
    }
    throw 'Unexpected git invocation'
}
function Remove-Item { param($LiteralPath, [switch]$Force) $script:removed += $LiteralPath }
$projectRoot = $repo
$asset = Join-Path $repo 'Content/RegressionSentinel.uasset'
$CleanNew = $true
$trackAssets = $true
foreach ($mode in @('?? Content/RegressionSentinel.uasset', '!! Content/RegressionSentinel.uasset', 'none')) {
    $script:gitMode = $mode
    $before = @{$asset = 1}
    $script:after = @{$asset = 2}
    $script:removed = @()
    . $reportBlock
    Assert-Equal $script:removed.Count 0 "Preserve pre-existing asset ($mode)"

    $before = @{}
    . $reportBlock
    Assert-Equal $script:removed.Count 1 "Clean first-build asset ($mode)"
    Assert-Equal $script:removed[0] $asset 'Only the snapshot-new asset is selected'
}
foreach ($mode in @(' M Content/RegressionSentinel.uasset', 'error')) {
    $script:gitMode = $mode
    $before = @{}
    $script:removed = @()
    . $reportBlock
    Assert-Equal $script:removed.Count 0 "Preserve tracked/unknown asset ($mode)"
}
Write-Output 'Driver regressions passed (exit verdicts, preservation, first-build cleanup, Git failure).'
