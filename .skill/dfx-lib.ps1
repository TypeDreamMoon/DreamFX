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

# ---------------------------------------------------------------- info
#
# `dfx.ps1 info` prints what a bug report needs. Everything is read from files and git; nothing is
# launched, and no absolute path is ever returned, because the output is meant to be pasted into a
# public issue.

function Get-DreamFXEngineInfo {
    # Version from Engine/Build/Build.version; kind from InstalledBuild.txt, which only installed
    # (launcher) engines ship. A tree with engine sources and no such file is a source build.
    param([string]$EngineRoot)

    $version = 'unknown'
    $kind = 'unknown'
    if ($EngineRoot -and (Test-Path -LiteralPath $EngineRoot)) {
        $buildVersion = Join-Path $EngineRoot 'Engine/Build/Build.version'
        if (Test-Path -LiteralPath $buildVersion) {
            try {
                $build = Get-Content -LiteralPath $buildVersion -Raw -Encoding UTF8 | ConvertFrom-Json
                if ($null -ne $build.MajorVersion -and $null -ne $build.MinorVersion) {
                    $version = '{0}.{1}.{2}' -f [int]$build.MajorVersion, [int]$build.MinorVersion, [int]$build.PatchVersion
                }
            }
            catch {
                $version = 'unknown'
            }
        }
        if (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine/Build/InstalledBuild.txt')) { $kind = 'launcher' }
        elseif (Test-Path -LiteralPath (Join-Path $EngineRoot 'Engine/Source/Runtime')) { $kind = 'source' }
    }
    [pscustomobject]@{ Version = $version; Kind = $kind }
}

function Get-DreamFXCommit {
    # "<short hash> (<branch>)[ dirty]" for the repository whose top level is $RepoRoot, else unknown.
    # A plugin unpacked from a zip into a project that is itself a repository sits below that
    # repository's top level; reporting the project's commit there would be wrong, so it is unknown.
    param([string]$RepoRoot)

    if (-not $RepoRoot -or -not (Get-Command git -ErrorAction SilentlyContinue)) { return 'unknown' }
    try {
        $prefix = & git -C $RepoRoot rev-parse --show-prefix 2>$null
        if ($LASTEXITCODE -ne 0 -or $prefix) { return 'unknown' }
        $hash = & git -C $RepoRoot rev-parse --short HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $hash) { return 'unknown' }
        $branch = & git -C $RepoRoot rev-parse --abbrev-ref HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $branch) { $branch = 'unknown' }
        elseif ($branch -eq 'HEAD') { $branch = 'detached' }
        $dirty = & git -C $RepoRoot status --porcelain --untracked-files=no 2>$null
        $suffix = if ($LASTEXITCODE -eq 0 -and $dirty) { ' dirty' } else { '' }
        return "$hash ($branch)$suffix"
    }
    catch {
        return 'unknown'
    }
}

function Get-DreamFXOsName {
    try {
        if ($IsWindows -or $env:OS -eq 'Windows_NT') {
            $current = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction Stop
            $build = [int]$current.CurrentBuild
            # ProductName still says "Windows 10" on Windows 11; the build number is what tells them apart.
            $name = if ($build -ge 22000) { 'Windows 11' } else { 'Windows 10' }
            $text = "$name $($current.EditionID) 10.0.$build"
            if ($null -ne $current.UBR) { $text += ".$($current.UBR)" }
            if ($current.DisplayVersion) { $text += " ($($current.DisplayVersion))" }
            return $text
        }
        $description = [System.Runtime.InteropServices.RuntimeInformation]::OSDescription
        if ($description) { return $description.Trim() }
    }
    catch {
    }
    return 'unknown'
}

function Format-DreamFXEnvironment {
    # One "key : value" line per field, keys padded to one column, in the order given.
    param([System.Collections.Specialized.OrderedDictionary]$Fields, [string]$Title = 'DreamFX environment')

    $width = ($Fields.Keys | ForEach-Object { $_.Length } | Measure-Object -Maximum).Maximum
    $lines = @($Title)
    foreach ($key in $Fields.Keys) {
        $value = if ([string]::IsNullOrWhiteSpace([string]$Fields[$key])) { 'unknown' } else { [string]$Fields[$key] }
        $lines += ('{0} : {1}' -f $key.PadRight($width), $value)
    }
    return $lines
}