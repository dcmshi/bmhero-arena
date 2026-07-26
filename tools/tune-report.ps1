# Feel-metrics sweep driver. Builds tune_report once per tuning variant and
# prints a side-by-side comparison table.
#
#   tools\tune-report.ps1
#   tools\tune-report.ps1 -Compare base,friction=0.020,friction=0.030,friction=0.045
#   tools\tune-report.ps1 -Compare base,turn=1456 -Csv sweep.csv
#   tools\tune-report.ps1 -SelfTest
#
# Variant syntax is <knob>=<value>; "base" means default tuning. Multiple knobs
# in one variant are separated by '+' (e.g. friction=0.020+turn=1456). Values are
# wrapped in Q(...) automatically except for raw-integer knobs. This script owns
# the -D quoting so callers never fight PowerShell escaping.
param(
    [string[]] $Compare = @("base"),
    [string]   $Cc      = "",
    [string]   $Csv     = "",
    [switch]   $SelfTest
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "tune-report"
New-Item -ItemType Directory -Force -Path $work | Out-Null

# gcc is not on the default Windows PATH here - the project's C toolchain is
# MSYS2 UCRT64 (README section Windows). Find it rather than making the caller
# set PATH up first.
#
# GOTCHA: MSYS2's gcc.exe FAILS SILENTLY (exit 1, no diagnostic) when invoked by
# absolute path without its own bin dir on PATH - it can't load libisl/libmpc/etc.
# So we must prepend the directory, not just return the exe path.
function Resolve-Cc([string]$explicit) {
    $found = $null
    if ($explicit)            { $found = $explicit }
    elseif ($env:BMHERO_CC)   { $found = $env:BMHERO_CC }
    else {
        $onPath = Get-Command gcc -ErrorAction SilentlyContinue
        if ($onPath) { return $onPath.Source }   # already on PATH: DLLs resolve
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

# short name -> macro. Raw-integer knobs are NOT wrapped in Q().
$KNOBS = [ordered]@{
    friction = @{ macro = "TUNE_RUN_FRICTION"; raw = $false }
    accel    = @{ macro = "TUNE_RUN_ACCEL";    raw = $false }
    top      = @{ macro = "TUNE_RUN_SPEED";    raw = $false }
    air      = @{ macro = "TUNE_AIR_CONTROL";  raw = $false }
    gravity  = @{ macro = "TUNE_GRAVITY";      raw = $false }
    jump     = @{ macro = "TUNE_JUMP_IMPULSE"; raw = $false }
    turn     = @{ macro = "TUNE_TURN_RATE";    raw = $true  }
}

function Build-Variant([string]$variant) {
    $defs = @()
    if ($variant -ne "base") {
        foreach ($part in $variant.Split("+")) {
            $kv = $part.Split("=", 2)
            if ($kv.Count -ne 2) { throw "bad variant '$part' (expected knob=value)" }
            $k = $kv[0].Trim(); $v = $kv[1].Trim()
            if (-not $KNOBS.Contains($k)) {
                throw "unknown knob '$k'. Valid: $(($KNOBS.Keys) -join ', ')"
            }
            $macro = $KNOBS[$k].macro
            $val   = if ($KNOBS[$k].raw) { $v } else { "Q($v)" }
            $defs += "-D$macro=$val"
        }
    }
    $exe = Join-Path $work ("tr_" + ($variant -replace '[^A-Za-z0-9]', '_') + ".exe")
    $ccArgs = @("-std=c11","-Wall","-Wextra","-Werror","-O2") + $defs + @(
        "-o", $exe,
        (Join-Path $root "src\arena\arena_sim.c"),
        (Join-Path $root "tools\tune_probes.c"),
        (Join-Path $root "tools\tune_report.c"))
    & $CC @ccArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed for variant '$variant'" }
    return $exe
}

function Get-Metrics([string]$exe) {
    $m = [ordered]@{}
    foreach ($line in (& $exe)) {
        if ($line -match '^#') { continue }
        $c = $line -split "`t"
        if ($c.Count -ge 2) { $m[$c[0]] = $c[1] }
    }
    return $m
}

if ($SelfTest) {
    # Cross-tune monotonicity: compile-time constants can't vary inside one
    # binary, so this property is only checkable at the script level. This is
    # the assertion tests/test_tune_report.c structurally cannot make.
    $lo = Get-Metrics (Build-Variant "friction=0.015")
    $hi = Get-Metrics (Build-Variant "friction=0.060")
    $ok = [double]$lo["stop_distance"] -gt [double]$hi["stop_distance"]
    Write-Host ("SELFTEST stop_distance: friction 0.015 -> {0}u, 0.060 -> {1}u : {2}" -f `
        $lo["stop_distance"], $hi["stop_distance"], $(if ($ok) { "PASS" } else { "FAIL" }))

    $g1 = Get-Metrics (Build-Variant "gravity=0.0175")
    $g2 = Get-Metrics (Build-Variant "gravity=0.0350")
    $ok2 = [double]$g1["jump_apex"] -gt [double]$g2["jump_apex"]
    Write-Host ("SELFTEST jump_apex: gravity 0.0175 -> {0}u, 0.0350 -> {1}u : {2}" -f `
        $g1["jump_apex"], $g2["jump_apex"], $(if ($ok2) { "PASS" } else { "FAIL" }))

    $t1 = Get-Metrics (Build-Variant "turn=728")
    $t2 = Get-Metrics (Build-Variant "turn=1456")
    $ok3 = [int]$t1["turn180_ticks"] -gt [int]$t2["turn180_ticks"]
    Write-Host ("SELFTEST turn180_ticks: turn 728 -> {0}, 1456 -> {1} : {2}" -f `
        $t1["turn180_ticks"], $t2["turn180_ticks"], $(if ($ok3) { "PASS" } else { "FAIL" }))

    if ($ok -and $ok2 -and $ok3) { Write-Host "`nSELFTEST PASS"; exit 0 }
    Write-Host "`nSELFTEST FAIL"; exit 1
}

$cols = [ordered]@{}
foreach ($v in $Compare) { $cols[$v] = Get-Metrics (Build-Variant $v) }

$metrics = @($cols[$Compare[0]].Keys)
$w = 18
# widest variant name + 2, so long names like "friction=0.020" keep a gutter
$cw = (($Compare | ForEach-Object { $_.Length }) | Measure-Object -Maximum).Maximum + 2
if ($cw -lt 10) { $cw = 10 }
$hdr = "metric".PadRight($w) + (($Compare | ForEach-Object { $_.PadLeft($cw) }) -join "")
Write-Host ""
Write-Host $hdr
Write-Host ("-" * $hdr.Length)
foreach ($k in $metrics) {
    $row = $k.PadRight($w)
    foreach ($v in $Compare) { $row += ($cols[$v][$k]).PadLeft($cw) }
    Write-Host $row
}

if ($Csv) {
    $out = @(("metric," + ($Compare -join ",")))
    foreach ($k in $metrics) {
        $out += (@($k) + @($Compare | ForEach-Object { $cols[$_][$k] })) -join ","
    }
    $out | Set-Content -Path $Csv -Encoding UTF8
    Write-Host "`nwrote $Csv"
}
