# A1.2d Real Bomber Mesh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Players 1–3's puppet actors render as real bombers (`gFileArray[1]` multi-part skeletal model) in a neutral idle pose, facing their sim heading — replacing the bomb placeholders (spec: `docs/superpowers/specs/2026-07-22-a1-2d-real-bomber-mesh-design.md`).

**Architecture:** Decomp-first RE pins the game's own bomber-assembly recipe (part loop + binds + pose), then a one-puppet spike proves it draws (symbolized-dump loop on white screens, pool pressure measured), then rollout to all three puppets. Facing is already plumbed (`Rot.y = arena_export_puppet_yaw(i)`, degrees) — it becomes visible once the mesh is asymmetric; this slice only verifies it. Sweep safety is made topology-independent by a new native `arena_mark_actor_slot`.

**Tech Stack:** VS 2022 clang-cl (native), LLVM-15 MIPS (patches), the A1.0 three-toolchain recipe (memory `recomp-build-toolchain`); decomp reads in `lib/bmhero` (read-only).

## Global Constraints

- Fork `C:\Users\dshi\GitRepos\BMHeroRecomp`, **new branch `feature/a1.2d-bomber-mesh`** off `feature/a1.2b-spawn-bombers`. `bmhero-arena` gets only doc updates; **sim NOT modified** (pinned hash `4b6687d4`).
- **Patches STATELESS** (mutable patch statics abort `0xC0000409`); auto-named `D_*` DATA symbols do NOT resolve via the patch reloc path → pass/read their **address as a literal** (e.g. `((s32*)0x80134794)`); native-export ABI: max 4 args, 32-bit ints only, floats as u32 bit patterns (function symbols like `func_8001BD44` resolve fine).
- Verified facts (plan-time greps, 2026-07-22):
  - `func_8001BD44(s32 objId, s32 arg1, s32 arg2, s32 srcPtr)` defined `boot/17930.c:1144` — first arg is an objId. `code/70C40.c:23-35` loads **two parts onto ONE objId** with arg1 = 0 then 1 → arg1 = part index (leading hypothesis).
  - Demo player assembly `code/4DFF0.c:424-431`: `for i in 0..7: func_8001BD44(i, 0, D_80134794->unk34[i], gFileArray[1].ptr + D_80134794->unk14[i]); func_8001BE6C(i,0,0,0); func_8001B754(0,0); gObjects[i].actionState = 1;` — one object **per part** (alternate hypothesis), demo context.
  - Unexplored `gFileArray[1]` loads: `boot/2BF00.c:588` (`func_8001BD44(sp40, sp3C, sp38, gFileArray[1].ptr)` — variable args, candidate in-level player assembly), `overlays/13AC20/13AC20.c:422` (`(0, 0, 0x13, gFileArray[1].ptr)` — the §8.6 cfg-0x13 reference).
  - Facing: `arena_puppet_yaw(i)` returns `yaw * 360/65536` **degrees**; the patch already writes it to `gObjects[slot].Rot.y` every frame (`arena_render.c:204`), same convention as player 0 (`Rot.y`, visually verified since A1.2a). No sim or units work expected.
  - Existing exports end at `arena_export_blastactor_get_slot = 0x8F0001BC`; next free `0x8F0001C0`.
  - Current puppet spawn (to be replaced for i=1..3): `arena_render.c:136-148` — `func_80027464` with `info.unk4 = 9` (bomb mesh) + `func_8001ABF4(slot, 0, 0, D_801163DC_ADDR)`.
  - The old "pool ceiling 6–8 actors" finding is **INVALID** (was the load-crash race, §8.9) — headroom is unknown, measured in Task 4.
- **[build]** (MANDATORY `make -C patches clean` after any `patches/*.c` edit):
  ```
  $vs="C:\Program Files\Microsoft Visual Studio\2022\Community"; Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"; Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64" | Out-Null; $env:Path="C:\Users\dshi\GitRepos\.tools\llvm15\bin;"+$env:Path+";C:\msys64\ucrt64\bin;C:\msys64\usr\bin"; cd C:\Users\dshi\GitRepos\BMHeroRecomp; make.exe -C patches clean; cmake --build build-cmake --target BMHeroRecompiled
  ```
- **[launch]** = human runs `play.bat`, arrow→Battle→Enter, Start + A×3 into the arena; agent verifies via scratchpad `capture-game.ps1` + `arena_bridge.log`.
- **[dump]** (white screens / crashes) = relaunch via `playrwdi.bat`, reproduce, then:
  ```
  cdb -z <newest %LOCALAPPDATA%\CrashDumps\*.dmp> -y "srv*C:\sym*https://msdl.microsoft.com/download/symbols;C:\Users\dshi\GitRepos\BMHeroRecomp\build-rwdi" -c ".ecxr; kc 20; q"
  ```

---

### Task 1: Branch + RE — bomber assembly topology (spec §3 Q1–Q3)

**Files:**
- Read-only: `lib/bmhero/src/boot/17930.c`, `lib/bmhero/src/boot/2BF00.c`, `lib/bmhero/src/code/4DFF0.c`, headers defining `D_80134794`'s type
- Modify (bmhero-arena): `docs/bmhero-recomp-integration-notes.md` (§8.5b, new "A1.2d findings" subsection — draft, finished in Task 2)

**Interfaces:**
- Produces: the **RECIPE decision** the spike (Task 4) executes — either **(A) one object, parts via arg1** or **(B) one object per part** — plus: the in-level descriptor address + field offsets (part count, per-part cfg + data offset), and what `func_8001BE6C` / `func_8001B754` do.

- [ ] **Step 1: Create the branch**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git checkout feature/a1.2b-spawn-bombers && git status   # expect clean
git checkout -b feature/a1.2d-bomber-mesh
```

- [ ] **Step 2: Pin `func_8001BD44` arg semantics** — read its body at `boot/17930.c:1144`. Answer: what does arg1 index (a per-object part array in `Unk140`? which field bounds it)? What is arg2 (a model-config index — the `0x13` from §8.6)? Confirm the 70C40 two-part pattern is "parts in one object".

- [ ] **Step 3: Chase the in-level player assembly** — read `boot/2BF00.c` around line 588: what are `sp40/sp3C/sp38` (fixed values? loop variables?), is this inside a loop over parts, and what descriptor feeds it? Grep for its enclosing function's callers to confirm it runs on level entry (not demo). If 2BF00.c is a dead end, fall back to mapping `code/4DFF0.c:400-440` + the `D_80134794` typedef (grep `D_80134794` in `lib/bmhero/include` + `src`) and note the demo caveat explicitly.

- [ ] **Step 4: Identify `func_8001BE6C` + `func_8001B754`** — find their bodies (grep `lib/bmhero/src/boot`); answer whether they are per-part attach/skeleton wiring or pose/anim-frame setup, and whether the spike must call them per part, once per object, or not at all.

- [ ] **Step 5: Approach-B check** — is there a single function wrapping the whole part loop (the enclosing function of Step 3's site) that is free of `gPlayerObject`/demo state and callable with (objId/slot, model base)? If YES, the Task 4 recipe becomes "call that function"; record its symbol + signature.

- [ ] **Step 6: Record findings** — append an "A1.2d findings (Task 1: topology)" subsection to §8.5b in `docs/bmhero-recomp-integration-notes.md`: the RECIPE decision (A or B), descriptor address + layout, the two helpers' roles, approach-B verdict. State every address as a literal (they'll be used as literals in the patch).

- [ ] **Step 7: Commit (bmhero-arena)**
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add docs/bmhero-recomp-integration-notes.md
git commit -m "docs(A1.2d): RE findings - bomber assembly topology"
```

---

### Task 2: RE — anim binds, idle pose, facing check (spec §3 Q4–Q6)

**Files:**
- Read-only: `lib/bmhero/src/boot/17930.c` (or wherever `func_8001ABF4` lives — grep), `lib/bmhero/src/code/4DFF0.c:136-215` (demo anim-texture calls), headers/data defining `D_80101E8C`
- Modify (bmhero-arena): `docs/bmhero-recomp-integration-notes.md` (§8.5b findings, part 2)

**Interfaces:**
- Consumes: Task 1's RECIPE decision (whether binds are per part or per object).
- Produces: the **BIND+POSE recipe** for Task 4 — `func_8001ABF4` true arg semantics, which anim config(s) at which literal address(es) give a neutral idle, and how many binds per bomber.

- [ ] **Step 1: Pin `func_8001ABF4` arg semantics** — read its body. The demo calls `(0, partIdx?, 0, &D_80101E8C[n])` (varying arg1) while A1.2b used `(slot, 0, 0, cfg)` and it worked for a 1-part bomb. Answer: is arg0 an objId and arg1 a part/channel index (consistent with both)? What pool entry (`Unk148`/`D_8016C298`) does each call bind?

- [ ] **Step 2: Map the bomber anim-config family** — find `D_80101E8C`'s data (element size, count) and any *sibling* configs used for the standing player (the credit-scene family `D_80101758` at `4DFF0.c:165-205` binds parts 0..3 — note which configs pair with which parts). Pick the candidate **idle** config(s) + literal address(es) for the spike; note 2 fallback candidates in case the first reads wrong on screen.

- [ ] **Step 3: Facing sanity (docs-only)** — confirm from Task 1's reading that nothing in the assembly overrides `Rot.y` per frame (if the game's player update writes it, our per-frame write wins anyway — it runs after `func_80024744()`; note where in the frame the puppet write happens: `arena_render.c:195-208`). Record: facing needs **no new code**, only visual verification in Task 5.

- [ ] **Step 4: Record findings + commit** — append "A1.2d findings (Task 2: binds/pose/facing)" to §8.5b:
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add docs/bmhero-recomp-integration-notes.md
git commit -m "docs(A1.2d): RE findings - anim binds, idle pose, facing"
```

---

### Task 3: Native — topology-independent sweep safety (`arena_mark_actor_slot`)

**Files (fork):**
- Modify: `src/arena_bridge/arena_bridge.cpp`, `src/arena_bridge/arena_bridge.h`, `src/arena_bridge/arena_bridge_export.cpp`, `src/main/main.cpp`, `patches/syms.ld`

**Interfaces:**
- Produces (EXPORT names): `arena_export_mark_actor_slot(slot)` — registers any extra gObjects slot as ours so the boss-suppression sweep spares it (covers part-objects if RECIPE B, harmless if RECIPE A). Existing `arena_export_is_actor_slot(slot)` starts honoring the marked list.

- [ ] **Step 1: Native state + fn in `arena_bridge.cpp`** — in the anonymous namespace (next to the slot tables):
```cpp
    int g_extra_slots[64];      /* A1.2d: extra actor slots (bomber part objects) */
    int g_extra_count = 0;
```
New fn after `arena_is_actor_slot`, and add the marked-list check inside `arena_is_actor_slot` (before its final `return 0`):
```cpp
extern "C" void arena_mark_actor_slot(int slot) {
    if (slot < 0) return;
    for (int k = 0; k < g_extra_count; k++) if (g_extra_slots[k] == slot) return;
    if (g_extra_count < 64) g_extra_slots[g_extra_count++] = slot;
}
```
```cpp
    /* inside arena_is_actor_slot, before return 0: */
    for (int k = 0; k < g_extra_count; k++) if (g_extra_slots[k] == slot) return 1;
```

- [ ] **Step 2: Declaration in `arena_bridge.h`** (inside the `extern "C"` block, after `arena_is_actor_slot`):
```c
void  arena_mark_actor_slot(int slot);   /* A1.2d: sweep-spare an extra slot (bomber parts) */
```

- [ ] **Step 3: Shim in `arena_bridge_export.cpp`** (append):
```cpp
/* A1.2d: extra actor slots (bomber part objects). */
extern "C" void arena_export_mark_actor_slot(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; arena_mark_actor_slot(_arg<0, int>(rdram, ctx));
}
```

- [ ] **Step 4: Register in `main.cpp`** — fwd-decl next to the other `arena_export_*`:
```cpp
extern "C" void arena_export_mark_actor_slot(uint8_t* rdram, recomp_context* ctx);
```
and in the `REGISTER_FUNC` block: `REGISTER_FUNC(arena_export_mark_actor_slot);`

- [ ] **Step 5: `syms.ld`** — after `arena_export_blastactor_get_slot = 0x8F0001BC;`:
```
arena_export_mark_actor_slot = 0x8F0001C0;
```

- [ ] **Step 6: Build** — run **[build]**; expect clean link (new sym unreferenced by the patch yet — harmless).

- [ ] **Step 7: Commit (fork)**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add src/arena_bridge/ src/main/main.cpp patches/syms.ld
git commit -m "feat(arena): mark-actor-slot export - sweep safety for bomber parts (A1.2d)"
```

---

### Task 4: One-puppet spike — player 1 becomes a bomber (decision gate)

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 1 RECIPE + descriptor literals, Task 2 BIND+POSE literals, Task 3 `arena_export_mark_actor_slot`, existing `arena_export_dbg_u32`.
- Produces: a drawing skeletal bomber for player 1 (players 2–3 stay bombs this task) + measured pool cost; or an explicit STOP at the decision gate.

- [ ] **Step 1: Add imports** — after the existing `DECLARE_FUNC`s in `arena_render.c`:
```c
/* A1.2d: real bomber mesh */
DECLARE_FUNC(void, arena_export_mark_actor_slot, s32 slot);
extern void func_8001BE6C(s32 arg0, s32 arg1, s32 arg2, s32 arg3);  /* role: per Task 1 findings */
extern void func_8001B754(s32 arg0, s32 arg1);                      /* role: per Task 1 findings */
```
(`func_8001BD44` + `func_8001ABF4` are already imported/declared.)

- [ ] **Step 2: Add the descriptor literals** — next to `D_801163DC_ADDR`, **filled from the §8.5b findings** (values below are the demo-descriptor defaults; replace with Task 1's in-level values if they differ):
```c
/* A1.2d bomber assembly (literals per the data-symbol rule; source: §8.5b findings).
 * Descriptor layout per 4DFF0.c:424: s32 unk14[8] part data offsets at +0x14,
 * s32 unk34[8] part cfgs at +0x34, -1 = part absent. NOTE: D_80134794
 * (0x80134794) is a POINTER to the DEMO descriptor and is likely null/stale
 * in-level — the FILL below must be Task 1's in-level descriptor source
 * (either a static descriptor address used directly, or a pointer variable
 * double-deref'd like the demo default shown). */
#define BOMBER_DESC_BASE    (*(volatile s32*)0x80134794)        /* <- FILL: findings (in-level descriptor) */
#define BOMBER_DESC_OFF(p)  (((volatile s32*)(BOMBER_DESC_BASE + 0x14))[p])
#define BOMBER_DESC_CFG(p)  (((volatile s32*)(BOMBER_DESC_BASE + 0x34))[p])
#define BOMBER_PART_MAX     8                                   /* <- FILL: findings part count   */
#define BOMBER_IDLE_ANIM    ((struct UnkStruct8016C298_1*)0x80101E8C)  /* <- FILL: findings idle  */
```

- [ ] **Step 3: Replace player 1's spawn** — in the `for (i = 1; i < 4; i++)` spawn block (`arena_render.c:136-148`), special-case `i == 1` with the RECIPE (leading shape A — one object, parts via arg1; if Task 1 chose B, spawn one `func_80027464` object per present part, mark each via `arena_export_mark_actor_slot`, record the ROOT slot in the puppet table, and apply the same marker discipline). Shape-A code:
```c
                if (i == 1) {   /* A1.2d spike: player 1 = real bomber */
                    s32 slot = func_80027464(1, &info,
                                             gPlayerObject->Pos.x,
                                             gPlayerObject->Pos.y,
                                             gPlayerObject->Pos.z, 0.0f);
                    arena_export_dbg_u32(40, (u32)slot);            /* spawned root */
                    if (slot >= 0) {
                        s32 p;
                        for (p = 0; p < BOMBER_PART_MAX; p++) {
                            s32 off = BOMBER_DESC_OFF(p);
                            s32 cfg = BOMBER_DESC_CFG(p);
                            if (off != -1 && cfg != -1) {
                                func_8001BD44(slot, p, cfg, (s32)gFileArray[1].ptr + off);
                                arena_export_dbg_u32(41, (u32)((p << 16) | (cfg & 0xFFFF)));
                            }
                        }
                        /* binds + pose per Task 2 findings (count/args may differ): */
                        func_8001ABF4(slot, 0, 0, BOMBER_IDLE_ANIM);
                        arena_export_dbg_u32(42, 0);
                        arena_export_mark_actor_slot(slot);
                    }
                    arena_export_puppet_set_slot(i, slot);
                } else {
                    /* existing bomb-placeholder recipe, unchanged */
```
Marker discipline is the contract: **tag 40** = root slot, **tag 41** = each part load (part<<16|cfg), **tag 42** = each bind, so the log names the last step reached before any abort.

- [ ] **Step 4: Build** — run **[build]**.

- [ ] **Step 5: Human boot gate (the spike)** — **[launch]**; agent captures + reads `arena_bridge.log`. Wanted: player 1's puppet is a bomber standing idle (players 2–3 still bombs); it moves under sim control. **White screen / crash:** run **[dump]**, match the aborting frame against the tag-40/41/42 trail, fix (wrong cfg/offset/bind count are the likely culprits — try Task 2's fallback anim candidates before re-deriving), rebuild, repeat. Budget ~3 fix cycles.

- [ ] **Step 6: Measure pool cost** — from the same boot's log: number of tag-41 part loads + tag-42 binds for one bomber (= model/anim-pool cost), plus total actor slots consumed (root + any part objects). Append the numbers to §8.5b findings (bmhero-arena) — this replaces the invalidated ceiling data point.

- [ ] **Step 7: DECISION GATE** — bomber draws and is stable → proceed to Task 5. Not stabilizable within the fix budget → **STOP; do not grind.** Options per spec §4: single-part stand-in mesh, or keep bombs; either way re-plan with the user before writing more code.

- [ ] **Step 8: Commit (fork + findings)**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(A1.2d): player-1 puppet renders the skeletal bomber (spike)"
cd /c/Users/dshi/GitRepos/bmhero-arena
git add docs/bmhero-recomp-integration-notes.md
git commit -m "docs(A1.2d): pool-cost measurement (one skeletal bomber)"
```

---

### Task 5: Rollout — players 2–3 + facing verify + regression

**Files (fork):** Modify `patches/arena_render.c`

**Interfaces:**
- Consumes: Task 4's proven recipe (exactly as committed — no re-derivation).

- [ ] **Step 1: Convert players 2–3** — remove the `i == 1` special case so all of `i = 1..3` run the bomber recipe (delete the bomb-placeholder else-branch for puppets; bombs/blast actors keep the bomb mesh). Delete the now-dead §8.5b "deferred" comment block at `arena_render.c:63-67`.

- [ ] **Step 2: Build** — run **[build]**.

- [ ] **Step 3: Human boot gate (exit criterion)** — **[launch]**, then a full match loop: move, throw (hold+release LShift), set (Q), kick (walk into a settled bomb), detonations. Agent captures + reads the log. Verify ALL of:
  1. 3 idle bombers at the sim corners (screenshot);
  2. facing tracks movement — puppets 1–3 run the sim's idle input, so verify via player 0's own bomber body first (it shares the yaw convention), then confirm the puppets' standing orientation is sane; if yaw reads 90°/180° off, add a constant offset at the `Rot.y` write and rebuild once;
  3. bombs + blast pops still render (regression);
  4. no white screen / crash.

- [ ] **Step 4: Stability soak** — repeat **[launch]** until ≥5 cumulative clean arena entries on this build (per spec §1); log any failure with **[dump]**.

- [ ] **Step 5: Commit + push (fork)**
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp
git add patches/arena_render.c
git commit -m "feat(A1.2d): all 3 puppets render skeletal bombers + facing verified"
git push -u origin feature/a1.2d-bomber-mesh
```

---

### Task 6: Docs close-out

**Files (bmhero-arena):** Modify `docs/bmhero-recomp-integration-notes.md` (§8.5b), `CLAUDE.md`

- [ ] **Step 1: Rewrite §8.5b** — from "deferred follow-up" to the shipped recipe: RECIPE shape (A/B), descriptor + anim literals, bind counts, pool cost, the helpers' roles, and any dead ends hit during the spike (so they're not retried).

- [ ] **Step 2: Update `CLAUDE.md`** — new "A1.2d complete" status block (what shipped, branch, key findings, follow-ups: walk anim, per-player colors); update the milestone line (next: anim + camera-relative input + HUD).

- [ ] **Step 3: Commit + push**
```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md docs/bmhero-recomp-integration-notes.md
git commit -m "docs: A1.2d complete - real bomber mesh on all 3 puppets"
git push
```

---

## Plan self-review notes

- **Spec coverage:** §3 Q1–Q3 → Task 1; Q4–Q6 → Task 2; §4 spike + pool measurement + decision gate → Task 4 (steps 5–7); §5 rollout + facing + sweep exemption → Task 5 + Task 3 (`arena_mark_actor_slot` makes sweep safety independent of the RECIPE A/B outcome); §6 constraints → Global Constraints; §7 non-goals untouched; §8 build/testing/docs → [build]/[launch]/[dump] + Task 6.
- **Placeholder scan:** the four `<- FILL` literals in Task 4 Step 2 are the plan's intentional unknowns — each names the exact findings entry that supplies it (the blast-plan precedent: `ARENA_EXPLOSION_ID`). RECIPE B's alternate shape is described at its decision point (Task 4 Step 3), not left implicit.
- **Type/name consistency:** native `arena_mark_actor_slot` / export `arena_export_mark_actor_slot` consistent across bridge/shim/main/syms/DECLARE_FUNC; address `0x8F0001C0` follows `0x8F0001BC`; dbg tags 40/41/42 stated once and referenced in the boot gate; `func_8001BD44`/`func_8001ABF4` signatures match existing imports in `arena_render.c:48-55`.
