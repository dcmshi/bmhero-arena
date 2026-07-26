# Find the FIRST tick where two sim builds diverge, and show which fields moved.
#
# The hash pin tells you THAT something changed. This tells you WHERE and WHAT.
#
#   tools\trace-diff.ps1 -A base -B friction=0.020     # tuning variant vs default
#   tools\trace-diff.ps1 -A base -B base -OptA -O0 -OptB -O3   # determinism hunt
#   tools\trace-diff.ps1 -A base -B turn=1456 -Player 0 -Ticks 600
#
# Variant syntax matches tune-report.ps1: <knob>=<value>, '+'-separated,
# or "base" for defaults. A determinism bug shows up as A and B being the SAME
# variant at different optimisation levels yet still diverging - that is a real
# bug (invariant #1/#2), not a tuning difference.
param(
    [string] $A       = "base",
    [string] $B       = "base",
    [string] $OptA    = "-O2",
    [string] $OptB    = "-O2",
    [int]    $Ticks   = 5400,
    [int]    $Player  = -1,
    [string] $Cc      = "",
    [int]    $Context = 2
)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$work = Join-Path ([IO.Path]::GetTempPath()) "trace-diff"
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
$CC = Resolve-Cc $Cc

$KNOBS = [ordered]@{
    friction = @{ macro = "TUNE_RUN_FRICTION"; raw = $false }
    accel    = @{ macro = "TUNE_RUN_ACCEL";    raw = $false }
    top      = @{ macro = "TUNE_RUN_SPEED";    raw = $false }
    air      = @{ macro = "TUNE_AIR_CONTROL";  raw = $false }
    gravity  = @{ macro = "TUNE_GRAVITY";      raw = $false }
    jump     = @{ macro = "TUNE_JUMP_IMPULSE"; raw = $false }
    turn     = @{ macro = "TUNE_TURN_RATE";    raw = $true  }
}

function Build-Trace([string]$variant, [string]$opt, [string]$tag) {
    $defs = @()
    if ($variant -ne "base") {
        foreach ($part in $variant.Split("+")) {
            $kv = $part.Split("=", 2)
            if ($kv.Count -ne 2) { throw "bad variant '$part' (expected knob=value)" }
            $k = $kv[0].Trim(); $v = $kv[1].Trim()
            if (-not $KNOBS.Contains($k)) {
                throw "unknown knob '$k'. Valid: $(($KNOBS.Keys) -join ', ')"
            }
            $val = if ($KNOBS[$k].raw) { $v } else { "Q($v)" }
            $defs += "-D$($KNOBS[$k].macro)=$val"
        }
    }
    $exe = Join-Path $work "trace_$tag.exe"
    $ccArgs = @("-std=c11","-Wall","-Wextra","-Werror",$opt) + $defs + @(
        "-o", $exe,
        (Join-Path $root "tools\arena_trace.c"),
        (Join-Path $root "src\arena\arena_sim.c"))
    & $CC @ccArgs
    if ($LASTEXITCODE -ne 0) { throw "build failed for '$variant' at $opt" }

    $runArgs = @("--ticks", "$Ticks")
    if ($Player -ge 0) { $runArgs += @("--player", "$Player") }
    return @(& $exe @runArgs)
}

Write-Host "A: $A ($OptA)"
Write-Host "B: $B ($OptB)"
Write-Host "ticks: $Ticks$(if ($Player -ge 0) { ", player $Player" } else { ", all players" })`n"

$rowsA = Build-Trace $A $OptA "a"
$rowsB = Build-Trace $B $OptB "b"

if ($rowsA.Count -ne $rowsB.Count) {
    Write-Host "traces differ in length: A=$($rowsA.Count) B=$($rowsB.Count)" -ForegroundColor Red
    exit 1
}

$header = $rowsA[0] -split ","
$firstDiff = -1
for ($i = 1; $i -lt $rowsA.Count; $i++) {
    if ($rowsA[$i] -ne $rowsB[$i]) { $firstDiff = $i; break }
}

if ($firstDiff -lt 0) {
    if ($A -eq $B -and $OptA -eq $OptB) {
        Write-Host "IDENTICAL (expected - same variant, same flags)" -ForegroundColor Green
    } else {
        Write-Host "IDENTICAL - these two builds produce a bit-identical trace." -ForegroundColor Green
    }
    exit 0
}

$fa = $rowsA[$firstDiff] -split ","
$fb = $rowsB[$firstDiff] -split ","
Write-Host "FIRST DIVERGENCE at tick $($fa[0])" -ForegroundColor Yellow
Write-Host ""
Write-Host ("  {0,-12} {1,14} {2,14}" -f "field", "A", "B")
Write-Host ("  " + ("-" * 42))
for ($c = 0; $c -lt $header.Count; $c++) {
    if ($fa[$c] -ne $fb[$c]) {
        Write-Host ("  {0,-12} {1,14} {2,14}" -f $header[$c], $fa[$c], $fb[$c]) -ForegroundColor Yellow
    }
}

# surrounding ticks, so you can see the run-up to the divergence
$lo = [Math]::Max(1, $firstDiff - $Context)
$hi = [Math]::Min($rowsA.Count - 1, $firstDiff + $Context)
Write-Host "`n  context (A / B), ticks $($lo)..$($hi):"
for ($i = $lo; $i -le $hi; $i++) {
    $mark = if ($i -eq $firstDiff) { ">>" } else { "  " }
    $ha = ($rowsA[$i] -split ",")[1]; $hb = ($rowsB[$i] -split ",")[1]
    Write-Host ("  {0} tick {1,-6} hashA={2} hashB={3}{4}" -f `
        $mark, ($rowsA[$i] -split ",")[0], $ha, $hb, $(if ($ha -ne $hb) { "  <-- differs" } else { "" }))
}

$sameBuild = ($A -eq $B)
if ($sameBuild -and $OptA -ne $OptB) {
    Write-Host "`nSAME tuning at different -O levels diverged. That is a DETERMINISM BUG" -ForegroundColor Red
    Write-Host "(invariant #1/#2), not a tuning difference - investigate, do not repin."
}
exit 1
