# One-command local gate. Runs everything CI runs, with CI's exact flags, so a
# green run here means the push will be green too.
#
#   tools\gate.ps1              # the full gate
#   tools\gate.ps1 -Opt -O0     # same, at a different optimisation level
#
# Exits nonzero on the first failure. Run this before every commit that touches
# src/arena/ or tools/.
param([string]$Cc = "", [string]$Opt = "-O2")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "arena-gate"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# MSYS2 gcc fails silently when invoked by absolute path without its own bin dir
# on PATH (it can't load libisl/libmpc) - so prepend, don't just resolve.
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
    if (-not $found) { throw "no C compiler found. Pass -Cc <path> or set `$env:BMHERO_CC." }
    $bin = Split-Path $found -Parent
    if ($env:PATH -notlike "*$bin*") { $env:PATH = "$bin;" + $env:PATH }
    return $found
}
$CC   = Resolve-Cc $Cc
$SIM  = Join-Path $root "src\arena\arena_sim.c"
$FLAGS = @("-std=c11","-Wall","-Wextra","-Werror",$Opt)
$fails = 0

function Run-Suite([string]$name, [string[]]$sources) {
    $exe = Join-Path $work "$name.exe"
    & $CC @FLAGS -o $exe @sources
    if ($LASTEXITCODE -ne 0) { Write-Host "[$name] BUILD FAILED" -ForegroundColor Red; $script:fails++; return }
    $out = & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$name] FAILED" -ForegroundColor Red
        $out | ForEach-Object { Write-Host "    $_" }
        $script:fails++
    } else {
        Write-Host ("[{0}] {1}" -f $name, ($out | Select-Object -Last 1)) -ForegroundColor Green
    }
}

Write-Host "gate: $CC $($FLAGS -join ' ')`n"

Run-Suite "determinism"     @($SIM, (Join-Path $root "tests\test_determinism.c"))
Run-Suite "movement"        @($SIM, (Join-Path $root "tests\test_movement.c"))
Run-Suite "bomb_mechanics"  @($SIM, (Join-Path $root "tests\test_bomb_mechanics.c"))
Run-Suite "tune_probes"     @($SIM, (Join-Path $root "tools\tune_probes.c"),
                               (Join-Path $root "tests\test_tune_report.c"))

# --- pinned hash + TUNE_VERSION (mirrors the CI logic) ---
$pin = (Get-Content (Join-Path $root "tools\pinned_hash.txt") -Raw).Trim() -split '\s+'
$pinVer = $pin[0]; $pinHash = $pin[1]
$tuning = Get-Content (Join-Path $root "src\arena\arena_tuning.h") -Raw
$null = $tuning -match '(?m)^\s*#define\s+TUNE_VERSION\s+(\d+)'
$curVer = $Matches[1]
$hashExe = Join-Path $work "arena_hash.exe"
& $CC @FLAGS -o $hashExe (Join-Path $root "tools\arena_hash.c") $SIM
$h = (& $hashExe).Trim()
if ($h -eq $pinHash) {
    Write-Host "[hash] $h matches pin (TUNE_VERSION $curVer)" -ForegroundColor Green
} elseif ($curVer -eq $pinVer) {
    Write-Host "[hash] INVARIANT #4 VIOLATED: $pinHash -> $h with TUNE_VERSION still $curVer" -ForegroundColor Red
    Write-Host "       Bump TUNE_VERSION, then run tools\repin.ps1."
    $fails++
} else {
    Write-Host "[hash] changed $pinHash -> $h (TUNE_VERSION $pinVer -> $curVer) - run tools\repin.ps1" -ForegroundColor Yellow
    $fails++
}

# --- feel-metrics baseline ---
$reportExe = Join-Path $work "tune_report.exe"
& $CC @FLAGS -o $reportExe $SIM (Join-Path $root "tools\tune_probes.c") (Join-Path $root "tools\tune_report.c")
$now  = @(& $reportExe | ForEach-Object { $_ -replace "`r", "" })
$base = @(Get-Content (Join-Path $root "tools\tune_metrics.baseline"))
$d = Compare-Object $base $now
if ($d) {
    Write-Host "[metrics] BASELINE DRIFT" -ForegroundColor Red
    foreach ($x in $d) {
        $tag = if ($x.SideIndicator -eq "=>") { "new" } else { "old" }
        Write-Host ("    {0}  {1}" -f $tag, ($x.InputObject -replace "`t", "  "))
    }
    Write-Host "       If intentional: bump TUNE_VERSION and run tools\repin.ps1."
    $fails++
} else {
    Write-Host "[metrics] baseline matches" -ForegroundColor Green
}

Write-Host ""
if ($fails -eq 0) { Write-Host "GATE GREEN" -ForegroundColor Green; exit 0 }
Write-Host "GATE FAILED ($fails)" -ForegroundColor Red; exit $fails
