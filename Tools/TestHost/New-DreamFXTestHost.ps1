#Requires -Version 7.0
<#
.SYNOPSIS
    Creates or refreshes the DreamFX test host: a minimal Unreal project whose Plugins\DreamFX is a git
    worktree of the DreamFX repository.

.DESCRIPTION
    DreamFX's gate (.skill\ci.ps1: lint, build, verify, corpus) writes .uasset files and boots an editor
    process on the project it runs in, so run inside the working project (DevTest) it needs that project's
    editor closed -- and an editor left open there rebuilds and saves the same packages from its source
    watcher. The host is a project of its own: its Plugins\DreamFX is a separate worktree with its own
    Binaries, Intermediate and generated Content, so it builds and runs while the working project's editor
    stays open.

    The script is idempotent. Run it again at any time and it only reports.

      1. Checks, before touching anything: git is on PATH, the host root is not on drive C, the
         repository is a git repository, the engine the template names exists.
      2. Plugins\DreamFX: if it exists it must be a worktree of -RepoPath (same git common directory,
         its own working tree rather than the repository's main one reached through a link) with
         -Branch checked out. If it does not exist it is created with "git worktree add". An existing
         worktree is never deleted, reset, or switched to another branch.
      3. Copies Template\ into the root. A file that is missing is written; a file whose content already
         matches is left alone; a file that differs is listed and kept, unless -Force is given, in which
         case it is backed up next to itself (<name>.bak-<timestamp>) and replaced.
      4. Writes <Root>\.dreamfx-testhost.json (template version, repository, branch, creation time),
         only when something in it changed.
      5. Prints the commands that build the host and run the gate in it.

    Nothing outside -Root is written, apart from what "git worktree add" itself records in the
    repository's .git\worktrees directory when it creates the worktree.

.PARAMETER Root
    The host project directory. Must not be on drive C.

.PARAMETER RepoPath
    Any working tree of the DreamFX repository; the new worktree is added to that repository.

.PARAMETER Branch
    The branch the worktree must have checked out (and is created with).

.PARAMETER EngineRoot
    The engine directory. By default it is looked up from the EngineAssociation in the template's
    .uproject (HKCU\Software\Epic Games\Unreal Engine\Builds), falling back to F:\UnrealEngine\UE_Moon.

.PARAMETER Force
    Replace template files that differ from the template, keeping a backup of each.

.PARAMETER KeepCurrentHead
    Accept whatever an existing worktree has checked out -- another branch, or a detached commit, as a
    revert experiment would leave it -- instead of requiring -Branch.

.PARAMETER Detach
    Follow -Branch with a detached HEAD instead of checking the branch out. A branch can be checked out
    in one worktree at a time, so when the branch is the one being developed in the repository's own
    working tree, the host cannot have it too; with -Detach the worktree is created with
    "git worktree add --detach" at the branch's tip, and on every later run a clean detached worktree is
    moved to the branch's current tip (a dirty one is left where it is, with a warning). The host then
    tests what has been committed to the branch, nothing more. This is the one case in which the script
    moves a worktree's HEAD, and only a detached, clean one.

.PARAMETER AllowSystemDrive
    Accept a host root on drive C. The refusal exists for a machine whose drive C is nearly full; a
    machine that has only drive C, with room on it, passes this (or sets DREAMFX_ALLOW_DRIVE_C=1).

.EXAMPLE
    pwsh -NoProfile -File .\New-DreamFXTestHost.ps1 -Detach -WhatIf

.EXAMPLE
    pwsh -NoProfile -File .\New-DreamFXTestHost.ps1 -Detach
    The usual case: main is checked out in DevTest\Plugins\DreamFX, so the host follows it detached.

.NOTES
    Exit codes:
      0  the host matches the template and the worktree passed every check;
      1  a check failed; the failure is printed, and nothing after the failing step was done;
      2  the host is usable but does not match the template: files differ and were kept (no -Force),
         or -WhatIf found something it would have written.
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [string] $Root = 'I:\UnrealProject_Moon\DEV_58\DreamFXTestHost',
    [string] $RepoPath = 'I:\UnrealProject_Moon\DEV_58\DevTest\Plugins\DreamFX',
    [string] $Branch = 'main',
    [string] $EngineRoot = '',
    [switch] $Force,
    [switch] $KeepCurrentHead,
    [switch] $Detach,
    [switch] $AllowSystemDrive
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
# git's exit code is checked by hand after every call (Invoke-Git); it must not become an exception.
$PSNativeCommandUseErrorActionPreference = $false

# Bump whenever anything under Template\ changes, so a host records which template it was made from.
$TemplateVersion = 1
$ProjectFileName = 'DreamFXTestHost.uproject'
$MetadataFileName = '.dreamfx-testhost.json'
$TemplateEngineIniRelative = 'Config\DefaultEngine.ini'
$FallbackEngineRoot = 'F:\UnrealEngine\UE_Moon'
# Below this the first build (plugin and project intermediates, logs) is at risk of running out of room.
$MinimumFreeGigabytes = 15

# ----------------------------------------------------------------------------------------------- helpers

function Write-Step([string] $Message) {
    Write-Host ''
    Write-Host "== $Message"
}

function Write-Info([string] $Message) {
    Write-Host "   $Message"
}

function Stop-WithFailure([string[]] $Messages) {
    foreach ($Message in $Messages) {
        [Console]::Error.WriteLine("ERROR: $Message")
    }
    exit 1
}

function Resolve-UserPath([string] $Path) {
    # Relative to the PowerShell location the caller is in, not to the process's current directory,
    # which is what [IO.Path]::GetFullPath would use.
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function ConvertTo-NormalPath([string] $Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }
    $Separator = [System.IO.Path]::DirectorySeparatorChar
    $Full = [System.IO.Path]::GetFullPath($Path.Trim().Replace('/', [string]$Separator))
    $Trimmed = $Full.TrimEnd($Separator)
    # "C:" alone would mean "the current directory on C"; keep the root's separator.
    if ($Trimmed.Length -eq 2 -and $Trimmed[1] -eq ':') {
        return $Trimmed + $Separator
    }
    return $Trimmed
}

function Test-SamePath([string] $A, [string] $B) {
    return [string]::Equals((ConvertTo-NormalPath $A), (ConvertTo-NormalPath $B), [System.StringComparison]::OrdinalIgnoreCase)
}

function Test-PathUnder([string] $Path, [string] $Directory) {
    $NormalPath = ConvertTo-NormalPath $Path
    $NormalDirectory = (ConvertTo-NormalPath $Directory).TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    return $NormalPath.StartsWith($NormalDirectory, [System.StringComparison]::OrdinalIgnoreCase)
}

function Resolve-PhysicalPath([string] $Path) {
    # Follows every symbolic link and junction along the path, so the drive that comes back is the one
    # the bytes are really written to. A part that does not exist yet is appended as it is: it will be
    # created under whatever its existing parent really is.
    $Separator = [System.IO.Path]::DirectorySeparatorChar
    $Full = ConvertTo-NormalPath $Path
    $PathRoot = [System.IO.Path]::GetPathRoot($Full)
    $Current = $PathRoot
    $Parts = @($Full.Substring($PathRoot.Length).Split($Separator, [System.StringSplitOptions]::RemoveEmptyEntries))
    for ($Index = 0; $Index -lt $Parts.Count; $Index++) {
        $Candidate = Join-Path $Current $Parts[$Index]
        $Item = Get-Item -LiteralPath $Candidate -Force -ErrorAction SilentlyContinue
        if ($null -eq $Item) {
            return ConvertTo-NormalPath (Join-Path $Current (($Parts[$Index..($Parts.Count - 1)]) -join $Separator))
        }
        $Hops = 0
        while ($null -ne $Item -and $Item.LinkType -and $Hops -lt 16) {
            $Target = @($Item.Target)[0]
            if ([string]::IsNullOrWhiteSpace($Target)) {
                break
            }
            if (-not [System.IO.Path]::IsPathRooted($Target)) {
                $Target = Join-Path (Split-Path -Parent $Candidate) $Target
            }
            $Candidate = ConvertTo-NormalPath $Target
            $Item = Get-Item -LiteralPath $Candidate -Force -ErrorAction SilentlyContinue
            $Hops++
        }
        $Current = $Candidate
    }
    return ConvertTo-NormalPath $Current
}

function Get-DriveName([string] $Path) {
    return ([System.IO.Path]::GetPathRoot((ConvertTo-NormalPath $Path))).TrimEnd([System.IO.Path]::DirectorySeparatorChar).ToUpperInvariant()
}

function Invoke-Git {
    param(
        [Parameter(Mandatory = $true)] [string] $WorkingDirectory,
        [Parameter(Mandatory = $true)] [string[]] $Arguments,
        [switch] $AllowFailure
    )
    # stderr is folded into the result instead of being allowed to become a terminating error, and the
    # output is decoded as UTF-8 whatever code page the console is on.
    $PreviousPreference = $ErrorActionPreference
    $PreviousEncoding = $null
    try {
        $ErrorActionPreference = 'Continue'
        try {
            $PreviousEncoding = [Console]::OutputEncoding
            [Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
        }
        catch {
            $PreviousEncoding = $null
        }
        $Output = & git -C $WorkingDirectory @Arguments 2>&1
        $ExitCode = $LASTEXITCODE
    }
    finally {
        if ($null -ne $PreviousEncoding) {
            try { [Console]::OutputEncoding = $PreviousEncoding } catch { }
        }
        $ErrorActionPreference = $PreviousPreference
    }
    # A command that prints nothing leaves $Output null, and @($null) is one null element, not none.
    $Text = (@($Output) | Where-Object { $null -ne $_ } | ForEach-Object { $_.ToString() }) -join "`n"
    if ($ExitCode -ne 0 -and -not $AllowFailure) {
        throw "git $($Arguments -join ' ') (in $WorkingDirectory) failed with exit code $ExitCode.`n$Text"
    }
    return [pscustomobject]@{ ExitCode = $ExitCode; Output = $Text.Trim() }
}

function Get-WorktreeEntries([string] $Repository) {
    # "git worktree list --porcelain": one block per worktree, blocks separated by an empty line.
    $Entries = [System.Collections.Generic.List[object]]::new()
    $Listing = (Invoke-Git -WorkingDirectory $Repository -Arguments @('worktree', 'list', '--porcelain')).Output
    foreach ($Block in ($Listing -split "`n\s*`n")) {
        $Entry = [ordered]@{ Path = ''; Branch = ''; Head = ''; Prunable = $false }
        foreach ($Line in ($Block -split "`n")) {
            $Line = $Line.Trim()
            if ($Line.StartsWith('worktree ')) { $Entry.Path = ConvertTo-NormalPath $Line.Substring(9) }
            elseif ($Line.StartsWith('branch ')) { $Entry.Branch = $Line.Substring(7) }
            elseif ($Line.StartsWith('HEAD ')) { $Entry.Head = $Line.Substring(5) }
            elseif ($Line.StartsWith('prunable')) { $Entry.Prunable = $true }
        }
        if ($Entry.Path) {
            $Entries.Add([pscustomobject]$Entry)
        }
    }
    return $Entries.ToArray()
}

function Test-IsBinaryTemplateFile([string] $Path) {
    # Content the engine saves. Compared byte for byte: decoded as text, two different packages can read
    # the same once their invalid sequences have all become the same replacement character.
    return @('.uasset', '.umap', '.uexp', '.ubulk') -contains [System.IO.Path]::GetExtension($Path).ToLowerInvariant()
}

function ConvertTo-ComparableText([byte[]] $Bytes) {
    # Line endings and a byte-order mark are not differences: git may check the template out with
    # either ending, and an editor may add or drop the mark.
    $Text = [System.Text.Encoding]::UTF8.GetString($Bytes).TrimStart([char]0xFEFF)
    return $Text.Replace("`r`n", "`n")
}

function Get-FirstDifference([string] $Existing, [string] $Wanted) {
    $ExistingLines = $Existing -split "`n"
    $WantedLines = $Wanted -split "`n"
    $Count = [Math]::Max($ExistingLines.Count, $WantedLines.Count)
    for ($Index = 0; $Index -lt $Count; $Index++) {
        $Left = if ($Index -lt $ExistingLines.Count) { $ExistingLines[$Index] } else { '<end of file>' }
        $Right = if ($Index -lt $WantedLines.Count) { $WantedLines[$Index] } else { '<end of file>' }
        if ($Left -cne $Right) {
            $Shorten = { param([string] $S) if ($S.Length -gt 100) { $S.Substring(0, 100) + '...' } else { $S } }
            return "line $($Index + 1): on disk '$(& $Shorten $Left)', template '$(& $Shorten $Right)'"
        }
    }
    return 'no line differs'
}

function Get-ZenDataDirectory {
    # The same order ZenServerInterface.cpp (DetermineDataPath) resolves it in, minus the command line
    # and the editor's own setting, which a script cannot see. Only used to warn, never to configure.
    $Candidates = @(
        @{ Name = 'environment variable UE-ZenSubprocessDataPath'; Value = [Environment]::GetEnvironmentVariable('UE-ZenSubprocessDataPath') }
    )
    $RegistryValue = $null
    try {
        $RegistryValue = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Epic Games\Zen' -Name 'DataPath' -ErrorAction Stop).DataPath
    }
    catch {
        $RegistryValue = $null
    }
    $Candidates += @{ Name = 'registry HKCU\Software\Epic Games\Zen DataPath'; Value = $RegistryValue }
    $Candidates += @{ Name = 'environment variable UE-ZenDataPath'; Value = [Environment]::GetEnvironmentVariable('UE-ZenDataPath') }
    $LocalCache = [Environment]::GetEnvironmentVariable('UE-LocalDataCachePath')
    if (-not [string]::IsNullOrWhiteSpace($LocalCache) -and $LocalCache -ne 'None') {
        $Candidates += @{ Name = 'environment variable UE-LocalDataCachePath (+ \Zen)'; Value = (Join-Path $LocalCache 'Zen') }
    }
    foreach ($Candidate in $Candidates) {
        if (-not [string]::IsNullOrWhiteSpace($Candidate.Value)) {
            return [pscustomobject]@{ Path = $Candidate.Value; Source = $Candidate.Name }
        }
    }
    $Default = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'UnrealEngine\Common\Zen\Data'
    return [pscustomobject]@{ Path = $Default; Source = 'the engine default ([Zen.AutoLaunch] DataPath)' }
}

# ------------------------------------------------------------------------------------------ preflight

Write-Step 'Checking before changing anything'

$Failures = [System.Collections.Generic.List[string]]::new()
$PendingChanges = 0

$TemplateDirectory = Join-Path $PSScriptRoot 'Template'
$TemplateProject = Join-Path $TemplateDirectory $ProjectFileName
$TemplateEngineIni = Join-Path $TemplateDirectory $TemplateEngineIniRelative

$Root = ConvertTo-NormalPath (Resolve-UserPath $Root)
$RepoPath = ConvertTo-NormalPath (Resolve-UserPath $RepoPath)
$WorktreePath = Join-Path $Root 'Plugins\DreamFX'
Write-Info "Host root:  $Root"
Write-Info "Repository: $RepoPath"
Write-Info "Branch:     $Branch$(if ($KeepCurrentHead) { ' (or whatever an existing worktree has checked out)' })"

$GitAvailable = $null -ne (Get-Command git -CommandType Application -ErrorAction SilentlyContinue)
if (-not $GitAvailable) {
    $Failures.Add('git is not on PATH.')
}

if (-not (Test-Path -LiteralPath $TemplateProject -PathType Leaf) -or -not (Test-Path -LiteralPath $TemplateEngineIni -PathType Leaf)) {
    $Failures.Add("The template is incomplete or missing: expected $TemplateProject and $TemplateEngineIni.")
}

# Drive C has a few gigabytes left; the first build alone writes more than that.
$RootDrive = Get-DriveName $Root
$RootPhysical = Resolve-PhysicalPath $Root
$RootPhysicalDrive = Get-DriveName $RootPhysical
if (-not $AllowSystemDrive -and $env:DREAMFX_ALLOW_DRIVE_C -eq '1') { $AllowSystemDrive = [switch]$true }
if (($RootDrive -eq 'C:' -or $RootPhysicalDrive -eq 'C:') -and -not $AllowSystemDrive) {
    $Failures.Add("The host root must not be on drive C ($Root$(if ($RootPhysical -ne $Root) { " -> $RootPhysical" })). Pass -Root on another drive, or -AllowSystemDrive where drive C has room.")
}
if ((Test-SamePath $Root $RepoPath) -or (Test-PathUnder $Root $RepoPath)) {
    $Failures.Add("The host root ($Root) must not be the repository or inside it ($RepoPath).")
}
# A mistyped -Root that lands on a real project would have the template's files created inside it.
if (Test-Path -LiteralPath $Root -PathType Container) {
    $OtherProjects = @(Get-ChildItem -LiteralPath $Root -Filter '*.uproject' -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne $ProjectFileName })
    if ($OtherProjects.Count -gt 0) {
        $Failures.Add("$Root already holds another project ($($OtherProjects[0].Name)); the host needs a directory of its own.")
    }
}

$RepoCommonDirectory = ''
$RepoTopLevel = ''
if (-not (Test-Path -LiteralPath $RepoPath -PathType Container)) {
    $Failures.Add("The repository path does not exist: $RepoPath")
}
elseif ($GitAvailable) {
    $Common = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir') -AllowFailure
    $Top = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--show-toplevel') -AllowFailure
    if ($Common.ExitCode -ne 0 -or $Top.ExitCode -ne 0) {
        $Failures.Add("$RepoPath is not a git working tree: $($Common.Output)")
    }
    else {
        $RepoCommonDirectory = ConvertTo-NormalPath $Common.Output
        $RepoTopLevel = ConvertTo-NormalPath $Top.Output
        Write-Info "Repository git directory: $RepoCommonDirectory"
    }
}

$EngineAssociation = ''
if (Test-Path -LiteralPath $TemplateProject -PathType Leaf) {
    try {
        $EngineAssociation = [string](Get-Content -LiteralPath $TemplateProject -Raw | ConvertFrom-Json).EngineAssociation
    }
    catch {
        $Failures.Add("The template's $ProjectFileName is not valid JSON: $($_.Exception.Message)")
    }
}
if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
    if ($EngineAssociation) {
        try {
            $Registered = (Get-ItemProperty -LiteralPath 'HKCU:\Software\Epic Games\Unreal Engine\Builds' -Name $EngineAssociation -ErrorAction Stop).$EngineAssociation
            if (-not [string]::IsNullOrWhiteSpace($Registered)) {
                $EngineRoot = $Registered
                Write-Info "Engine $EngineAssociation is registered at $Registered."
            }
        }
        catch {
            Write-Info "Engine $EngineAssociation is not registered for this user; using $FallbackEngineRoot."
        }
    }
    if ([string]::IsNullOrWhiteSpace($EngineRoot)) {
        $EngineRoot = $FallbackEngineRoot
    }
}
$EngineRoot = ConvertTo-NormalPath (Resolve-UserPath $EngineRoot)
$BuildBatch = Join-Path $EngineRoot 'Engine\Build\BatchFiles\Build.bat'
$EditorCommand = Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
if (-not (Test-Path -LiteralPath $BuildBatch -PathType Leaf)) {
    $Failures.Add("No engine at $EngineRoot (expected $BuildBatch). Pass -EngineRoot.")
}
elseif (-not (Test-Path -LiteralPath $EditorCommand -PathType Leaf)) {
    Write-Warning "The engine at $EngineRoot has no $EditorCommand yet; the host can be created, but nothing can run in it until the engine is built."
}
else {
    Write-Info "Engine: $EngineRoot"
}

if ($Failures.Count -gt 0) {
    Stop-WithFailure $Failures.ToArray()
}

# Warnings only: neither of these is the script's to change.
try {
    $Drive = [System.IO.DriveInfo]::new((Get-DriveName $RootPhysical))
    $FreeGigabytes = [Math]::Round($Drive.AvailableFreeSpace / 1GB, 1)
    Write-Info "Free space on $($Drive.Name): $FreeGigabytes GB"
    if ($FreeGigabytes -lt $MinimumFreeGigabytes) {
        Write-Warning "Only $FreeGigabytes GB free on $($Drive.Name); the first build of the host needs several."
    }
}
catch {
    Write-Info "Could not read the free space on $RootPhysicalDrive."
}

# The derived data cache. The engine's local Zen store is machine-wide and shared with every other
# project on this engine; the host deliberately does not configure it (see Template\Config\DefaultEngine.ini).
# What is worth knowing is where its bytes really land, because the default directory is under the user
# profile on drive C and only a link keeps it off that drive.
$Zen = Get-ZenDataDirectory
$ZenPhysical = Resolve-PhysicalPath $Zen.Path
Write-Info "Derived data cache (local Zen store): $($Zen.Path)$(if (-not (Test-SamePath $ZenPhysical $Zen.Path)) { " -> $ZenPhysical" }), from $($Zen.Source)"
if ((Get-DriveName $ZenPhysical) -eq 'C:') {
    Write-Warning "The shared derived data cache is physically on drive C ($ZenPhysical). The host does not change that: it is the same cache every project on this engine uses. Move it with a directory link or the UE-ZenDataPath environment variable if drive C fills up."
}

# ------------------------------------------------------------------------------------------ worktree

Write-Step "Plugins\DreamFX as a worktree of the repository"

$WorktreeEntries = @(Get-WorktreeEntries $RepoPath)
$ResolvedBranch = $Branch

if (Test-Path -LiteralPath $WorktreePath) {
    $Common = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--path-format=absolute', '--git-common-dir') -AllowFailure
    if ($Common.ExitCode -ne 0) {
        Stop-WithFailure @("$WorktreePath exists but is not a git working tree. It is left as it is; move it away to let this script create the worktree.")
    }
    $Top = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--show-toplevel')).Output
    if (-not (Test-SamePath $Top $WorktreePath)) {
        Stop-WithFailure @("$WorktreePath is a folder inside the working tree $Top, not a worktree of its own.")
    }
    if (-not (Test-SamePath $Common.Output $RepoCommonDirectory)) {
        Stop-WithFailure @("$WorktreePath belongs to another repository (git directory $($Common.Output), expected $RepoCommonDirectory).")
    }
    # A link to the repository's own working tree would pass both checks above and would put every build
    # of the host into the very working tree the host exists to stay out of.
    if (Test-SamePath (Resolve-PhysicalPath $WorktreePath) (Resolve-PhysicalPath $RepoTopLevel)) {
        Stop-WithFailure @("$WorktreePath is the repository's own working tree ($RepoTopLevel), reached through a link, not a separate worktree.")
    }
    $Listed = @($WorktreeEntries | Where-Object { Test-SamePath $_.Path $WorktreePath })
    if ($Listed.Count -eq 0) {
        Stop-WithFailure @("$WorktreePath is not in the repository's worktree list (git -C `"$RepoPath`" worktree list).")
    }

    $SymbolicHead = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('symbolic-ref', '--quiet', '--short', 'HEAD') -AllowFailure
    $CurrentBranch = if ($SymbolicHead.ExitCode -eq 0) { $SymbolicHead.Output } else { '' }
    $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
    $Described = if ($CurrentBranch) { "branch $CurrentBranch at $ShortHead" } else { "a detached HEAD at $ShortHead" }
    # -Detach: the worktree follows the branch with a detached HEAD. A clean one is moved to the
    # branch's current tip; a dirty one is left alone, because moving it could lose work.
    if ($Detach -and -not $CurrentBranch) {
        $Tip = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('rev-parse', '--verify', '--quiet', "refs/heads/$Branch") -AllowFailure
        $HeadCommit = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', 'HEAD')).Output
        $DirtyNow = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('status', '--porcelain')).Output
        if ($Tip.ExitCode -ne 0) {
            Stop-WithFailure @("-Detach: branch $Branch does not exist in the repository.")
        }
        elseif ($Tip.Output -ne $HeadCommit) {
            if ($DirtyNow) {
                Write-Warning "$WorktreePath is detached at $ShortHead with uncommitted changes; not moved to the tip of $Branch."
            }
            elseif ($PSCmdlet.ShouldProcess($WorktreePath, "git checkout --detach $Branch")) {
                $Moved = Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('checkout', '--detach', $Branch) -AllowFailure
                if ($Moved.ExitCode -ne 0) {
                    Stop-WithFailure @("git checkout --detach $Branch failed in ${WorktreePath}:", $Moved.Output)
                }
                $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
                $Described = "a detached HEAD at $ShortHead"
                Write-Info "Moved the detached worktree to the tip of $Branch ($ShortHead)."
            }
        }
        else {
            Write-Info "The detached worktree is at the tip of $Branch ($ShortHead)."
        }
    }
    elseif ($CurrentBranch -cne $Branch) {
        # Branch names are case-sensitive to git even where the file system is not.
        if (-not $KeepCurrentHead) {
            Stop-WithFailure @(
                "$WorktreePath has $Described, not branch $Branch.",
                'This script never switches, resets or removes a worktree. Check out the branch there yourself (git -C "<worktree>" switch <branch>), or pass -Branch with the one it has, or -KeepCurrentHead to accept it as it is.'
            )
        }
        Write-Warning "$WorktreePath has $Described, not branch $Branch; kept as it is (-KeepCurrentHead)."
    }
    $ResolvedBranch = if ($CurrentBranch) { $CurrentBranch } elseif ($Detach) { "$Branch (detached)" } else { "(detached at $ShortHead)" }
    Write-Info "Existing worktree, $Described."

    $Dirty = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('status', '--porcelain')).Output
    if ($Dirty) {
        $DirtyCount = @($Dirty -split "`n").Count
        Write-Info "It has $DirtyCount uncommitted change(s); the host builds the worktree exactly as it is, uncommitted changes included."
    }
}
else {
    $Missing = @($WorktreeEntries | Where-Object { Test-SamePath $_.Path $WorktreePath })
    if ($Missing.Count -gt 0) {
        Stop-WithFailure @(
            "$WorktreePath does not exist but is still registered as a worktree of the repository.",
            "Clear the stale registration yourself (git -C `"$RepoPath`" worktree prune) and run this again."
        )
    }
    $Holder = @($WorktreeEntries | Where-Object { $_.Branch -ceq "refs/heads/$Branch" })
    if ($Holder.Count -gt 0 -and -not $Detach) {
        Stop-WithFailure @(
            "Branch $Branch is already checked out in $($Holder[0].Path)$(if ($Holder[0].Prunable) { ' (a worktree whose folder is gone)' }).",
            'A branch can be checked out in one worktree at a time. Pass another -Branch, or free that one first.'
        )
    }
    $LocalBranch = Invoke-Git -WorkingDirectory $RepoPath -Arguments @('show-ref', '--verify', '--quiet', "refs/heads/$Branch") -AllowFailure
    if ($LocalBranch.ExitCode -ne 0) {
        $RemoteRefs = (Invoke-Git -WorkingDirectory $RepoPath -Arguments @('for-each-ref', '--format=%(refname)', 'refs/remotes')).Output -split "`n"
        $Tracking = @($RemoteRefs | Where-Object { $_ -match "^refs/remotes/[^/]+/$([regex]::Escape($Branch))$" })
        if ($Tracking.Count -ne 1) {
            Stop-WithFailure @("Branch $Branch exists neither locally nor on exactly one remote (found $($Tracking.Count) remote copies).")
        }
        Write-Info "Branch $Branch exists only as $($Tracking[0]); git will create a local branch that tracks it."
    }

    $PendingChanges++
    $AddArguments = if ($Detach) { @('worktree', 'add', '--detach', $WorktreePath, $Branch) } else { @('worktree', 'add', $WorktreePath, $Branch) }
    if ($PSCmdlet.ShouldProcess($WorktreePath, "git $($AddArguments -join ' ')")) {
        $PluginsDirectory = Split-Path -Parent $WorktreePath
        if (-not (Test-Path -LiteralPath $PluginsDirectory)) {
            New-Item -ItemType Directory -Path $PluginsDirectory -Force | Out-Null
        }
        $Added = Invoke-Git -WorkingDirectory $RepoPath -Arguments $AddArguments -AllowFailure
        if ($Added.ExitCode -ne 0) {
            Stop-WithFailure @("git worktree add failed:", $Added.Output)
        }
        $PendingChanges--
        $ShortHead = (Invoke-Git -WorkingDirectory $WorktreePath -Arguments @('rev-parse', '--short', 'HEAD')).Output
        if ($Detach) {
            $ResolvedBranch = "$Branch (detached)"
            Write-Info "Created the worktree, detached at the tip of $Branch ($ShortHead)."
        }
        else {
            Write-Info "Created the worktree, branch $Branch at $ShortHead."
        }
    }
}

# ------------------------------------------------------------------------------------------ template

Write-Step 'Project files from the template'

$Stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$Created = [System.Collections.Generic.List[string]]::new()
$Unchanged = [System.Collections.Generic.List[string]]::new()
$Replaced = [System.Collections.Generic.List[string]]::new()
$Kept = [System.Collections.Generic.List[string]]::new()

$TemplateFiles = @(Get-ChildItem -LiteralPath $TemplateDirectory -Recurse -File | Sort-Object FullName)
foreach ($TemplateFile in $TemplateFiles) {
    $Relative = [System.IO.Path]::GetRelativePath($TemplateDirectory, $TemplateFile.FullName)
    $Destination = Join-Path $Root $Relative
    $Wanted = [System.IO.File]::ReadAllBytes($TemplateFile.FullName)

    if (-not (Test-Path -LiteralPath $Destination -PathType Leaf)) {
        $PendingChanges++
        if ($PSCmdlet.ShouldProcess($Destination, 'Create from the template')) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $Destination) -Force | Out-Null
            [System.IO.File]::WriteAllBytes($Destination, $Wanted)
            $PendingChanges--
        }
        $Created.Add($Relative)
        continue
    }

    $ExistingBytes = [System.IO.File]::ReadAllBytes($Destination)
    if (Test-IsBinaryTemplateFile $Relative) {
        if ([System.Linq.Enumerable]::SequenceEqual($ExistingBytes, $Wanted)) {
            $Unchanged.Add($Relative)
            continue
        }
        $Difference = "the bytes differ ($($ExistingBytes.Length) on disk, $($Wanted.Length) in the template)"
    }
    else {
        $ExistingText = ConvertTo-ComparableText $ExistingBytes
        $WantedText = ConvertTo-ComparableText $Wanted
        if ($ExistingText -ceq $WantedText) {
            $Unchanged.Add($Relative)
            continue
        }
        $Difference = Get-FirstDifference $ExistingText $WantedText
    }
    if ($Force) {
        $PendingChanges++
        if ($PSCmdlet.ShouldProcess($Destination, "Replace (first difference at $Difference); the old file is kept as $([System.IO.Path]::GetFileName($Destination)).bak-$Stamp")) {
            Copy-Item -LiteralPath $Destination -Destination "$Destination.bak-$Stamp" -Force
            [System.IO.File]::WriteAllBytes($Destination, $Wanted)
            $PendingChanges--
        }
        $Replaced.Add("$Relative ($Difference)")
    }
    else {
        $PendingChanges++
        $Kept.Add("$Relative ($Difference)")
    }
}

$Verb = if ($WhatIfPreference) { 'would be ' } else { '' }
foreach ($Name in $Created) { Write-Info "${Verb}created:   $Name" }
foreach ($Name in $Replaced) { Write-Info "${Verb}replaced:  $Name" }
foreach ($Name in $Unchanged) { Write-Info "unchanged: $Name" }
foreach ($Name in $Kept) { Write-Info "DIFFERS, kept as it is: $Name" }
if ($Kept.Count -gt 0) {
    Write-Warning "$($Kept.Count) file(s) differ from the template and were left alone. Run again with -Force to replace them (each old file is backed up next to itself)."
}

# ------------------------------------------------------------------------------------------ metadata

Write-Step "Host metadata ($MetadataFileName)"

$MetadataPath = Join-Path $Root $MetadataFileName
$Now = (Get-Date).ToString('o')
$Metadata = [ordered]@{
    schema            = 1
    templateVersion   = $TemplateVersion
    repoPath          = $RepoPath
    gitCommonDir      = $RepoCommonDirectory
    worktreePath      = $WorktreePath
    branch            = $ResolvedBranch
    engineAssociation = $EngineAssociation
    engineRoot        = $EngineRoot
    createdBy         = 'Plugins/DreamFX/Tools/TestHost/New-DreamFXTestHost.ps1'
    createdAt         = $Now
    updatedAt         = $Now
}
$StableKeys = @('schema', 'templateVersion', 'repoPath', 'gitCommonDir', 'worktreePath', 'branch', 'engineAssociation', 'engineRoot', 'createdBy')
$NeedsWrite = $true
if (Test-Path -LiteralPath $MetadataPath -PathType Leaf) {
    try {
        $Previous = Get-Content -LiteralPath $MetadataPath -Raw | ConvertFrom-Json
        $Same = $true
        foreach ($Key in $StableKeys) {
            $Property = $Previous.PSObject.Properties[$Key]
            if ($null -eq $Property -or [string]$Property.Value -cne [string]$Metadata[$Key]) {
                $Same = $false
            }
        }
        $PreviousCreated = $Previous.PSObject.Properties['createdAt']
        if ($null -ne $PreviousCreated -and $PreviousCreated.Value) {
            # ConvertFrom-Json turns an ISO timestamp into a DateTime; write it back in the same form.
            $Value = $PreviousCreated.Value
            $Metadata['createdAt'] = if ($Value -is [datetime]) { $Value.ToString('o') } else { [string]$Value }
        }
        $NeedsWrite = -not $Same
    }
    catch {
        Write-Warning "$MetadataPath is not valid JSON; it will be rewritten."
    }
}
if ($NeedsWrite) {
    $PendingChanges++
    if ($PSCmdlet.ShouldProcess($MetadataPath, 'Write the host metadata')) {
        New-Item -ItemType Directory -Path $Root -Force | Out-Null
        $Json = $Metadata | ConvertTo-Json -Depth 3
        [System.IO.File]::WriteAllText($MetadataPath, $Json + "`n", [System.Text.UTF8Encoding]::new($false))
        $PendingChanges--
        Write-Info "Written: template version $TemplateVersion, branch $ResolvedBranch."
    }
}
else {
    Write-Info 'Unchanged.'
}

# ------------------------------------------------------------------------------------------ next step

Write-Step 'Next'

$Gate = Join-Path $WorktreePath '.skill\ci.ps1'
$HostProject = Join-Path $Root $ProjectFileName
Write-Info 'Build the host:'
Write-Info "  & `"$BuildBatch`" DreamFXTestHostEditor Win64 Development `"-Project=$HostProject`" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges"
Write-Info 'Then run the gate in it (lint, build, verify, corpus):'
Write-Info "  pwsh -NoProfile -File `"$Gate`" -Project `"$HostProject`""
Write-Info 'The first build compiles the plugin and the host module only; engine modules are reused as they are.'

if ($PendingChanges -gt 0) {
    exit 2
}
exit 0
