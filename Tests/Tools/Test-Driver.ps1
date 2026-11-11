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
# Launcher-installed engines: the manifest names the engine by AppName UE_<version>, and the plugin
# artifacts installed beside it share its InstallLocation, so only the whole name may match.
$launcherRoot = Join-Path ([IO.Path]::GetTempPath()) ('dfx-launcher-' + [guid]::NewGuid().ToString('N'))
$engineDir = New-Item -ItemType Directory -Path (Join-Path $launcherRoot 'UE_5.8') -Force
$otherDir = New-Item -ItemType Directory -Path (Join-Path $launcherRoot 'Other') -Force
$manifestPath = Join-Path $launcherRoot 'LauncherInstalled.dat'
try {
    @{ InstallationList = @(
        @{ AppName = 'FabPlugin_5.9'; InstallLocation = $otherDir.FullName },
        @{ AppName = 'UE_5.8'; InstallLocation = $engineDir.FullName },
        @{ AppName = 'UE_5.80'; InstallLocation = $otherDir.FullName },
        @{ AppName = 'UE_5.7'; InstallLocation = (Join-Path $launcherRoot 'gone') }
    ) } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

    Assert-Equal (Get-DreamFXLauncherEngineRoot -Association '5.8' -ManifestPath $manifestPath) $engineDir.FullName 'Launcher engine is found by its UE_ name'
    Assert-Equal ([string](Get-DreamFXLauncherEngineRoot -Association '5.9' -ManifestPath $manifestPath)) '' 'A plugin artifact is not an engine'
    Assert-Equal ([string](Get-DreamFXLauncherEngineRoot -Association '5.7' -ManifestPath $manifestPath)) '' 'An engine whose directory is gone is not returned'
    Assert-Equal ([string](Get-DreamFXLauncherEngineRoot -Association '5.6' -ManifestPath $manifestPath)) '' 'An engine the launcher does not list is not found'
    Assert-Equal ([string](Get-DreamFXLauncherEngineRoot -Association '5.8' -ManifestPath (Join-Path $launcherRoot 'missing.dat'))) '' 'A missing manifest is not an error'
    Set-Content -LiteralPath $manifestPath -Value 'not json' -Encoding UTF8
    Assert-Equal ([string](Get-DreamFXLauncherEngineRoot -Association '5.8' -ManifestPath $manifestPath)) '' 'A corrupt manifest is not an error'
}
finally {
    Remove-Item -LiteralPath $launcherRoot -Recurse -Force -ErrorAction SilentlyContinue
}

# `dfx.ps1 info`: engine version and kind come from files the engine ships, a field that cannot be
# read is `unknown`, and the output never carries a path. These run before the git mock below.
$engineFixture = Join-Path ([IO.Path]::GetTempPath()) ('dfx-engine-' + [guid]::NewGuid().ToString('N'))
try {
    $buildDir = New-Item -ItemType Directory -Path (Join-Path $engineFixture 'Engine/Build') -Force
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Version 'unknown' 'No Build.version is unknown'
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Kind 'unknown' 'Neither marker is unknown'
    Set-Content -LiteralPath (Join-Path $buildDir 'Build.version') -Value '{ "MajorVersion": 5, "MinorVersion": 8, "PatchVersion": 1, "Changelist": 0 }' -Encoding UTF8
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Version '5.8.1' 'Version is read from Build.version'
    $null = New-Item -ItemType Directory -Path (Join-Path $engineFixture 'Engine/Source/Runtime') -Force
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Kind 'source' 'Engine sources without InstalledBuild.txt are a source build'
    Set-Content -LiteralPath (Join-Path $buildDir 'InstalledBuild.txt') -Value 'x' -Encoding UTF8
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Kind 'launcher' 'InstalledBuild.txt marks an installed engine'
    Set-Content -LiteralPath (Join-Path $buildDir 'Build.version') -Value 'not json' -Encoding UTF8
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot $engineFixture).Version 'unknown' 'A corrupt Build.version is not an error'
    Assert-Equal (Get-DreamFXEngineInfo -EngineRoot (Join-Path $engineFixture 'gone')).Version 'unknown' 'A missing engine is not an error'
    Assert-Equal (Get-DreamFXCommit -RepoRoot $engineFixture) 'unknown' 'A directory outside any repository has no commit'
}
finally {
    Remove-Item -LiteralPath $engineFixture -Recurse -Force -ErrorAction SilentlyContinue
}
# A plugin unpacked inside some other repository must not report that repository's commit.
Assert-Equal (Get-DreamFXCommit -RepoRoot (Join-Path $repo 'Tests')) 'unknown' 'A subdirectory of a repository has no commit of its own'
$commit = Get-DreamFXCommit -RepoRoot $repo
if ($commit -notmatch '^([0-9a-f]{7,} \(.+\)( dirty)?|unknown)$') { throw "Commit field : unexpected '$commit'" }
$lines = @(Format-DreamFXEnvironment -Fields ([ordered]@{ 'plugin.version' = '1.0.0'; 'os' = '' }))
Assert-Equal ($lines -join '|') 'DreamFX environment|plugin.version : 1.0.0|os             : unknown' 'Keys share one column and an empty value is unknown'

$info = @(& pwsh -NoProfile -File (Join-Path $repo '.skill/dfx.ps1') info -Engine (Join-Path $repo 'no-such-engine') 2>&1 | ForEach-Object { "$_" })
Assert-Equal $LASTEXITCODE 0 'info exits 0 when the engine cannot be found'
Assert-Equal $info[0] 'DreamFX environment' 'info title'
$keys = @($info | Select-Object -Skip 1 | ForEach-Object { ($_ -split ' : ', 2)[0].Trim() })
Assert-Equal ($keys -join ',') 'plugin.version,plugin.commit,engine.version,engine.kind,os,locale,shell' 'info keys and their order'
Assert-Equal ($info | Where-Object { $_ -match 'engine\.version' }) 'engine.version : unknown' 'An unresolved engine is unknown'
if (@($info | Where-Object { $_ -match '[A-Za-z]:[\\/]|(^|\s)/[^\s]+/|Users[\\/]' }).Count -gt 0) {
    throw "info printed a path: $($info -join ' | ')"
}

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
    try { $null = Get-AssetSnapshot -Root $repo -RequireComplete }
    catch { $failed = $_.Exception.Message -match 'Simulated incomplete asset scan' }
    Assert-Equal $failed $true 'An incomplete snapshot must abort before cleanup'

    # Without -CleanNew the snapshot only feeds a report; an unreadable directory must not fail the build.
    $failed = $false
    try { $null = Get-AssetSnapshot -Root $repo }
    catch { $failed = $true }
    Assert-Equal $failed $false 'A report-only snapshot tolerates an incomplete scan'
}
$report = @($driver.EndBlock.Statements | Where-Object {
    $_ -is [System.Management.Automation.Language.IfStatementAst] -and
    $_.Clauses[0].Item1.Extent.Text -eq '$trackAssets'
})
Assert-Equal $report.Count 1 'Locate the production asset report'
$reportBlock = [scriptblock]::Create($report[0].Extent.Text)
function Get-AssetSnapshot { param($Root, [switch]$RequireComplete) return $script:after }
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
Write-Output 'Driver regressions passed (exit verdicts, preservation, first-build cleanup, Git failure, info).'
