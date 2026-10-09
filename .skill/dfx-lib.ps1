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
