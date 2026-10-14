$ErrorActionPreference = 'Stop'
# Shared driver decisions. The caller owns process launch and file deletion.
function Get-DreamFXExitCode {
    param([int]$ProcessExit, [string[]]$LogLines)

    $reported = @($LogLines | Select-String -Pattern 'Commandlet DreamFXCommandlet_\d+ finished execution \(result (-?\d+)\)')
    if ($reported.Count -eq 0) { return $ProcessExit }
    $result = [int]$reported[-1].Matches[0].Groups[1].Value
    if ($result -ne 0 -or $ProcessExit -eq 0) { return $result }

    # The known exit-3 anomaly is only accepted after a clean shutdown. Crashes after
    # commandlet completion must still fail, even when the commandlet itself returned zero.
    $cleanShutdown = @($LogLines | Where-Object { $_ -match 'LogExit: Exiting\.' }).Count -gt 0
    $fatal = @($LogLines | Where-Object { $_ -match 'Fatal error:|Unhandled Exception:|Assertion failed:' }).Count -gt 0
    if ($ProcessExit -eq 3 -and $cleanShutdown -and -not $fatal) { return 0 }
    return $ProcessExit
}

function Get-DreamFXAssetChanges {
    param([hashtable]$Before, [hashtable]$After)

    foreach ($path in ($After.Keys | Sort-Object)) {
        $existed = $Before.ContainsKey($path)
        if (-not $existed -or $Before[$path] -ne $After[$path]) {
            [pscustomobject]@{ Path = $path; Existed = $existed }
        }
    }
}

function Get-DreamFXLauncherEngineRoot {
    # The Epic Games Launcher records every engine it installs in LauncherInstalled.dat, but writes
    # the HKLM "Unreal Engine\<version>" key only on some installs: an engine moved to another drive,
    # or installed by a newer launcher, has the manifest entry and no key, so a project with
    # EngineAssociation "5.8" resolves nowhere in the registry. The engine itself is the entry whose
    # AppName is exactly UE_<association>; the same InstallLocation also appears under plugin
    # artifacts (FabPlugin_5.8, QuixelBridge_5.8), which is why the name is matched whole.
    param(
        [string]$Association,
        [string]$ManifestPath = (Join-Path $env:ProgramData 'Epic/UnrealEngineLauncher/LauncherInstalled.dat')
    )

    if (-not $Association -or -not $ManifestPath -or -not (Test-Path -LiteralPath $ManifestPath)) { return $null }
    try {
        $manifest = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
    }
    catch {
        return $null
    }

    foreach ($entry in @($manifest.InstallationList)) {
        if ($entry.AppName -ceq "UE_$Association" -and $entry.InstallLocation -and (Test-Path -LiteralPath $entry.InstallLocation)) {
            return (Resolve-Path -LiteralPath $entry.InstallLocation).Path
        }
    }
    return $null
}