# Unit translation between the four number systems this project mixes. Every
# RE/tuning session converts these by hand and gets one wrong eventually.
#
#   tools\conv.ps1 -Q 618              # Q20.12 raw -> everything
#   tools\conv.ps1 -Unit 0.151         # sim world units -> everything
#   tools\conv.ps1 -Hero 18            # Hero game units -> everything
#   tools\conv.ps1 -Deg 4              # degrees -> binary angle
#   tools\conv.ps1 -Bam 728            # binary angle -> degrees
#   tools\conv.ps1 -Ticks 45           # ticks -> seconds
#   tools\conv.ps1 -Sec 0.75           # seconds -> ticks
#   tools\conv.ps1 -Table              # the constants + a reference table
#
# THE SYSTEMS
#   Q20.12   raw fixed-point int in ArenaState (Q_ONE = 4096). src/arena only.
#   unit     sim world units (Q / 4096). "1.0q ~= 1 world unit".
#   Hero     the game's own units. g_scale = 120 Hero units per sim unit
#            (BMHeroRecomp src/arena_bridge/arena_bridge.cpp:23).
#   game u/f the decomp's per-frame speeds. Height anchor S = 1/119 converts
#            game u/frame -> sim unit/tick (docs/bmhero-player-movement-re.md).
#   BAM      binary angle, 65536 = 360 deg (uint16 yaw).
param(
    [double] $Q     = [double]::NaN,
    [double] $Unit  = [double]::NaN,
    [double] $Hero  = [double]::NaN,
    [double] $GameU = [double]::NaN,
    [double] $Deg   = [double]::NaN,
    [double] $Bam   = [double]::NaN,
    [double] $Ticks = [double]::NaN,
    [double] $Sec   = [double]::NaN,
    [switch] $Table
)
$Q_ONE  = 4096.0
$SCALE  = 120.0      # Hero units per sim unit (arena_bridge.cpp g_scale)
$S      = 1.0/119.0  # height anchor: game u/frame -> sim unit/tick
$HZ     = 60.0

function Show-Length([double]$unit, [string]$src) {
    $q = $unit * $Q_ONE
    Write-Host "from: $src"
    Write-Host ("  Q20.12 raw     {0,12:N0}   (exact: {1:N3})" -f [Math]::Round($q), $q)
    Write-Host ("  sim units      {0,12:N5}" -f $unit)
    Write-Host ("  Hero units     {0,12:N3}   (x{1})" -f ($unit * $SCALE), $SCALE)
    Write-Host ("  game u/frame   {0,12:N4}   (/ S, S=1/119)" -f ($unit / $S))
    Write-Host ("  as speed       {0,12:N3} unit/s   ({1:N1} Hero/s)" -f ($unit * $HZ), ($unit * $SCALE * $HZ))
    Write-Host ("  Q() literal    Q({0:N6})" -f $unit)
}

function Show-Angle([double]$deg, [string]$src) {
    $bam = $deg * 65536.0 / 360.0
    Write-Host "from: $src"
    Write-Host ("  degrees        {0,12:N4}" -f $deg)
    Write-Host ("  BAM (uint16)   {0,12:N0}   (0x{1:X4})" -f [Math]::Round($bam), [int][Math]::Round($bam))
    Write-Host ("  per-second     {0,12:N2} deg/s   (at 60Hz, if this is per-tick)" -f ($deg * $HZ))
    if ($deg -ne 0) {
        Write-Host ("  180 deg takes  {0,12:N1} ticks  ({1:N2}s)" -f (180.0/$deg), (180.0/$deg/$HZ))
    }
}

$did = $false

if (-not [double]::IsNaN($Q))     { Show-Length ($Q / $Q_ONE)  "Q20.12 raw $Q";        $did = $true }
if (-not [double]::IsNaN($Unit))  { Show-Length $Unit           "sim units $Unit";      $did = $true }
if (-not [double]::IsNaN($Hero))  { Show-Length ($Hero / $SCALE) "Hero units $Hero";    $did = $true }
if (-not [double]::IsNaN($GameU)) { Show-Length ($GameU * $S)   "game u/frame $GameU";  $did = $true }
if (-not [double]::IsNaN($Deg))   { Show-Angle  $Deg            "degrees $Deg";         $did = $true }
if (-not [double]::IsNaN($Bam))   { Show-Angle  ($Bam * 360.0 / 65536.0) "BAM $Bam";    $did = $true }

if (-not [double]::IsNaN($Ticks)) {
    Write-Host "from: $Ticks ticks"
    Write-Host ("  seconds        {0,12:N4}" -f ($Ticks / $HZ))
    Write-Host ("  ms             {0,12:N1}" -f ($Ticks / $HZ * 1000.0))
    $did = $true
}
if (-not [double]::IsNaN($Sec)) {
    Write-Host "from: $Sec seconds"
    Write-Host ("  ticks (60Hz)   {0,12:N2}" -f ($Sec * $HZ))
    Write-Host ("  frames (30Hz)  {0,12:N2}" -f ($Sec * 30.0))
    $did = $true
}

if ($Table -or -not $did) {
    Write-Host "constants"
    Write-Host ("  Q_ONE   {0}      (Q20.12: raw = units x 4096)" -f $Q_ONE)
    Write-Host ("  g_scale {0}    Hero units per sim unit (arena_bridge.cpp:23)" -f $SCALE)
    Write-Host ("  S       1/119   game u/frame -> sim unit/tick (height anchor)")
    Write-Host ("  tick    1/60 s`n")
    Write-Host "current tuning (from arena_tuning.h defaults)"
    Write-Host ("  {0,-20} {1,8} {2,10} {3,10} {4,12}" -f "constant","Q raw","sim unit","Hero","per second")
    Write-Host ("  " + ("-" * 64))
    $rows = @(
        @{ n = "TUNE_RUN_SPEED";    q = 618  },
        @{ n = "TUNE_RUN_ACCEL";    q = 51   },
        @{ n = "TUNE_RUN_FRICTION"; q = 122  },
        @{ n = "TUNE_JUMP_IMPULSE"; q = 1146 },
        @{ n = "TUNE_GRAVITY";      q = 71   },
        @{ n = "TUNE_AIR_CONTROL";  q = 34   },
        @{ n = "TUNE_BLAST_RADIUS"; q = 6553 }
    )
    foreach ($r in $rows) {
        $u = $r.q / $Q_ONE
        Write-Host ("  {0,-20} {1,8} {2,10:N5} {3,10:N2} {4,12:N3}" -f `
            $r.n, $r.q, $u, ($u * $SCALE), ($u * $HZ))
    }
    Write-Host ("`n  TUNE_TURN_RATE       {0,8} {1,9:N2} deg/frame  ({2:N0} deg/s)" -f `
        728, (728 * 360.0 / 65536.0), (728 * 360.0 / 65536.0 * $HZ))
    Write-Host ("`n  arena 0 half_x  {0,7:N2} sim  {1,8:N0} Hero" -f 7.9,  (7.9 * $SCALE))
    Write-Host ("  arena 0 half_z  {0,7:N2} sim  {1,8:N0} Hero" -f 3.87, (3.87 * $SCALE))
}
