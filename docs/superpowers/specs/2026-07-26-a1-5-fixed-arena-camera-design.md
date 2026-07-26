# A1.5 — Fixed arena camera — design

**Date:** 2026-07-26
**Status:** approved, pending implementation
**Repo:** `dcmshi/BMHeroRecomp` (fork-side only — no sim change, no submodule bump)
**Predecessor:** A1.4 (`8.13`), canonical `main` @ `73402a3` (`TUNE_VERSION` 7)

## Problem

Feel-testing is untrustworthy. The user's report: *"the sim isn't hooked up 1 to 1
with the actual recomp/drawing logic."*

The position pipeline is in fact already 1:1 — A1.4 replaced the delta drive with
an **absolute** drive (§8.13), so the rendered player position *is* the sim
position by construction and cannot drift. What is lying is the **camera**:

1. **Drift.** The Nitros boss room has a rail camera that swings `rot.y` across
   **25–89°** and periodically **cuts** (eye teleports ~1800 units). Because the
   game rotates the stick in place by `gView.rot.y` (`func_80024744`, camtype 6 —
   §8.11), a *held* stick direction maps to a *changing* world direction. Holding
   "up" curves. Any judgement of turn rate, friction, or momentum made through
   that camera is contaminated.
2. **Foreshortening.** Under the room's pitched perspective, motion toward/away
   from the camera covers less screen distance per world unit than motion across
   it, so W/S reads slower than A/D despite being isotropic in world space.

Both are stand-in artifacts of borrowing a boss room as an arena. Neither is a sim
bug. Both are fixed by owning the camera — **without** rebuilding the map.

This matters right now because the 6°/frame turn rate (canonical v7) was chosen
from *measured geometry* and still needs a subjective confirm. Confirming it
through a swinging camera would produce a meaningless answer.

## Non-goals

- **No sim change.** `TUNE_VERSION` stays 7, hash stays `07fc6ade`, and the
  submodule pointer does not move. This slice is entirely fork-side.
- **Not building a real arena map.** That remains its own (large) milestone. This
  slice makes the *existing* stand-in readable.
- **Not A1.2g.** Exit trigger, damage tiles, explosion visual and HUD are a
  separate spec, written after this one is verified working.
- **No camera control/feel features** — no follow, no zoom, no shake, no
  sudden-death reframing. One static pose. YAGNI.

## Prior art this must respect

- **§5 — measure, don't guess.** A1.2a tried blind camera compensation (a guessed
  1.4× Z boost) and it looked *more* compressed, i.e. the guess was backwards. The
  recorded lesson is that the camera's real orientation must be measured. This
  design therefore measures first and applies second, as two separate steps.
- **§8.11 — the stick is already camera-relative.** The game rotates
  `gActiveContStickX/Y` by `gView.rot.y` for `gCameraType ∈ {1,2,5,6,7,8}`; the
  Nitros arena is type 6. Adding our own rotation double-rotates (cost: forward
  speed cut to ⅓). We must not add rotation — we make `rot.y` *stable* instead.
- **§8.11 — no silent libcalls.** An emitted math libcall in a patch links
  silently and jumps to 0. This design uses **no runtime trig** (below).
- **§8.13 — re-assert every frame.** The boss re-activates if the sweep stops, so
  the established idiom is to overwrite each frame rather than set once.

## Architecture

### Component 1 — measurement probe (`ARENA_AUTO_BATTLE=6`)

Ships and runs **before** any override exists. Logs, every 30 frames:

```
[cam] type=%d rot=(%.2f,%.2f,%.2f) at=(%.1f,%.1f,%.1f) eye=(%.1f,%.1f,%.1f) dist=%.1f
```

plus `gPlayerObject->Pos` on the same line cadence.

It answers four things we must not assume:

| question | why it matters |
|---|---|
| Are the `struct View` offsets right? | `at`@0x00 precedes `eye`@0x0C (§8.11 corrected an earlier misread). Garbage values here mean a bad read, caught before we write. |
| What does the rail camera actually do? | Confirms the 25–89° swing and the cut, and gives the real value ranges. |
| Is `gCameraType` 6 in this room? | Determines whether the stick is rotated at all. If it is *not* in `{1,2,5,6,7,8}`, the whole input mapping premise changes. |
| Where does the arena sit in camera terms? | Gives the framing distance empirically — we do not know the FOV, so `CAM_DIST` must be measured, not derived. |

Gate: `arena-soak.ps1 -Mode 6 -Expect '\[cam\] type='`.

### Component 2 — the override

**Location:** `patches/arena_render.c`, inside the existing
`if (arena_bridge_is_battle() && gPlayerObject != NULL)` block at the top of
`arena_render_routine`, immediately after the boss-suppression sweep and
**before** the `func_80024744()` call on line 136.

**The ordering is load-bearing.** `func_80024744` reads `gView.rot.y` to rotate
the stick. If we wrote the camera after that call, the stick for that frame would
be rotated by the *game's* swinging yaw while the *picture* used ours — the two
would disagree by up to 64°, which is worse than the bug we are fixing.

**What is written**, every frame:

```
gView.at   = arena centre, in Hero world coords
gView.eye  = at + fixed offset
gView.up   = (0, 1, 0)
gView.rot  = (CAM_PITCH_DEG, CAM_YAW_DEG, 0)
gView.dist = CAM_DIST
```

The exact `rot` component order is one of the things the probe confirms;
`rot.y` is known to be yaw (§8.11), the other two are verified before use.

### Component 3 — the pose

**Yaw = 0°.** Two reasons, both structural rather than aesthetic:
- The arena is wider than deep (`half_x` 7.9 vs `half_z` 3.87 sim), so looking
  along Z puts the long axis horizontal across the screen — correct framing.
- The game's stick rotation by `rot.y` becomes the **identity**. Stick-up maps to
  one fixed world axis forever. For a slice whose entire purpose is a trustworthy
  feel read, an identity input mapping is worth more than any framing preference.

**Pitch = 60°** (from the horizontal, looking down). For a camera pitched θ above
the ground plane, a world-Z (toward/away) displacement projects onto the screen
compressed by **`sin θ`** relative to an equal world-X (across) displacement — at
θ=90° (straight down) the factor is 1 and there is no foreshortening at all.

| pitch | `sin θ` = W/S vs A/D screen travel | arc height readable |
|---|---|---|
| 45° | 0.71 — the artifact we are removing | yes |
| **60°** | **0.87 — near-equal** | **yes** |
| 80° | 0.98 — effectively none | poor |

60° is the balance point: the compression is small enough not to mislead a feel
read, while the view keeps enough depth to judge bomb arcs and player separation.

**No runtime trig.** Pitch and yaw are compile-time constants, so their sine and
cosine are too. With yaw 0:

```c
/* precomputed: sinf(60deg)=0.8660254, cosf(60deg)=0.5 — NEVER call sinf/cosf in
 * a patch; an emitted libcall links silently and jumps to 0 (§8.11). */
#define CAM_SIN_PITCH  0.8660254f
#define CAM_COS_PITCH  0.5f
eye.x = at.x;
eye.y = at.y + CAM_DIST * CAM_SIN_PITCH;
eye.z = at.z + CAM_DIST * CAM_COS_PITCH * CAM_Z_SIGN;
```

`CAM_Z_SIGN` (+1/−1) is resolved by the probe — whether the camera must sit at
+Z or −Z of the arena depends on the room's handedness, and guessing it yields a
mirrored view rather than an obvious error.

**Arena centre.** New native exports `arena_cam_at_x/y/z`, computed with the
*existing* frozen-origin mapping so the camera and the actors agree by
construction:

```
at.x = g_origin_x + (0 - g_ref_sx) * g_scale     /* sim (0,0) = arena centre */
at.z = g_origin_z + (0 - g_ref_sz) * g_scale_z
at.y = g_origin_y + CAM_AT_Y_LIFT                /* aim slightly above the floor */
```

Reusing that mapping is deliberate: if the origin capture is ever wrong, the
camera is wrong *in the same direction* as the actors, which keeps the picture
self-consistent and makes the error obvious rather than confusing.

**Tunables.** `CAM_PITCH`, `CAM_YAW`, `CAM_DIST`, `CAM_AT_Y_LIFT`, `CAM_Z_SIGN`
are `#define`s. `CAM_DIST` starts from the probe's measurement and is iterated by
screenshot. Iteration is cheap now — `build.ps1 -Config rwdi -Soak 5` is one
command.

## The risk worth naming

**If the game's camera routine runs after our hook, our writes are stomped and
the override silently does nothing** — the picture would look unchanged and we
could waste a session tuning constants that never take effect.

**Detection is built in, not hoped for:** the probe logs `gView` at routine
*entry*. Once the override is live, entry values should equal what we wrote on the
previous frame. A mismatch proves another writer owns `gView` after us.

**Fallback if that happens:** patch the game's camera update to no-op while
`arena_bridge_is_battle()`, the same shape as the existing `func_80081C50` warp
patch. More RE, but bounded and already a proven pattern in this codebase.

## Testing

| what | how |
|---|---|
| struct offsets sane | probe values are plausible world coords, not garbage/NaN |
| camera type | probe prints `type=6`; if not in `{1,2,5,6,7,8}`, stop and re-plan the input mapping |
| **no drift** (the core property) | new soak gate `-Constant 'rot=\(([-0-9.]+)'` — ≥2 samples, all identical. Drift is the bug, so *constancy* is the assertion |
| override actually takes | entry-logged `gView` matches the previous frame's written values |
| framing | `capture-game.ps1` screenshot: whole arena in frame, no clipping |
| no regression | 5/5 boot soak; `-AnimProbe` still passes |
| input still sane | probe drives full stick +X then −Z; player travels the corresponding world axes (sim already guarantees isotropy, so this checks the mapping, not the physics) |

**New soak gate.** `arena-soak.ps1` gains `-Constant '<regex with one capture>'`,
asserting ≥2 samples that are all equal. This completes the trio alongside the
existing `-Expect` (appears) and `-Rising` (increases), and needs no harness edit
for future probes.

## Order of work

1. Probe (`-Mode 6`) + the `-Constant` soak gate — measure, resolve `CAM_Z_SIGN`
   and the `CAM_DIST` starting value, confirm `gCameraType`.
2. Override + native `arena_cam_at_*` exports, pose from step 1.
3. Verify: constancy gate, entry/write match, screenshot, 5/5 soak.
4. Iterate `CAM_DIST` / `CAM_AT_Y_LIFT` by screenshot until framing is clean.
5. Hand off for the feel-boot — **including re-testing the 6°/frame turn rate**,
   which is the reason this slice exists.

## Success criteria

- `gView.rot.y` is provably constant for a whole battle session.
- A held stick direction moves the player along a straight screen path.
- W/S and A/D read as comparable speeds.
- The whole arena is in frame.
- 5/5 soak green, `-AnimProbe` still green, sim untouched (`TUNE_VERSION` 7,
  hash `07fc6ade`).
- The user can give a real answer on 6°/frame.
