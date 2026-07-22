# A1.2e Camera-Relative Stick Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stick-up moves the bomberman directly away from the camera (measured from `gView` every frame), killing the A1.2a "compressed" forward/back — spec `docs/superpowers/specs/2026-07-22-a1-2e-camera-relative-input-design.md`.

**Architecture:** Patch-only change in `arena_render_routine`'s battle block: phase 1 logs the camera's real geometry (hold-L forensics) and gates on sane data; phase 2 rotates the stick by the camera's normalized ground-plane forward with a stateless identity fallback and a vector clamp; phase 3 removes the forensics and documents the measured numbers. No native, syms.ld, or sim changes.

**Tech Stack:** LLVM-15 MIPS patch toolchain (floats legal in-patch), `gView` (named symbol, `struct View` — **`.at` @0x00, `.eye` @0x0C, at-first ordering**), libultra `sqrtf` intrinsic (`PR/gu.h:195`).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, **new branch `feature/a1.2e-camera-input`** off `feature/a1.2d-bomber-mesh`. Only `patches/arena_render.c` changes (+ docs in `bmhero-arena`). **Sim NOT modified** (pinned hash `4b6687d4`).
- Patches STATELESS; buttons (jump/bomb/set) and neutral-input handling untouched.
- **The sqrt-libcall trap:** the patch link uses `--unresolved-symbols=ignore-all`, so if clang emits a CALL to `sqrtf` instead of the MIPS `sqrt.s` instruction, it links silently and jumps to address 0 at runtime. After EVERY build that touches the float code, run **[nmcheck]**; a `U sqrtf` line = STOP, switch the call to `__builtin_sqrtf` and re-verify.
- **[build-rwdi]** (iterate on the symbolized build; MANDATORY `make -C patches clean` first):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; make.exe -C patches clean; cmake --build build-rwdi --target BMHeroRecompiled
  ```
  **[build-release]** = same but `--build build-cmake` (run once at the end; kill any running `BMHeroRecompiled` first — a hung/running game locks the exe).
- **[nmcheck]**:
  ```
  C:\Users\dshi\GitRepos\.tools\llvm15\bin\llvm-nm.exe C:\Users\dshi\GitRepos\BMHeroRecomp\patches\arena_render.o | Select-String -Pattern 'sqrt'
  ```
  Expected: **no output** (sqrt lowered to the instruction). `U sqrtf` = STOP per above.
- **[launch]** = human runs `playrwdi.bat`, launcher → Battle → Enter; agent verifies via `capture-game.ps1` + `arena_bridge.log` (§8.8 loop; hangs → attach `cdb -p`, not dump-wait).
- **[floatdecode]** (agent-side, turns dbg-tag bit patterns back into floats):
  ```
  [BitConverter]::ToSingle([BitConverter]::GetBytes([Convert]::ToUInt32("HEXVALUE",16)),0)
  ```

---

### Task 1: Branch + phase-1 camera forensics (measure, don't guess)

**Files:**
- Modify: `patches/arena_render.c` (battle block, before the input scaling)

**Interfaces:**
- Consumes: `gView` (named extern, `variables.h:609`), `gActiveContButton` + `CONT_L` (L trigger = E key — unused by the arena), existing `DECLARE_FUNC arena_export_dbg_u32`.
- Produces: measured arena camera geometry (eye/at/fwd) → the **PERP-SIGN decision** and the **sanity verdict** Task 2 consumes; numbers recorded for §8.11 (Task 3).

- [ ] **Step 1: Create the branch**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout feature/a1.2d-bomber-mesh && git status   # expect clean
git checkout -b feature/a1.2e-camera-input
```

- [ ] **Step 2: Add the forensics block** — in `arena_render_routine`, inside the battle block, immediately BEFORE the `s32 sx = ...` input scaling:
```c
        /* A1.2e phase-1 (TEMPORARY, removed in cleanup): camera forensics.
         * Logged only while L (E key) is held so the human samples exactly
         * where it matters. Floats logged as bit patterns. tags:
         * 70 eye.x  71 eye.z  72 at.x  73 at.z  74 fwd.x  75 fwd.z (normd) */
        if (gActiveContButton & CONT_L) {
            union { f32 f; u32 u; } v;
            f32 cfx = gView.at.x - gView.eye.x;
            f32 cfz = gView.at.z - gView.eye.z;
            f32 cl2 = cfx * cfx + cfz * cfz;
            v.f = gView.eye.x; arena_export_dbg_u32(70, v.u);
            v.f = gView.eye.z; arena_export_dbg_u32(71, v.u);
            v.f = gView.at.x;  arena_export_dbg_u32(72, v.u);
            v.f = gView.at.z;  arena_export_dbg_u32(73, v.u);
            if (cl2 > 0.0001f) {
                f32 cinv = 1.0f / sqrtf(cl2);
                v.f = cfx * cinv; arena_export_dbg_u32(74, v.u);
                v.f = cfz * cinv; arena_export_dbg_u32(75, v.u);
            }
        }
```
(`sqrtf` comes from `PR/gu.h` via `ultra64.h`; if the compiler complains it is undeclared, add `extern f32 sqrtf(f32);` next to the other externs instead of new includes.)

- [ ] **Step 3: Build + nmcheck** — run **[build-rwdi]**, then **[nmcheck]** (expect no output; `U sqrtf` = STOP → replace `sqrtf(` with `__builtin_sqrtf(` and repeat).

- [ ] **Step 4: Human measurement boot** — **[launch]**; instructions to the human: *walk a slow square around the arena (up, right, down, left edges); at each corner STOP and hold E for ~1 second; also hold E once mid-screen while standing still.* Agent then decodes tags 70–75 with **[floatdecode]** and answers, in writing:
  1. Sanity: eye ≠ at; coords within the arena's range (compare `[capture]`/`[simpos]` lines) — **if garbage: STOP (spec §3), re-plan; do not build Task 2 on it.**
  2. Camera ground-plane forward (fwd.x, fwd.z) at each sample → derived yaw (`atan2(fwd.x, fwd.z)` agent-side).
  3. Yaw drift across the square (constant vs follow-rotating).
  4. **PERP-SIGN decision:** with `right = (-fwd.z, +fwd.x)`, does world = stickRight·right move the player screen-right given the measured fwd? (Sanity-check on paper against one sample; the identity case fwd=(0,−1) must give right=(1,0).)

- [ ] **Step 5: Commit the spike**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "spike(A1.2e): hold-L camera forensics - gView eye/at/fwd tags 70-75"
```

---

### Task 2: Camera-relative mapping (patch-only)

**Files:**
- Modify: `patches/arena_render.c` (the `sx`/`sy` input scaling block)

**Interfaces:**
- Consumes: Task 1's PERP-SIGN decision + sanity verdict; existing `arena_export_tick_input(sx, sy, buttons)` (unchanged signature).
- Produces: the shipped mapping. Convention notes for reviewers: **sim stick-up = −Z**; identity fallback must reproduce the old behavior exactly.

- [ ] **Step 1: Replace the raw scaling** — swap the current block:
```c
        s32 sx = (s32)(gActiveContStickX * (31.0f / 80.0f));
        s32 sy = (s32)(gActiveContStickY * (31.0f / 80.0f));
```
with (perp sign below assumes Task 1 confirmed `right = (−fz, +fx)`; if the measurement said otherwise, flip BOTH `(-cfz)` and `(cfx)` signs and note it in the comment):
```c
        /* A1.2e: camera-relative stick (spec 2026-07-22). World move dir =
         * stickUp * cameraFwd + stickRight * cameraRight, where cameraFwd is
         * gView's ground-plane forward (at - eye, normalized) re-read every
         * frame. right = (-fwd.z, +fwd.x) per the phase-1 measurement.
         * Identity check: fwd=(0,-1) (camera looking down -Z, the old implicit
         * assumption) gives right=(1,0) => exactly the old mapping.
         * Degenerate camera frame (len^2 < 1e-4) -> identity (stateless).
         * Vector-clamped to unit before scaling so diagonals don't distort.
         * Sim convention: stick-up = -Z  =>  sim_y = -world_z. */
        f32 ux = (f32)gActiveContStickX * (1.0f / 80.0f);
        f32 uy = (f32)gActiveContStickY * (1.0f / 80.0f);
        f32 wx, wz;
        {
            f32 cfx = gView.at.x - gView.eye.x;
            f32 cfz = gView.at.z - gView.eye.z;
            f32 cl2 = cfx * cfx + cfz * cfz;
            if (cl2 > 0.0001f) {
                f32 cinv = 1.0f / sqrtf(cl2);
                cfx *= cinv; cfz *= cinv;
                wx = uy * cfx + ux * (-cfz);
                wz = uy * cfz + ux * ( cfx);
            } else {
                wx = ux; wz = -uy;  /* identity: old mapping (sim_y = -wz below
                                     * must yield +uy, hence the pre-negation) */
            }
        }
        {
            f32 m2 = wx * wx + wz * wz;
            if (m2 > 1.0f) { f32 minv = 1.0f / sqrtf(m2); wx *= minv; wz *= minv; }
        }
        s32 sx = (s32)(wx * 31.0f);
        s32 sy = (s32)(-wz * 31.0f);
```
Keep the existing per-axis `if (sx > 31)...` clamps AFTER this (belt-and-braces), and everything from `s32 jump = ...` on unchanged.

**Why the identity branch pre-negates:** old mapping was `sim = (stickX, stickY)·31/80`, and this code derives `sim_y = −wz` — so the fallback must hand over `wz = −uy` to reproduce it exactly. The rotated path needs no extra negation: a camera behind the player looking down −Z has fwd=(0,−1), giving `wz = uy·(−1) = −uy` → `sim_y = uy` ✓ — the two paths agree by construction.

- [ ] **Step 2: Build + nmcheck** — **[build-rwdi]**, then **[nmcheck]** (no output expected).

- [ ] **Step 3: Human feel gate** — **[launch]**; protocol: *without moving the camera intentionally, walk each cardinal (up/down/left/right on the stick) ~2s each, then the four diagonals, then hold E at two corners (forensics still active) for a correlated sample; then throw (hold+release LShift) while running, set (Q), walk-in kick.* Verify with the human + capture:
  1. stick-up = player recedes straight from camera; stick-down = approaches; left/right lateral. **If left/right are MIRRORED:** flip the two perp signs (one-line edit), rebuild once, re-gate — anticipated correction, not a failure.
  2. Diagonals feel same-speed as cardinals (vector clamp working).
  3. The forward/back "compression" is gone (compare against A1.2a's note).
  4. Bombs/blasts/set/kick regress clean.
  Agent cross-checks: decode a hold-E sample; confirm the world direction the sim received (`[simpos]` p0 deltas) matches `stickUp·fwd` for that sample.

- [ ] **Step 4: Commit**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(A1.2e): camera-relative stick via gView - vector-clamped, stateless identity fallback"
```

---

### Task 3: Cleanup, soak, docs

**Files:**
- Modify: `patches/arena_render.c` (remove forensics), `bmhero-arena` `docs/bmhero-recomp-integration-notes.md` (§8.11), `bmhero-arena` `CLAUDE.md`

- [ ] **Step 1: Remove the phase-1 forensics block** (the whole `if (gActiveContButton & CONT_L) {...}` from Task 1) — the mapping itself stays fully commented.

- [ ] **Step 2: Build both** — **[build-rwdi]** AND **[build-release]** (kill any running game first), **[nmcheck]** once more.

- [ ] **Step 3: Final soak boot** — **[launch]** (either exe): one clean entry + 60s of play touching every mechanic. Cumulative clean-boot count across Tasks 1–3 must be ≥3 (spec §1); if any boot crashed/hung during this plan, diagnose per §8.8 before closing.

- [ ] **Step 4: Push fork branch**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "chore(A1.2e): remove phase-1 camera forensics"
git push -u origin feature/a1.2e-camera-input
```

- [ ] **Step 5: Docs (bmhero-arena)** — add **§8.11 Camera-relative input (A1.2e)** to `docs/bmhero-recomp-integration-notes.md`: `gView` semantics (**`.at` @0x00 BEFORE `.eye` @0x0C** — easy to swap; `rot`/`up`/`dist` fields exist), the measured arena camera numbers from Task 1 (eye/at sample, derived yaw, drift verdict), the perp sign shipped, the sqrt-intrinsic + [nmcheck] ritual, and the mapping formula with the sim's stick-up=−Z convention. Update `CLAUDE.md`: A1.2e complete status block + milestone line (next: **A1.3 feel constants — spec immediately**, per the agreed sequencing).

- [ ] **Step 6: Commit + push**
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md docs/bmhero-recomp-integration-notes.md
git commit -m "docs: A1.2e complete - camera-relative input; §8.11 gView reference"
git push
```

---

## Plan self-review notes

- **Spec coverage:** §3 measurement + validate-before-trust STOP → Task 1 (step 4.1); §4 mapping items 1–6 → Task 2 step 1 (degenerate fallback, vector clamp, unchanged buttons/pack); §5 verify/cleanup/§8.11/CLAUDE.md → Tasks 2–3; §6 non-goals untouched (no camera-behavior work, no sim edits); §7 build/branch → Global Constraints.
- **Placeholder scan:** the perp sign is the plan's one measured unknown — Task 2 states the assumed sign, the flip procedure, and the identity-case check; the identity-fallback negation subtlety is called out explicitly with the corrected line (`wz = -uy`). No TBDs.
- **Type consistency:** `sqrtf(f32)` per `PR/gu.h:195`; `gView.at`/`gView.eye` per `types.h:257` (at-first noted twice); tags 70–75 unique vs. existing 40–63; `CONT_L` follows the `CONT_*` family already used in the file (`CONT_A/B/G`); `arena_export_tick_input(sx, sy, buttons)` signature unchanged.
