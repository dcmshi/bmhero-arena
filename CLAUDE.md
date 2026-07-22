# Bomberman Hero Multiplayer — project context for Claude Code

Multiplayer (battle arena + later co-op campaign) for Bomberman Hero, built on
BMHeroRecomp (N64Recomp static recompilation + RT64). Read these before any work:

- `docs/bmhero-multiplayer-architecture.md` — overall design: two sim domains,
  determinism model, netcode topology, milestones (arena-first, A0 done)
- `docs/bmhero-battle-arena-design.md` — the mode being built now: ruleset,
  ArenaState spec, tick pipeline, GekkoNet plan, render bridge, host-session model
- `docs/bmhero-recomp-integration-notes.md` — **living RE reference for the
  fork-side render bridge** (patch/export mechanism, per-frame hooks, object &
  player struct, input, camera, coord mapping) — read before any A1.2+ work
- `README.md` — this repo's layout, build, and the five invariants

## Current status (2026-07-22)

**A1.2c complete — blasts render as pops; THE load crash fixed (2026-07-22).**
Slice 2 shipped the fallback blast visual: 4 pooled blast actors (bomb mesh,
proven recipe) appear at each live `blasts[]` center for the blast's 20-tick
life — detonations now give visible feedback at the right spot (spread → up to
4 pops). The game's effect spawner (`func_80081468`) was investigated and
**abandoned** for the bypassed arena (effects invisible; two IDs crash — §8.9).
**Two big wins from the debugging:** (1) the **stochastic "black screen
selecting battle" crash is fixed** — an upstream debug print in
`load_from_rom_to_addr` racing the loader (named by a symbolized RWDI dump);
it had contaminated earlier findings, so the "pool ceiling" result is invalid.
(2) **Symbolized-dump tooling** now exists (`build-rwdi` + `playrwdi.bat`) and
a source-level **patch-machinery reference**
(`docs/bmhero-recomp-patch-machinery-reference.md`) corrects earlier folklore
(data symbols resolve from `data_dump.toml`; Release `0xC0000409` = uncaught
exception; patch `.data/.bss` is memory-backed). Also: the generic `[14..77]`
draw **ignores `Scale`**; the boss re-activates if the sweep stops (every-frame
sweep stays). Follow-ups: real explosion visual (effect-asset RE), real bomber
mesh (§8.5b), re-test larger bomb pools. Fork branch
`feature/a1.2b-spawn-bombers`.

**A1.2c slice 1 complete — bombs render + set/kick wired (2026-07-21).** The
sim's live bombs are drawn in the arena: a pool of 6 bomb actors
(`=TUNE_MAX_LIVE_BOMBS`) spawned once (proven `func_80027464` + `func_8001ABF4`
recipe), toggled active/hidden each frame from `bombs[].state`, positioned from
the sim **including Y** (throw arc shows; new `g_ref_sy` capture). Verified: a
throw + the 4-bomb spread appear and arc on screen. Input now folds
jump|bomb|set into a packed `buttons` arg (the export ABI caps at 4 args) and
wires **set/kick = `CONT_G`** (Z / Q key); set is log-confirmed (Q → live-bomb
count 0→4) — set bombs spawn at the player's feet so they're visually subtle.
The boss-suppression sweep now spares all actor slots via `arena_is_actor_slot`.
Fork branch `feature/a1.2b-spawn-bombers`.
- **Pool ceiling (finding):** 6 bomb actors stable, **8+ crash at spawn** — the
  suppressed Nitros boss holds model/anim-pool slots that `actionState=NONE`
  doesn't free, leaving little headroom. Raising the sim bomb-cap (A1.3) needs
  the boss's slots actually freed first (integration notes §8).
- **Deferred to A1.2c slice 2 — blasts/explosions:** detonation currently = the
  bomb vanishes (no explosion effect). The visual payoff (explosion, fuse blink,
  event feedback) is the next slice; candidates `gFileArray[0xA/0xB/0xC]`,
  `blasts[]` growth via `radius_t`/`ttl`.

**A1.2b complete — 4 actors on screen with bomb placeholders (2026-07-21).**
The full simulated roster is puppeted in the arena: player 0 (the campaign
object, A1.2a) + **3 extra actors spawned into `gObjects[14..77]`** via the
game's own `func_80027464`, positioned from the sim, on a **clean flat arena**
(`MAP_NITROS_1`, boss suppressed). Stable, no mirror, no crash — confirmed on
screen. The "animated models can't draw via the general spawn" wall (was
BLOCKED) is broken. Actors are **bomb placeholders**; the real bomber mesh is a
scoped follow-up (skeletal model — §8.5b). Full RE detail in **integration
notes §8** (rewritten). Fork branch `feature/a1.2b-spawn-bombers`.
- **The unlock:** `func_80027464` loads model parts (`Unk140`) but not the
  animation instance an animated model needs; adding **`func_8001ABF4(slot,0,0,
  cfg)`** binds it (`Unk148`/`D_8016C298` pool) and the draw stops aborting.
- **Two hard patch gotchas found:** (1) auto-named DATA symbols (`D_801163DC`)
  don't resolve via the patch reloc path and silently corrupt the whole patch →
  pass their **address as a literal** `((T*)0x801163DC)`; (2) the export ABI
  can't take float ARGS → pass floats as **`u32` bit patterns** through int args
  (union bitcast). Both cost hours; §8.2.
- **Positioning fix:** the earlier positioning crash was the **Battle Room's
  pits** — actors at the sim corners land off-platform, where per-object
  collision (`func_8001CD20`, runs on all active `[14..77]` regardless of objID)
  aborts. Fixed by warping to a **flat arena** (`ARENA_WARP_MAP`=15) + a
  **boss-suppression sweep** (deactivate non-puppet `[14..77]` before the update
  loop). §8.5.
- **Real bomber mesh — follow-up (§8.5b):** swapping bomb (`gFileArray[9]`) →
  bomber (`gFileArray[1]`, cfg `0x13`) spawns but **white-screens on draw** —
  the player-bomber is a multi-part **skeletal** model (`4DFF0.c` loads it in a
  loop w/ per-part offsets); needs per-part skeletal binds + an idle pose. A
  scoped rendering sub-task. Inert objID also still TBD (door behaviour is
  harmless now that neighbours are suppressed).
- **Tooling:** `PrintWindow` screenshots (occlusion-proof), `arena_dbg_u32` →
  `arena_bridge.log` markers; hands-off keyboard input to the game is unreliable
  (SDL focus) — human does the ~15s room nav, agent builds + verifies (§8.8).
- **Bugfix (earlier pass):** idle players 1–3 were fed raw-`0` `ArenaInput`
  (decodes to `(−32,−32)`); neutral is `arena_input_pack(0,…)`=`0x820`. Fixed in
  `arena_bridge.cpp`; changes the non-battle proof-of-life hash, not the pinned
  CI sim hash. Static patches must stay **stateless** (patch-local mutable
  statics abort `0xC0000409`) — keep state native (`cdb` forensics, §1).

## Current status (2026-07-18)

**A0 complete.** Headless deterministic arena sim in `src/arena/`, all tests
green (`tests/test_determinism.c`): bit-identical replay, GekkoNet-style
rollback stress, snapshot round-trip, liveness. Scripted-match hash pinned at
`4b6687d4` (re-pinned 2026-07-19 with the bomb-mechanics correction —
`TUNE_VERSION` 2, first intentional gameplay change; previously `a55aa9b1`)
— CI matrix on GitHub: https://github.com/dcmshi/bmhero-arena.

**A1.2a complete (2026-07-20).** First render bridge on screen: in the Battle
Room the campaign player object (`gPlayerObject`) is puppeted from our
fixed-point sim, drawn by Hero. A `RECOMP_PATCH` on the 6-line level-enter
`func_800824A8` routes the per-frame `gDebugRoutine2` through
`arena_render_routine`, which reads the controller, ticks the sim via native
`arena_export_tick_input`, and moves the player **by the sim's per-frame
displacement** (dx/dz added to live Pos; **Y left to the game** so it stays
grounded and the camera follows — no teleport, no fragile spawn-capture).
Native coord exports in `arena_bridge` (syms.ld `0x8F000128+`); VI-callback
tick gated off in battle so the patch drives the tick. Verified: bomberman
moves under our physics on screen. **Known item:** forward/back reads
"compressed" (camera foreshortening); the right fix is camera-relative input
using the real `gView` transform — deferred to the feel pass (blind
scale-tuning went the wrong way, so measure, don't guess). Fork branch
`feature/a1.2a-puppet-player`. Build gotcha: after editing any `patches/*.c`,
`make clean` in `patches/` before the cmake build (ninja doesn't reliably
re-run the patch make — stale patch = mismatch crash). Next: A1.2b spawn +
puppet all 4 bombers.

**A1.1b-ii complete (2026-07-20).** Battle launch warps into `MAP_BATTLE_ROOM`
(Hero's own dedicated arena — loads cleanly on direct entry, no fallback
needed) instead of the campaign level. Mechanism: `arena_bridge_is_battle`
native export (`syms.ld` `0x8F000124` + `REGISTER_FUNC`) lets
`patches/arena_warp.c` `RECOMP_PATCH` `func_80081C50` (the level-load prep
that seeds the next-level var + spawn from `gCurrentLevel`, called from 9
level-transition handlers) override `gCurrentLevel` before the loader reads
it; non-battle launches unchanged. First `patches/` change since A1.0
(LLVM-15 MIPS). Verified by boot gate (Battle → Battle Room). Fork branch
`feature/a1.1b-ii-map-warp`.

**A1.1c dropped/deferred (2026-07-20, decided during brainstorming).** Two
findings collapsed it: (1) the Battle Room is already a clean versus arena —
**no campaign enemies to suppress** (only the campaign player object + HUD,
which are A1.2 render-bridge concerns); (2) the intro→file-select frontend
skip is deep, fragile RE — it lives inside `func_80083180`, a giant
"not reducible" goto state machine with no clean patch seam, and it's a UX
nicety, not a blocker. Deferred in favor of A1.2. Revisit the frontend-skip
later (lightweight input-injection or state-machine RE) if the friction
matters. Next: **A1.2 render bridge** — write `ArenaState` into `gObjects`
so Hero draws our 4 bombers in the Battle Room.

**A1.1b complete (2026-07-19).** The recomp launcher has a **Battle** option
(`on_launcher_init`, `main.cpp`) that sets a native battle-mode flag
(`arena_bridge_set_battle_mode`) then launches via `recomp::start_game` — same
path as Start Game, gated on `recomp::is_rom_valid`. The per-VI proof log
distinguishes battle (`[arena] BATTLE MODE tick N …`) from normal; confirmed
by boot gate (Battle → BATTLE MODE, still lands in single-player campaign as
expected — the destination warp is A1.1b-ii). Convenience `play.bat` in the
fork root launches from the repo root so `assets/` resolves (raw double-click
of the exe crashes `0xC0000409`). Fork branch `feature/a1.1b-battle-menu`.
The battle flag is the seam A1.1b-ii (warp to a dedicated arena map) and A1.2
(render bridge) build on. Next: A1.1b-ii forced map-shell warp.

**A1.1a complete (2026-07-19).** `bmhero-arena` is a submodule of the fork at
`lib/bmhero-arena`; `arena_sim.c` compiles natively into `BMHeroRecompiled`
(clang-cl), and a fork-owned `src/arena_bridge/` ticks a silent-passenger
`ArenaState` from the recomp's native VI callback (`main.cpp`), logging
`[arena] tick N hash H` to console + `arena_bridge.log` each second —
verified advancing (60/120/180…) and deterministic across runs
(tick 120 → `b6272ae5`). **No MIPS patch needed** — the native VI hook is the
design's tick edge, dissolving the spec's "which per-frame function to patch"
risk. Fork branch `feature/a1.1a-arena-proof`. Next: A1.1b Battle menu entry
(+ warp-into-map), A1.1c spawn suppression, A1.2 render bridge.

**A1.0 complete (2026-07-19).** Fork `dcmshi/BMHeroRecomp` builds locally and
boots the campaign (intro + level play confirmed) at
`C:\Users\dshi\GitRepos\BMHeroRecomp`. Repo decision: `bmhero-arena` stays
canonical; the fork will consume it as a **submodule** in A1.1. Build recipe
(the toolchain fight is documented so A1.1+ can rebuild):
- N64Recomp built from source with MSYS2 gcc → `N64Recomp.exe`/`RSPRecomp.exe`
  staged in the fork root (they need `C:\msys64\ucrt64\bin` on PATH at run
  time for their runtime DLLs).
- Main build: VS 2022 clang-cl + `cmake --build build-cmake --target
  BMHeroRecompiled` from a VS dev shell.
- **`patches/` sub-build needs LLVM 15** (upstream CI uses clang-15): VS's
  clang-19 has no MIPS backend; MSYS2's LLVM-22 lld rejects the old linker
  flags. Fix: portable LLVM 15.0.7 extracted (no install) to
  `C:\Users\dshi\GitRepos\.tools\llvm15`. Zero fork source changes.
- Full-build PATH: `…\.tools\llvm15\bin` + VS dev-shell PATH +
  `C:\msys64\ucrt64\bin` + `C:\msys64\usr\bin` (order matters: LLVM15 first
  so patches' `clang`/`ld.lld` = v15; MSVC `link.exe` before msys; `make`
  from usr/bin). Your ROM's sha1 is already the recompiler-input image.
Default keyboard controls (recomp): WASD stick, Space=A, LShift=B, Q=Z, E=L,
R=R, Enter=Start, arrows=C, IJKL=D-pad, Esc=recomp menu. Next: A1.1 mod
scaffold — arena submodule + Battle menu entry + map-shell load.

**A2 SyncSession complete (2026-07-19).** `src/netplay/` wraps GekkoNet
(FetchContent, pinned tag `v20260629200724-02c447c`, BSD-2) behind one C
interface — couch/online/stress — the session owns `ArenaState` and is the
sole `arena_tick` caller. Gates: GekkoNet stress session (continuous
rollback re-sim, 3600 ticks clean) + two-process localhost match with
matching confirmed hashes (`p2p tick 600 hash bbf9c071`), both in ctest and
the `netcode` CI job (ubuntu+windows). Viewer drives matches through the
session (couch default; `--host <port> --peer <addr>` / `--join <addr>
--port <p>` for 2P online; `--frames` smoke stays sessionless, hash
`eeeb76f6`). Spec: `docs/superpowers/specs/2026-07-19-a2-syncsession-design.md`.

**Bomb mechanics are Hero-authentic (2026-07-19).** Fixed-arc throw (no
stick/momentum modifier — decomp-verified in `bmhero src/code/69AA0.c`:
speed 35 / pitch 80° / facing only; kick = flat launch at speed 30), 4-bomb
spread on ≥2s hold, set via input bit 14 (works mid-air), **walk-in kick**
(run into any settled bomb; setter immune until stepped clear;
`BSTATE_SLIDING`; detonates on first contact — owner-recalled, verify in
A1). Cap 6 live bombs. TDD'd in `tests/test_bomb_mechanics.c`; design doc
§2 records mechanics, sources, and decomp anchors for A1 calibration.

**SDL debug viewer complete (2026-07-19).** `tools/viewer/` (floats OK there;
spec + post-playtest addendum in `docs/superpowers/specs/`): camera modes on
F1 — FOLLOW default (fixed yaw; **verified: Hero's real camera never rotates
with facing**, see design doc §7 note), CHASE, ORBIT (battle-mode preview),
TOP — pause/step/slow-mo, HUD with live hash, checkerboard ground, translucent
walls, F2 sudden-death toggle (viewer-side; sim untouched), `--frames N`
deterministic smoke flag. Pure modules unit-tested (`tests/test_viewer_*.c`).
Keyboard playtested; **gamepad path written but not yet device-tested** (no
pad on hand). Toolchain: MSYS2 UCRT64 (gcc/CMake/Ninja/SDL3), README §Windows.

## Hard invariants — breaking any of these breaks netplay

1. **No floats in `src/arena/`.** Q20.12 fixed-point only (`arena_math.h`),
   int64 intermediates. This is what makes cross-arch online play safe.
2. **Sim reads nothing outside** `ArenaState`, the tick's inputs, and
   `static const` data. No time, no globals, no allocation.
3. **Fixed iteration orders** (players 0..3, bombs 0..15, pairs 01,02,03,12,13,23)
   and fixed tick-phase order (see header comment in `arena_sim.c`).
4. **`ArenaState` layout changes = netcode version bump** — static_asserts
   enforce sizes; bump `TUNE_VERSION` and note it.
5. **Padding stays zeroed** (memset at init, whole-struct copies only) — the
   FNV hash covers all bytes.

Run `gcc -std=c11 -Wall -Wextra -O2 -o t src/arena/arena_sim.c
tests/test_determinism.c && ./t` after every sim change. If the pinned CI hash
changes, that must be an intentional gameplay change.

## Next milestones (in order; docs §9 of arena design)

1. **A1 render bridge + feel** — *A1.0 done (fork builds + boots); A1.1a done
   (arena submodule ticks natively in the recomp); A1.1b done (Battle menu
   entry + battle-mode flag); A1.1b-ii done (Battle warps into
   MAP_BATTLE_ROOM); A1.1c dropped (room already clean; frontend-skip deferred
   as fragile/low-ROI).* Remaining:
   **A1.2 render bridge** — A1.2a done (player puppeted from sim); A1.2b done
   (spawn+puppet all 4 — bomb placeholders, clean flat arena); A1.2c done
   (bombs render + set/kick wired; blasts render as pops — real explosion
   visual deferred, needs effect-asset RE, §8.9) → *real bomber mesh
   (skeletal, §8.5b)* → anim + **camera-relative input** (fix forward/back
   feel via `gView`) + HUD. Render writes into `gObjects` entries from
   `ArenaState` (Q20.12 → Hero coords).
   **Side task — arena-shell eval: DONE.** Warped to a Nitros boss room
   (`MAP_NITROS_1`) as the flat arena — direct-warp loads clean, boss suppressed
   via the pre-update sweep; `ARENA_WARP_MAP`=15.
   **A1.3** transcribe every `TODO(feel)` constant in `arena_tuning.h` from the
   decomp (https://github.com/Bomberhackers/bmhero — start at `gPlayerObject`
   usage and the object update dispatch over `gObjects[207]`; Hack64 wiki has
   supplementary RE notes; throw/spread/kick constants already extracted). All
   constants go only in `arena_tuning.h`.
2. **A3 online hardening** (ROM-free): rendezvous server + lobby codes,
   host-relay fallback via a custom GekkoNet adapter, 4P mesh WAN soak,
   desync surfacing UI, automatic player-slot assignment (arena doc §6).

## Repo plan

This repo stays **canonical and standalone** (sim/netcode/viewer keep their
own CI, tests, fast iteration). The fork `dcmshi/BMHeroRecomp` consumes it as
a **git submodule** from A1.1 onward (decision 2026-07-19, superseding the
earlier "fold src/arena into the fork" note) — the fork adds only the
integration layer (patches, render bridge, Battle menu). Builds standalone
with the one-line gcc command or CMake. The recomp is GPL-3.0 — all code here
ships GPL-compatible.

## Known intentional simplifications (v1)

No items/powerups (v2, appends ArenaItem[16] to state), no pits in arena 0,
sudden-death = wall shrink only, host migration deferred, tuning values are
placeholders pending A1 feel-matching.
