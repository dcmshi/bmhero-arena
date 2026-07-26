# Re-pin the scripted-match hash and the feel-metrics baseline after an
# INTENTIONAL gameplay change. Shows old -> new for both before writing.
#
#   tools\repin.ps1            # normal use
#   tools\repin.ps1 -Force     # tooling-only hash correction (no gameplay change)
#
# Refuses to run when the hash moved but TUNE_VERSION did not - that is exactly
# the invariant-#4 violation the pin exists to catch. Bump TUNE_VERSION first.
param([string]$Cc = "", [switch]$Force)
$ErrorActionPreference = "Stop"
$root     = Split-Path $PSScriptRoot -Parent
$pinFile  = Join-Path $root "tools\pinned_hash.txt"
$baseFile = Join-Path $root "tools\tune_metrics.baseline"
$work     = Join-Path ([IO.Path]::GetTempPath()) "repin"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# Same MSYS2 gotcha as tune-report.ps1: gcc invoked by absolute path without its
# own bin dir on PATH fails silently (exit 1, no diagnostic).
function Resolve-Cc([string]$explicit) {
    $found = $null
    if ($explicit)          { $found = $explicit }
    elseif ($env:BMHERO_CC) { $found = $env:BMHERO_CC }
    else {
        $onPath = Get-Command gcc -ErrorAction SilentlyContinue
        if ($onPath) { return $onPath.Source }
        foreach ($c in @("C:\msys64\ucrt64\bin\gcc.exe", "C:\msys64\mingw64\bin\gcc.exe")) {
            if (Test-Path $c) { $found = $c; break }
        }
    }
    if (-not $found) {
        throw "no C compiler found. Put gcc on PATH, or pass -Cc <path>, or set `$env:BMHERO_CC."
    }
    $bin = Split-Path $found -Parent
    if ($env:PATH -notlike "*$bin*") { $env:PATH = "$bin;" + $env:PATH }
    return $found
}
$CC = Resolve-Cc $Cc

$pin = (Get-Content $pinFile -Raw).Trim() -split '\s+'
$oldVer = $pin[0]; $oldHash = $pin[1]

$tuning = Get-Content (Join-Path $root "src\arena\arena_tuning.h") -Raw
if ($tuning -notmatch '(?m)^\s*#define\s+TUNE_VERSION\s+(\d+)') {
    throw "could not read TUNE_VERSION from src/arena/arena_tuning.h"
}
$newVer = $Matches[1]

$hashExe = Join-Path $work "arena_hash.exe"
& $CC -std=c11 -Wall -Wextra -Werror -O2 -o $hashExe `
    (Join-Path $root "tools\arena_hash.c") (Join-Path $root "src\arena\arena_sim.c")
if ($LASTEXITCODE -ne 0) { throw "hash generator build failed" }
$newHash = (& $hashExe).Trim()

$reportExe = Join-Path $work "tune_report.exe"
& $CC -std=c11 -Wall -Wextra -Werror -O2 -o $reportExe `
    (Join-Path $root "src\arena\arena_sim.c") `
    (Join-Path $root "tools\tune_probes.c") `
    (Join-Path $root "tools\tune_report.c")
if ($LASTEXITCODE -ne 0) { throw "tune_report build failed" }
$newMetrics = @(& $reportExe | ForEach-Object { $_ -replace "`r", "" })

$old  = @(Get-Content $baseFile)
$diff = Compare-Object $old $newMetrics
$hashChanged    = ($newHash -ne $oldHash)
$metricsChanged = [bool]$diff

Write-Host "TUNE_VERSION : $oldVer -> $newVer"
Write-Host "hash         : $oldHash -> $newHash"
Write-Host ("metrics      : {0}" -f $(if ($metricsChanged) { "CHANGED" } else { "unchanged" }))

Write-Host "`n--- feel-metrics diff ---"
if ($diff) {
    foreach ($d in $diff) {
        $tag = if ($d.SideIndicator -eq "=>") { "new" } else { "old" }
        Write-Host ("  {0}  {1}" -f $tag, ($d.InputObject -replace "`t", "  "))
    }
} else { Write-Host "  (no metric changed)" }

if (-not $hashChanged -and -not $metricsChanged -and -not $Force) {
    Write-Host "`nNothing to repin - hash and metrics both match the pins."
    exit 0
}

# Metrics can move while the hash does NOT: the scripted match doesn't exercise
# every constant (e.g. terminal velocity). That is still a gameplay change and
# still needs a version bump - so the rule keys off EITHER signal, not just the hash.
if (($hashChanged -or $metricsChanged) -and $newVer -eq $oldVer -and -not $Force) {
    $what = if ($hashChanged) { "sim hash" } else { "feel metrics (hash unchanged - the scripted match doesn't cover this constant)" }
    Write-Host ""
    Write-Host "REFUSING TO REPIN: the $what changed but TUNE_VERSION is still $oldVer." -ForegroundColor Red
    Write-Host ""
    Write-Host "A gameplay change must bump TUNE_VERSION - it is folded into the netcode"
    Write-Host "version handshake, and peers on different tuning must not be able to match."
    Write-Host "Bump TUNE_VERSION in src/arena/arena_tuning.h, then re-run this script."
    Write-Host ""
    Write-Host "(-Force overrides, for a tooling-only correction with no gameplay change.)"
    exit 1
}

# Explicit LF for both: .gitattributes checks these out as LF on every platform
# and CI diffs the baseline literally, so writing CRLF here would only create a
# spurious working-tree difference for git to normalise away.
[IO.File]::WriteAllText($pinFile,  "$newVer $newHash`n")
[IO.File]::WriteAllText($baseFile, (($newMetrics -join "`n") + "`n"))

Write-Host "`nRepinned. Commit tools/pinned_hash.txt and tools/tune_metrics.baseline."
