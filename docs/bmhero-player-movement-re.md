# Bomberman Hero — campaign player movement (RE findings for A1.3)

**Task:** A1.3 Task 1 — decomp-only RE of the campaign player physics, to source the
`TODO(feel)` constants in `src/arena/arena_tuning.h`. Read-only work against the fork
decomp `C:\Users\dshi\GitRepos\BMHeroRecomp\lib\bmhero`.

**RECIPE DECISION: FALLBACK (approach B).**
The free-facing *standard* walker (the routine that turns `moveAngle` toward the stick
during normal 360° gameplay) is **entirely undecompiled MIPS assembly** in this decomp,
and no `.s` files are checked into the repo, so its **turn rate and its confirmation of
the free-walk speed model are not recoverable from readable source.** What *is* fully
readable and cross-confirmed: the game **logic Hz**, a **unit-scale anchor**, the complete
**vertical physics** (jump / gravity / terminal), and a **real-gameplay scalar-speed /
acceleration model** from the one decompiled gameplay controller (the "run-into-the-screen"
auto-runner, `code_extra_3`). We port those; the turn rate is a documented gap with the
exact stick→target-angle mapping and a recommended empirical starting point.

---

## Why the standard walker is unreadable (trace + gate evidence)

The per-frame player dispatch is `func_80087994` (`lib/bmhero/src/code/76640.c:901-927`):
it calls a **per-world** controller `func_..._code_extra_N` selected by `D_8016523E`
(world/theme index 0–6), then two shared calls (`func_80086D50` = build the render matrix,
`func_80086ECC` = the "arm-wind" companion object). All the player *movement* lives inside
the per-world `code_extra_N` overlay.

Decompilation status of the seven world controllers:

| world | overlay file | dispatch fn | status |
|------|--------------|-------------|--------|
| 0 | `overlays/128D20/128D20.c` | `func_8028AA70_code_extra_0` | **182 `GLOBAL_ASM`, 0 decompiled** |
| 1 | `overlays/134440/134440.c` | `func_802838DC_code_extra_1` | **69 `GLOBAL_ASM`, 0 player C** |
| 2 | `overlays/138360/138360.c` | `func_802823A4_code_extra_2` | **48 `GLOBAL_ASM`, 0 player C** |
| 3 | `overlays/13AC20/13AC20.c` | `func_80281314_code_extra_3` | **fully decompiled (76 bodies)** — the auto-runner |
| 4 | `overlays/13C4C0/13C4C0.c` | `func_8028117C_code_extra_4` | **21 `GLOBAL_ASM`, 0 player C** |
| 5 | `overlays/13DAD0/13DAD0.c` | `func_802860F8_code_extra_5` | **122 `GLOBAL_ASM`, 0 player C** |
| 6 | `overlays/144420/144420.c` | `func_80282F24_code_extra_6` | **67 `GLOBAL_ASM`, 0 player C** |

Only **world 3** is decompiled, and it is a **fixed-facing** mode (see `## Turn`).

The brief's anchor `code/76640.c:442-443` (inside the shared `func_80085B34`) computes the
current-vs-target facing every frame:
```c
D_801651D0 = gPlayerObject->moveAngle;                                  // current facing
D_801651D4 = Math_CalcAngleRotated(gActiveContStickX, -gActiveContStickY); // stick target
```
Both globals are **written here and reset in `func_80087E14` (997-998) but never *read* in
any decompiled function** — their only consumer is the undecompiled `code_extra_0` asm.
Storing *current* and *target* separately is the signature of a **bounded turn** (a snap
would need only the target), but the step size is in code we cannot read.

---

## Units keystone

### (a) Logic Hz = **60 (NTSC)** — scale factor **1.0** to the sim

Chain (all in `lib/bmhero/src/boot/`):
- `main.c:119-121` — `func_80001CF0(&D_8004D748, …, /*vimode*/2, /*retraceCount*/1)` for NTSC.
- `main.c:581` — `osViSetEvent(&arg0->unk40, 0x29A, retraceCount)` → the VI interrupt posts
  message `0x29A` **every `retraceCount`=1 retrace** ≈ 60 Hz.
- `main.c:619-650` `thread4_func` — on `0x29A` calls `func_80002130`, which
  (`main.c:676-680`) **forwards a retrace message to every client on every `0x29A`** (no
  divider).
- `main.c:167-199` `func_80000964` main loop — on the retrace message (`case 1`) it calls
  `func_8001E80C()` **unconditionally** when active (`D_8016E0A0!=0`). Rendering
  (`func_8001D9E4`) is separately gated on a free framebuffer (`sp30<2`), so **render may
  drop to ~30 fps while logic keeps stepping at 60 Hz.**
- `boot/17930.c:1789-1804` `func_8001E80C` — updates controllers then calls the game
  routine `gDebugRoutine2()` (= `func_80024744`, `boot/21E10.c:826`).

⇒ **Player logic ticks once per NTSC retrace = 60 Hz.** The sim is also 60 Hz, so the
**Hz rescale factor is 1.0**: game per-frame constants port directly (after spatial scale).

> Caveat / conflict: an earlier note in `arena_tuning.h` (bomb comment) assumed "30 Hz world
> units." That assumption is **not supported by the scheduler code** above. If in-game feel
> testing later shows the ported movement runs ~2× too fast, the game is effectively 30 Hz
> and every *rate* term rescales: velocities (u/frame) ×0.5, accelerations (u/frame²) ×0.25,
> impulses/terminal (u/frame) ×0.5. Primary conversion below uses the code-derived **60 Hz**.

### (b) World-unit anchor = **player collision extent**

From the decompiled auto-runner's collision probe box `D_80281590_code_extra_3`
(`overlays/13AC20/13AC20.c:5-8`): the 8 probe points sit at **±29.0** in X/Z and span
**0 → 119.0** in Y. So the game player collision cylinder is **radius ≈ 29 game-units,
height ≈ 119 game-units** (corroborated by the ground probes `+119.0f`/`+120.0f` in the
same file, lines 78/145).

Two candidate scale factors `S` (sim Q20.12 world-units per game-unit):

| anchor | game | sim (`arena_tuning.h`) | S = sim / game |
|--------|------|------------------------|----------------|
| **height (PRIMARY)** | 119 u | `TUNE_PLAYER_HEIGHT` = 1.0 | **1/119 = 0.0084034** |
| radius (alt) | 29 u | `TUNE_PLAYER_RADIUS` = 0.35 | 0.35/29 = 0.0120690 |

**Chosen: height anchor, S = 0.0084034.** Decisive corroboration: the game's real top run
speed of 10 u/frame maps to `10 × 0.0084034 = 0.0840` sim-u/frame — essentially the existing
hand-tuned `TUNE_RUN_SPEED = Q(0.085)`. That independent match says the sim's spatial scale
already *is* ≈ height-based. (The game's radius/height ratio 29/119 = 0.244 ≠ the sim's 0.35,
so the two anchors cannot both hold; height is chosen because locomotion values then agree
with the existing tuning. The radius column is provided for anyone who prefers to keep the
0.35 radius exact — it only rescales all magnitudes uniformly; **the ratios in each section
below are anchor-independent and are the robust core.**)

Conversion recipe used throughout: **Q20.12 value = game_value × S × Hz_factor × 4096**,
with S = 0.0084034 and Hz_factor = 1.0.

---

## Turn

**Standard (free-facing) walk turn rate: NOT RECOVERABLE (gate trigger).** It is applied in
the undecompiled `code_extra_0` asm that reads `D_801651D0`/`D_801651D4` (above). The
*only* decompiled gameplay controller, the auto-runner `code_extra_3`, **fixes facing** and
so proves nothing about the turn rate:
- `func_802811A8_code_extra_3` (`overlays/13AC20/13AC20.c:361`): `moveAngle = 180.0f;`
- `func_80281314_code_extra_3` (`…:401`): `gPlayerObject->Rot.y = 180.0f;`

What **is** established about turning:

1. **Target-angle mapping** — `Math_CalcAngleRotated(gActiveContStickX, -gActiveContStickY)`
   (`code/76640.c:443`), body at `boot/math_util.c:147-155`:
   ```c
   f32 Math_CalcAngleRotated(f32 a0, f32 a1){ angle = Math_Atan2f(a0, -a1) + 90.0f; wrap<360; }
   ```
   With `a0=stickX, a1=-stickY` this is `atan2(stickX, stickY) + 90°` (degrees, `Math_Atan2f`
   at `math_util.c:51` returns [0,360)). `+90°` aligns stick-up with the facing basis.

2. **Stick is camera-rotated first** — `func_80024744` (`boot/21E10.c:826-835`): for camera
   types {1,2,5,6,7,8} (the arena is type 6) it rotates the raw stick vector by `gView.rot.y`
   about Y *before* it reaches `Math_CalcAngleRotated`:
   ```c
   guRotateF(mf, gView.rot.y, 0,1,0);
   guMtxXFMF(mf, gActiveContStickX, 0, gActiveContStickY, &gActiveContStickX, &sp6C, &gActiveContStickY);
   ```
   (This is the native camera-relative mapping already documented in integration-notes §8.11.)

3. **Velocity follows *facing*, not the stick** — see `## Speed`. The game rotates a scalar
   speed by `moveAngle`; the turn *is* the steering. Contrast the debug free-mover
   `Debug_HandleObjMovement` (`boot/2BF00.c:488`), which **snaps** `moveAngle` to the target
   every frame — that is debug behavior, not gameplay.

**Downstream recommendation (Task 2):** replace the sim's instant `p->yaw = dir`
(`arena_sim.c:138`) with a bounded turn toward `dir`. Rate is a **GAP — pick empirically**,
not from decomp. Suggested seed **≈ 12°/frame** (binary-angle `12 × 65536/360 ≈ 2185`
u16/frame), tuned in-game; the homing-projectile primitive `func_800157EC`
(`math_util.c:176-190`, returns −1/0/+1) applied at `69AA0.c:1359` gives only ~1°/frame,
which is far too slow for a player and is *not* the player rule.

---

## Speed

Source: decompiled auto-runner, `overlays/13AC20/13AC20.c`. This is a real gameplay mode
(constrained facing) — the **best available** speed model; whether the free walker shares
the exact 0.2 / 10 numbers is **unconfirmed** (medium confidence, since `moveSpeed` is a
shared object field and the integration `func_80280B30` is a copy of the shared
`func_8002D080`).

- **Scalar-speed acceleration** — `func_802807EC_code_extra_3:207-217`: each frame step
  `moveSpeed` toward a target `sp4` by **0.2 u/frame** (same rate up and down; overshoot
  clamps to target):
  ```c
  if (moveSpeed <= sp4){ moveSpeed += 0.2; if(moveSpeed>sp4) moveSpeed=sp4; }
  else               { moveSpeed -= 0.2; if(moveSpeed<sp4) moveSpeed=sp4; }
  ```
- **Speed clamp** — `…:218-221`: `moveSpeed ∈ [−7.0, +10.0]`. Top forward speed **10.0**,
  reverse cap −7.0 (reverse is this mode's back-pedal; free-walk has no reverse — omit).
- **Target-speed tiers by stick** — `…:194-206` (this mode reads stick-Y magnitude):
  `< −40 → −7`, `< −10 → −4`, `≤ 10 → 0` (deadzone ±10), `≤ 30 → 4`, `≤ 50 → 7`, else `10`.
  ⇒ **stick deadzone ≈ 10/128, and full deflection commands top speed.** For the sim, the
  analog magnitude scales the *target* speed (the sim already does this with `mag`).
- **Neutral stick ⇒ decelerate to 0 at the same 0.2/frame** (the `sp4=0` branch). So
  **friction rate == accel rate == 0.2 u/frame.** There is *no* separate friction constant.
- **Velocity = scalar speed along facing** — `func_80280B30_code_extra_3:236-237`
  (≡ shared `func_8002D080`, `boot/2BF00.c:479-482`):
  ```c
  Vel.x = sinf(moveAngle·DEG2RAD) · moveSpeed;
  Vel.z = cosf(moveAngle·DEG2RAD) · moveSpeed;   // moveAngle 0 → +Z, 90 → +X
  ```
  (Lateral `Vel.x = ±8` in `func_80280A7C:225-233` is a rail-mode side-shift; not general.)

**Anchor-independent ratios (the robust core):** accel / top = 0.2/10 = **0.02 of top speed
per frame ⇒ 50 frames (0.83 s @60 Hz) to reach full speed.** This is far more momentum than
the current placeholder (accel/top = 0.010/0.085 = 0.118 ⇒ ~8.5 frames) — **the main feel
gap.**

Converted (S=0.0084034, 60 Hz):

| constant | game | × S | Q20.12 (raw) | current placeholder |
|----------|------|-----|--------------|---------------------|
| `TUNE_RUN_SPEED` | 10 u/f | 0.084034 | **Q(0.084)** (344) | Q(0.085) (348) |
| `TUNE_RUN_ACCEL` | 0.2 u/f | 0.0016807 | **Q(0.00168)** (7) | Q(0.010) (41) |
| `TUNE_RUN_FRICTION` (= accel) | 0.2 u/f | 0.0016807 | **Q(0.00168)** (7) | Q(0.008) (33) |

*(radius anchor gives Q(0.121)/Q(0.0024)/Q(0.0024) — same ratios, uniformly larger.)*

---

## Airborne

Vertical physics are **game-global and high-confidence**: the auto-runner and the debug
mover give **identical** constants, and these are the only vertical numbers in the game.

- **Jump impulse** — `func_802811F8_code_extra_3:369-372`: on the jump trigger
  `Vel.y = 33.333332f;` and set the airborne bit `D_801651A4 |= 1`. Debug mover agrees:
  `Vel.y = 33.29999924` (`boot/2BF00.c:504`).
- **Gravity** — `func_80280BD8_code_extra_3:240-248`: while airborne (`D_801651A4 & 1`)
  `Vel.y -= 2.083333` per frame; grounded ⇒ `Vel.y = 0`. Debug mover agrees:
  `Vel.y -= 2.083333` (`boot/2BF00.c:534`). (2.083333 = 25/12.)
- **Terminal velocity** — `…:243-244`: clamp `Vel.y ≥ −48.0`. Debug mover agrees
  (`boot/2BF00.c:535-536`).
- **Air steering** — in the auto-runner, `func_802811F8`/`func_80281294` keep calling the
  speed and lateral updates **while airborne**, i.e. **full control in air** (no reduction).
  Standard-walk air control is unknown; recommend equal-to-ground or a mild reduction and
  tune empirically. (A1.3 SHIPPED `TUNE_AIR_CONTROL` = Q(0.00168) = full air control,
  i.e. equal to ground accel; low-confidence/empirical — revisit in the feel pass.)
- Integration order (`func_80281314:397-400`): state machine → gravity (`func_80280BD8`) →
  move/collide (`func_80280000`). Jump is an **edge-triggered instantaneous `Vel.y` set**,
  matching the sim's structure.

**Anchor-independent ratios:** apex time = 33.333/2.0833 = **16 frames (0.267 s @60 Hz)**;
jump height = v²/2g = **266.7 game-u = 2.24 player-heights**; terminal / top-run =
48/10 = **4.8×**.

Converted (S=0.0084034, 60 Hz):

| constant | game | × S | Q20.12 (raw) | current placeholder |
|----------|------|-----|--------------|---------------------|
| `TUNE_JUMP_IMPULSE` | 33.3333 u/f | 0.280112 | **Q(0.280)** (1147) | Q(0.140) (573) |
| `TUNE_GRAVITY` | 2.083333 u/f | 0.0175070 | **Q(0.0175)** (72) | Q(0.0075) (31) |
| `TUNE_TERMINAL_VY` **(new)** | −48 u/f | −0.403361 | **Q(−0.403)** (−1652) | — (none) |

*(radius anchor: Q(0.402)/Q(0.0251)/Q(−0.579) — same ratios.)*
Height check after conversion: 0.280²/(2·0.0175) = 2.24 sim-u = 2.24× height ✓ (consistent).

---

## Model-structure delta vs current `player_tick`

Current sim (`src/arena/arena_sim.c`, `player_tick` ~119):
1. **Facing snaps** instantly: `p->yaw = dir` (line 138).
2. Builds a **target velocity vector** straight from the stick direction (`sx,sz`,
   lines 134-137) and accelerates the velocity vector toward it per-axis
   (`qclamp(sx - vel.x, -acc, acc)`, lines 146-147). Facing is a passive by-product.
3. Separate `RUN_ACCEL` / `RUN_FRICTION` / `AIR_CONTROL`; no terminal-velocity clamp.

Game model:
1. **Facing (`moveAngle`) turns toward the stick target at a bounded rate** (rate = the GAP).
2. Maintains a **scalar `moveSpeed`** accelerated toward a stick-magnitude target at a
   single 0.2-equivalent rate (accel == decel == friction).
3. **Velocity = scalar speed × facing direction** (`Vel = {sin,cos}(moveAngle)·moveSpeed`) —
   you move where you *face*; **no strafing.** Turning is what redirects momentum.
4. Vertical: edge jump impulse, per-frame gravity while airborne, terminal clamp (sim
   matches structurally; values differ and the terminal clamp is missing).

**What the sim must add for A1.3:**
- **[Turn] (biggest change)** a bounded yaw turn toward `dir` (replaces the line-138 snap) —
  rate empirical (seed ≈ 12°/frame ≈ 2185 u16/frame).
- **[Speed] restructure to scalar-speed-along-facing:** drive a scalar `speed` toward
  `mag·TUNE_RUN_SPEED` at `TUNE_RUN_ACCEL` (= friction), then set
  `vel = speed × (sin(yaw), −cos(yaw))`. This couples velocity to the *gradually-turning*
  facing (the momentum feel), instead of pointing velocity at the raw stick.
- **[Values]** run-accel ≈ **7× smaller** (Q0.010→Q0.00168) = much more momentum; jump ≈ 2×
  (Q0.140→Q0.280); gravity ≈ 2.3× (Q0.0075→Q0.0175); **add** `TUNE_TERMINAL_VY` = Q(−0.403).
- All constants land in `arena_tuning.h` only; bump `TUNE_VERSION` (intentional gameplay
  change) and re-pin the CI hash.

---

## Uncertainty summary

| item | confidence | note |
|------|------------|------|
| Logic Hz = 60 | **high** | scheduler code; contradicts old 30 Hz assumption — verify by feel |
| Unit scale (height, S=0.0084) | **medium-high** | corroborated by top-speed↔placeholder match; ratios are anchor-free |
| Jump / gravity / terminal | **high** | two independent decompiled sources agree; game-global |
| Speed accel 0.2, top 10, friction=accel | **medium** | from the decompiled auto-runner; free-walk numbers unconfirmed |
| Scalar-speed-along-facing structure | **high** | shared `func_8002D080` integration |
| **Turn rate (deg/frame)** | **NONE** | undecompiled `code_extra_0` asm; empirical only — GAP |
| Air-control reduction | **low** | auto-runner uses full air control; free-walk unknown |
