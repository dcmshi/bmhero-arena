# Battle Arena Mode — Detailed Design

**Style:** Hero-style free 3D movement (run, jump, thrown bombs) in enclosed arenas
**Players:** up to 4 from day one, local couch and online rollback
**Companion doc:** `bmhero-multiplayer-architecture.md` (Domain B is this mode)
**Status:** design v0.1 — 2026-07-18

---

## 1. Design pillars

1. **It must feel like Bomberman Hero.** Analog run, jump, forward bomb throws with arcs, blast knockback — constants transcribed from the decomp's player/bomb code, not reinvented.
2. **Rollback is a property of the sim, not a feature bolted on.** Every rule below exists in a POD, fixed-point, allocation-free state struct. If a mechanic can't live there, it doesn't go in v1.
3. **The recompiled game is a dumb renderer.** The arena sim never reads from RDRAM; RDRAM is written *from* the sim once per rendered frame. One-way data flow is what keeps rollback trivial.

## 2. Ruleset (v1)

- **Match:** 2–4 players, last-bomber-standing rounds; first to 3 round wins. Round timer 2:00 → sudden death (arena walls close in / blast radius creeps up).
- **Health:** 2 hits per round (Hero-style heart) — a blast hit deals 1 damage + knockback; brief post-hit invulnerability (60 ticks). 0 HP = out for the round (becomes spectator-ghost).
- **Bombs:** press B to pull one out (can run/jump while holding); release =
  single-arc throw in facing direction, distance scaling with stick tilt at
  release (neutral stick = short lob). Hold ≥ ~2s to arm the **4-bomb
  spread** — a forward fan (±5°, ±15°) at a fixed shorter trajectory. A
  separate **set** button lays a bomb at the feet; pressing it with a
  settled bomb in front (any owner) **kicks** it — kicked bombs slide flat
  and **detonate on first contact** (wall, pillar, player, bomb) or on fuse
  expiry *(kick-vs-wall detonation owner-recalled; verify in A1)*. Thrown
  bombs bounce once, then sit with a fuse (~150 ticks); direct hit on a
  player detonates on impact. Max 6 live bombs per player. *(Mechanics
  verified 2026-07-19 vs GameFAQs/StrategyWiki/Bomberman Wiki — replaces
  the earlier invented charge-tier throw.)*
- **Blasts:** spherical, radius R for ~20 ticks; chain-detonate other bombs; blasts hurt the owner too.
- **Movement:** analog run (camera-relative), single jump, air control, blast knockback launches with brief tumble state; arena has pits/hazards in some layouts (fall = 1 damage + respawn on platform).
- **No powerups in v1.** Items (fire-up, bomb-up, speed, heart) are v2 — they slot into the state spec (§3) without layout changes.
- **Arenas:** small enclosed layouts built from static collision prisms + a visual set dressed with campaign level assets (e.g., a Planet Bomber garden ring, a Redial machine room). 3 layouts for v1.

Design tension to playtest early: free movement + thrown bombs means less "trapped in a corridor" than classic Bomberman. Sudden-death shrink, pit hazards, and bounce-detonation on direct hits are the pressure sources. If matches stall, v1.1 levers: shorter fuses, bigger radii, walls that close earlier.

## 3. ArenaState specification

**Implemented — `src/arena/arena_state.h` is authoritative**; layout locked by `static_assert`s (changing it is a netcode version bump). Summary:

All fixed-point **Q20.12** in `s32` (range ±524k units, 1/4096 precision — far beyond arena scale). Angles: `u16` binary angle (65536 = full turn). No pointers, no heap, fixed iteration order, explicit zeroed padding (the struct *is* the wire/snapshot format).

```c
typedef struct { s32 x, y, z; } Vec3q;            // 12B, Q20.12

typedef struct {                                   // 40B (asserted)
    Vec3q pos, vel;                                // 24B
    u16   yaw;  u8 state;  u8 hp;                  // IDLE/RUN/JUMP/TUMBLE/DEAD
    u16   timer;                                   // bomb-hold / tumble+invuln, per state
    u8    stocks_won; u8 held_bomb;                // held_bomb: bomb index+1, 0=none
    u8    live_bombs; u8 pad0;
    u16   last_input;                              // previous tick (edge detection)
    u32   anim_hint;                               // render-bridge hint, still deterministic
} ArenaPlayer;

typedef struct {                                   // 32B (asserted)
    Vec3q pos, vel;                                // FREE/HELD/AIRBORNE/SETTLED/EXPLODING
    u8 owner; u8 state; u16 fuse; u8 bounced; u8 pad0[3];
} ArenaBomb;

typedef struct {                                   // 16B (asserted)
    Vec3q center; u16 radius_t; u8 owner; u8 ttl;  // radius grows over first 12 ticks
} ArenaBlast;

typedef struct {                                   // 944B total (asserted)
    u32 tick; u32 rng;                             // xorshift32, only RNG source
    u8 round; u8 phase; u16 phase_timer;           // COUNTDOWN/PLAY/SUDDEN_DEATH/ROUND_END
    u8 arena_id; u8 num_players; u16 shrink_step;
    ArenaPlayer players[4]; ArenaBomb bombs[16]; ArenaBlast blasts[16];
} ArenaState;
```

944B ⇒ a full 8-frame rollback window of snapshots is ~7.5KB. Snapshot/restore is a single `memcpy`; GekkoNet's limited-saving option is unnecessary — save every frame. v2 items will append an `ArenaItem[16]` array (layout bump, planned).

**Static data (not in state, hashed into the version handshake):** arena collision set (AABBs + convex prisms + kill-plane), spawn points, shrink schedule, tuning table (§5). Identical on all peers by construction.

## 4. Simulation

**Tick rate:** 60Hz sim ticks (rollback granularity; Hero's campaign logic is ~30Hz but the arena sim is ours, and 60 halves rollback visual pops). Budget: full tick < 50µs so 8 re-sims cost < 0.5ms.

**Input word (per player, 16 bits):** stick X `s8` quantized ÷ stick Y `s8`? — no: pack as `{stick_x:s8-quantized-to-6b, stick_y:6b, jump:1, bomb:1, pad:2}` = 16 bits. Quantizing the analog stick to 6 bits/axis is essential: it bounds input entropy for prediction (repeat-last predicts analog poorly at full precision) and keeps input packets tiny. 4 players × 2B × 8-tick redundancy = 64B/packet.

**Fixed tick order (never reordered — this ordering is part of the determinism contract):**

1. Phase logic (countdown, timer, sudden-death shrink step).
2. For each player 0→3: input → state machine (run accel/friction, jump, charge/throw, tumble decay) → integrate velocity (gravity Q-constant) → collide vs. static prisms (slide), vs. kill-plane (pit death).
3. Player-vs-player circle pushout (fixed pair order 01,02,03,12,13,23).
4. For each bomb 0→15: held → follow owner anchor; airborne → integrate, bounce once (restitution constant), impact-vs-player check (detonate); settled → fuse decrement → detonate at 0.
5. Detonations spawn blasts (lowest free slot); blasts grow, chain-detonate bombs in radius, damage/knockback players (once per blast per player, tracked via invuln timer).
6. Deaths, round-end check, `rng` advances **only** via in-tick consumers (currently: sudden-death hazard placement; nothing else — movement/combat is RNG-free).

**Math discipline:** all products via `s64` intermediates then shift; one shared integer `sqrt`/`atan2` table; **no floats anywhere in the sim** — this is what makes cross-arch (x86_64↔ARM64) online play safe in arena mode regardless of the campaign's float-determinism findings.

## 5. Feel: transcribing Hero's constants

The decomp is the reference manual, not a dependency. Process: locate the player movement update in [bomberhackers/bmhero](https://github.com/Bomberhackers/bmhero) (start from `gPlayerObject` uses and the object-update dispatch over `gObjects[207]`), extract run speed, acceleration, jump impulse, gravity, throw arc velocity (plus spread fan angles/arm time and kick speed/range), blast radius/knockback — convert each float/fixed constant to Q20.12, and keep them in one `arena_tuning.h` table (versioned, hashed into the handshake). Where the decomp still has unmatched asm for a needed function, measure empirically in the recomp (record inputs, log `gPlayerObject` positions via a debug patch, fit constants). Tuning divergence from Hero is then a deliberate per-constant choice, not an accident.

## 6. Netcode integration (GekkoNet)

One `SyncSession` wrapper over [GekkoNet](https://github.com/HeatXD/GekkoNet), three configurations of the *same* loop:

- **Couch (local 1–4P):** GekkoNet local session, all players local, 0 delay. Same code path as online — permanent insurance against online-only bugs.
- **Online (2–4P):** online session, full-mesh UDP (4P mesh = 3 connections/peer; input packets are 64B at 60Hz ≈ 31kbps — trivial). Remote prediction: repeat-last-input; max prediction window 8 ticks; beyond that the sim stalls one frame rather than mispredicting further (tunable).
- **Stress (CI + dev):** GekkoNet stress session — continuously rolls back and re-sims over a check distance; run headless in CI over recorded 4P input scripts and fuzzed inputs, asserting state-hash equality. This is the arena's determinism gate (replaces the campaign's M1 harness for this mode).

Frame loop (per rendered frame): pump session events → feed local inputs → if `advance` events: run sim ticks; if `load`+`advance`: memcpy-restore, re-sim silently → render bridge writes *only* final present state. Desync detection: GekkoNet's built-in (full-frame saving keeps it available) + our own fnv1a(ArenaState) exchanged every 60 ticks with tick tag.

Mixed rounds (a player drops mid-match): GekkoNet event → mark player DEAD at a confirmed tick, continue 3-way; rejoin lands as spectator until next round.

**Hosted sessions (primary online model):** one player hosts; host = *session authority*, not input relay. The host runs the lobby, sets arena/rules, distributes RNG seed and version-hash gate, admits players, and is the connection target — guests connect directly (or hole-punch) to the host, and the host brokers guest↔guest address exchange. The rollback input exchange itself stays **full mesh** wherever NAT allows (star routing would double latency between non-host guests: A→host→B); host-relayed inputs are the per-pair fallback when punching fails. Friends-hosting removes the need for a public rendezvous server in v1 — the host fills that role; a rendezvous/relay service remains an optional later add for strangers/matchmaking. Host disconnect ends the match (host migration deferred; guests' round wins are reported locally).

**Session flow reuse:** join-code lobby UI (RmlUi), version-hash gating — per architecture doc §5; the arena is just a different `SyncSession` payload.

## 7. Render bridge (sim → recompiled game)

The arena runs inside a patched "Battle" menu entry that loads a campaign map shell (skybox, lighting, a visually matching level chunk) with normal gameplay object spawning suppressed, then:

- **Actors:** at load, spawn 4 bomberman objects + 16 bomb objects + effect slots from the `gObjects` pool using the game's own spawn routine (via patch-exported function). Each rendered frame, native bridge writes `pos` (Q20.12 → game's float coords), yaw, model/palette (4 player colors), and animation id derived from `state`+`anim_hint` into the corresponding `ObjectStruct` fields (struct layout from decomp `obj.h`/`types.h`).
- **Interpolation:** RT64 already interpolates object transforms across sim ticks — the bridge writes at sim cadence and high-framerate smoothness comes for free, same as the campaign.
- **Effects & audio are edge-triggered from confirmed ticks only.** The bridge diffs confirmed-frame state (not predicted): blast spawn → trigger Hero's explosion effect + SFX; because rollback never re-fires an already-confirmed edge and predicted edges wait for confirmation (≤ prediction-window delay on SFX, imperceptible at 8 ticks), there are no ghost explosions and no audio rewind problem.
- **Camera:** single shared camera for all configurations — fixed orbit position per arena framing the whole space (classic battle-mode presentation; also the only option that respects the recomp's single-viewpoint renderer). Slight auto-zoom on the players' bounding sphere, deterministic-irrelevant (camera reads sim, never feeds back).
  *Verified 2026-07-19 (viewer feel-testing + period reviews/guides): Hero's own camera is fixed-angle position-follow — it **never rotates with the player's facing** (only a slight C-button tilt while standing). A rotate-behind chase cam feels wrong and motion-sickness-inducing in practice. The fixed shared camera above therefore matches both the original's presentation and its input model (stick direction maps to a stable world basis). The SDL debug viewer's FOLLOW and ORBIT modes preview exactly this.*
- **HUD:** 4 heart/round counters via RmlUi overlay (already used for menus) rather than patching Hero's HUD — cheaper and rollback-oblivious.

## 8. Input (4P local)

ultramodern exposes 4 controller slots (`MAXCONTROLLERS 4`) but the frontend maps devices to port 1 — the input work is native-side: enumerate SDL gamepads → assignment screen in lobby ("press A to join"), keyboard as a 4th-player fallback. Feeds `SyncSession` local slots directly; the campaign's port-2 routing work is *not* a dependency.

## 9. Build plan (arena-first milestones)

| # | Milestone | Contents | Exit criterion |
|---|---|---|---|
| A0 ✅ | **Headless sim** — *done 2026-07-18, `bmhero-arena/`* | ArenaState, tick pipeline, tuning table v0 (placeholders marked `TODO(feel)`), replay/rollback-stress/snapshot/liveness tests; CI matrix in `ci/determinism.yml` pins scripted-match hash `a55aa9b1` across gcc/clang/MSVC-runner/ARM64 | 4P scripted match runs bit-identical across x86_64/ARM64, re-sim under continuous rollback *(local gcc -O0/-O2 verified; full matrix runs on first CI push)* |
| A1 | **On screen** | Battle menu entry, map shell load, render bridge, 1P walking/throwing with Hero assets | Feels like Hero (side-by-side capture vs. campaign movement) |
| A2 | **4P couch** | SDL pad assignment, shared camera, HUD, full ruleset, 3 arenas | 4P local match completable; playtest for stalling (§2 levers) |
| A3 | **Online** | GekkoNet online sessions, rendezvous server, lobby codes, desync surfacing | 4P mesh match over real WAN (100ms, 1% loss) with no perceived input lag |
| A4 | **Polish** | Items, spectators, reconnect-as-spectator, more arenas, sudden-death variants | — |

Campaign co-op (Domain A) resumes after A3 — it shares the transport/session/lobby layer built here, so arena-first front-loads all reusable netcode.

## 10. Risks specific to this mode

- **Feel-matching is the schedule risk, not netcode.** A1's side-by-side test exists to catch it early; empirical constant-fitting (§5) is the fallback where the decomp is incomplete.
- **Object pool contention:** 4+16+effects slots from `gObjects[207]` alongside the map shell's own objects — fine on paper; A1 verifies the shell leaves enough free slots (or the spawn-suppression patch widens).
- **GekkoNet at 4P mesh** is less battle-tested than 2P (its users are mostly fighting games) — A3 includes a soak test; the `SyncSession` wrapper keeps a hand-rolled fallback possible.
- **Free movement may undercut bomb pressure** (§2) — playtest lever list ready; worst case, the Hybrid style (grid bombs, free movement) remains reachable from this sim with modest changes.
