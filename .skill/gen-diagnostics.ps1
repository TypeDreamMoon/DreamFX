<#
.SYNOPSIS
    Regenerates the machine-written half of Docs/diagnostics/.

.DESCRIPTION
    Scans the plugin sources for `Diagnostics.Error/Warning/Info(TEXT("DFXnnnn"), ...)` and writes one
    page per thousand-range, with a section per code holding its severity, message template and the
    place that raises it.

    Only the text between the `generated:begin`/`generated:end` markers is rewritten. Everything a
    human wrote under a code -- the Cause and Fix paragraphs that make the page worth reading -- is
    carried across untouched. That is the whole point of splitting it this way: the half that rots
    (which codes exist, what they say) is regenerated, and the half that does not (why it happens) is
    written once.

    Re-run it after adding a diagnostic. `-Check` makes it a gate instead: exit 1 if the pages are out
    of date, which is how CI notices a new code that nobody documented.

.EXAMPLE
    ./gen-diagnostics.ps1

.EXAMPLE
    ./gen-diagnostics.ps1 -Check
#>
[CmdletBinding()]
param(
    # Report drift and exit non-zero instead of writing.
    [switch]$Check
)

$ErrorActionPreference = 'Stop'

$pluginRoot = Split-Path -Parent $PSScriptRoot
$sourceRoot = Join-Path $pluginRoot 'Source'
$docsRoot = Join-Path $pluginRoot 'Docs/diagnostics'

# ---------------------------------------------------------------- scan

# String keys, not integers: an OrderedDictionary indexed with an int looks up by *position*, so
# `$ranges[3]` returned the fourth entry and every page was titled with its neighbour's subject.
$ranges = [ordered]@{
    '1' = 'Driver and file I/O'
    '2' = 'Lexer and syntax'
    '3' = 'Declarations and document structure'
    '4' = 'Values, types and expressions'
    '5' = 'Generation and asset writing'
    '6' = 'Niagara compilation'
    '7' = 'Provenance, drift and lint'
    '8' = 'Decompiler'
}

$found = @{}

foreach ($file in Get-ChildItem -LiteralPath $sourceRoot -Filter '*.cpp' -File -Recurse) {
    $text = [System.IO.File]::ReadAllText($file.FullName)

    # The message is whatever TEXT("...") comes first after the code -- either the message itself or
    # an FString::Printf format string. Both are the right thing to show.
    $pattern = 'Diagnostics\.(Error|Warning|Info)\(\s*TEXT\("(DFX\d{4})"\)(.{0,400}?)(?:TEXT\("((?:[^"\\]|\\.)*)"\)|;)'
    foreach ($match in [regex]::Matches($text, $pattern, 'Singleline')) {
        $code = $match.Groups[2].Value
        $severity = $match.Groups[1].Value.ToLowerInvariant()
        $message = $match.Groups[4].Value

        if (-not $message) { $message = '(built at runtime)' }

        # Line number of the match, for the "raised by" pointer.
        $line = ($text.Substring(0, $match.Index) -split "`n").Count
        $relative = [System.IO.Path]::GetRelativePath($pluginRoot, $file.FullName) -replace '\\', '/'

        if ($found.ContainsKey($code)) {
            # Same code raised from more than one place: keep them all, they are usually the same
            # condition reached by different routes and a reader chasing one wants to see the others.
            $found[$code].Sites += "$relative`:$line"
        }
        else {
            $found[$code] = [pscustomobject]@{
                Code     = $code
                Severity = $severity
                Message  = $message
                Sites    = @("$relative`:$line")
            }
        }
    }
}

if ($found.Count -eq 0) {
    throw "No diagnostics found under '$sourceRoot'. Is the layout still Source/**/*.cpp?"
}

Write-Host "Found $($found.Count) diagnostic code(s)." -ForegroundColor DarkGray

# ---------------------------------------------------------------- merge and emit

function Get-HandWrittenSections {
    param([string]$Path)

    # Maps code -> everything the human wrote under that code's heading, i.e. what follows the
    # generated:end marker up to the next heading.
    $sections = @{}
    if (-not (Test-Path -LiteralPath $Path)) { return $sections }

    $content = [System.IO.File]::ReadAllText($Path)
    $pattern = '<!-- generated:end (DFX\d{4}) -->\r?\n(.*?)(?=\r?\n## DFX|\Z)'
    foreach ($match in [regex]::Matches($content, $pattern, 'Singleline')) {
        # Trim both ends, not just the tail: the writer puts a blank line after the marker, so keeping
        # the leading newline would make each run add one more and the file would never converge --
        # which turns -Check into a permanent false alarm.
        $sections[$match.Groups[1].Value] = $match.Groups[2].Value.Trim()
    }
    return $sections
}

$placeholder = @'
**Cause.** _Not written yet._

**Fix.** _Not written yet._
'@

if (-not $Check) {
    New-Item -ItemType Directory -Force -Path $docsRoot | Out-Null
}

$drift = @()
$indexRows = @()

foreach ($rangeKey in $ranges.Keys) {
    $codes = $found.Values |
        Where-Object { $_.Code.Substring(3, 1) -eq "$rangeKey" } |
        Sort-Object Code

    if (-not $codes) { continue }

    $pagePath = Join-Path $docsRoot "DFX$($rangeKey)xxx.md"
    $handWritten = Get-HandWrittenSections -Path $pagePath

    $builder = [System.Text.StringBuilder]::new()
    [void]$builder.AppendLine("# DFX$($rangeKey)xxx --- $($ranges[$rangeKey])")
    [void]$builder.AppendLine()
    [void]$builder.AppendLine('> The block between the generated markers is written by `.skill/gen-diagnostics.ps1`.')
    [void]$builder.AppendLine('> Everything below a marker is written by hand and survives a regeneration.')
    [void]$builder.AppendLine()

    foreach ($entry in $codes) {
        $prose = if ($handWritten.ContainsKey($entry.Code)) { $handWritten[$entry.Code] } else { $placeholder }
        if (-not $prose.Trim()) { $prose = $placeholder }

        [void]$builder.AppendLine("## $($entry.Code)")
        [void]$builder.AppendLine()
        [void]$builder.AppendLine("<!-- generated:begin $($entry.Code) -->")
        [void]$builder.AppendLine("**Severity** $($entry.Severity)")
        [void]$builder.AppendLine()
        [void]$builder.AppendLine('**Message**')
        [void]$builder.AppendLine()
        [void]$builder.AppendLine('```')
        [void]$builder.AppendLine($entry.Message)
        [void]$builder.AppendLine('```')
        [void]$builder.AppendLine()
        $tick = [char]0x60
        $sites = ($entry.Sites | Sort-Object -Unique | ForEach-Object { "$tick$_$tick" }) -join ', '
        [void]$builder.AppendLine("**Raised by** $sites")
        [void]$builder.AppendLine("<!-- generated:end $($entry.Code) -->")
        [void]$builder.AppendLine()
        [void]$builder.AppendLine($prose)
        [void]$builder.AppendLine()

        $undocumented = $prose -match 'Not written yet'
        $indexRows += [pscustomobject]@{
            Code         = $entry.Code
            Severity     = $entry.Severity
            Page         = "DFX$($rangeKey)xxx.md"
            Message      = $entry.Message
            Undocumented = $undocumented
        }
    }

    $rendered = $builder.ToString()
    $existing = if (Test-Path -LiteralPath $pagePath) { [System.IO.File]::ReadAllText($pagePath) } else { '' }

    if ($rendered -ne $existing) {
        if ($Check) {
            $drift += "DFX$($rangeKey)xxx.md"
        }
        else {
            [System.IO.File]::WriteAllText($pagePath, $rendered)
            Write-Host "  wrote DFX$($rangeKey)xxx.md ($($codes.Count) code(s))" -ForegroundColor Gray
        }
    }
}

# ---------------------------------------------------------------- index

$index = [System.Text.StringBuilder]::new()
[void]$index.AppendLine('# DreamFX diagnostics')
[void]$index.AppendLine()
[void]$index.AppendLine('Every `DFXnnnn` DreamFX can emit. The leading digit is the stage that raises it:')
[void]$index.AppendLine()
foreach ($rangeKey in $ranges.Keys) {
    [void]$index.AppendLine("- **DFX$($rangeKey)xxx** --- $($ranges[$rangeKey]) --- [DFX$($rangeKey)xxx.md](DFX$($rangeKey)xxx.md)")
}
[void]$index.AppendLine()
[void]$index.AppendLine('Generated by `.skill/gen-diagnostics.ps1`; run it after adding a code.')
[void]$index.AppendLine()
[void]$index.AppendLine('| Code | Severity | Message |')
[void]$index.AppendLine('| --- | --- | --- |')
foreach ($row in $indexRows | Sort-Object Code) {
    $short = $row.Message
    if ($short.Length -gt 110) { $short = $short.Substring(0, 107) + '...' }
    $short = $short -replace '\|', '\|'
    [void]$index.AppendLine("| [$($row.Code)]($($row.Page)#$($row.Code.ToLowerInvariant())) | $($row.Severity) | $short |")
}
[void]$index.AppendLine()

$indexPath = Join-Path $docsRoot 'README.md'
$renderedIndex = $index.ToString()
$existingIndex = if (Test-Path -LiteralPath $indexPath) { [System.IO.File]::ReadAllText($indexPath) } else { '' }

if ($renderedIndex -ne $existingIndex) {
    if ($Check) { $drift += 'README.md' }
    else {
        [System.IO.File]::WriteAllText($indexPath, $renderedIndex)
        Write-Host '  wrote README.md' -ForegroundColor Gray
    }
}

# ---------------------------------------------------------------- verdict

$missing = @($indexRows | Where-Object { $_.Undocumented })
if ($missing.Count -gt 0) {
    Write-Host "$($missing.Count) code(s) still have no Cause/Fix prose:" -ForegroundColor Yellow
    Write-Host "  $(($missing.Code | Sort-Object) -join ', ')" -ForegroundColor DarkYellow
}

if ($Check) {
    if ($drift.Count -gt 0) {
        Write-Host ''
        Write-Host "diagnostics docs are out of date: $($drift -join ', ')" -ForegroundColor Red
        Write-Host 'Run .skill/gen-diagnostics.ps1 and commit the result.' -ForegroundColor Red
        exit 1
    }
    Write-Host 'diagnostics docs are up to date.' -ForegroundColor Green
}

exit 0
