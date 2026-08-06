# Bomberman Hero Multiplayer — project context for Claude Code

Multiplayer (battle arena now, co-op campaign later) for Bomberman Hero, built on
BMHeroRecomp (N64Recomp static recompilation + RT64). This repo is the canonical
sim/netcode; the fork `dcmshi/BMHeroRecomp` consumes it as a submodule at
`lib/bmhero-arena` and owns the integration layer (patches, render bridge, menu).

## Read first

**Resuming work? Read the newest `docs/HANDOFF-*.md` right after this file.** It
carries the open items in priority order, the traps that cost real time, the
useful commands, and the one-time setup already done.

| doc | what it's for |
|---|---|
| `docs/HANDOFF-2026-08-06.md` | **current open items, traps, commands** — read on resume |
| `docs/bmhero-recomp-integration-notes.md` | living RE reference for the fork-side render bridge (patches/exports, per-frame hooks, object & player structs, input, camera, coord mapping). Read before any A1.2+ work. **§8.x is the authority when anything else disagrees.** |
| `docs/bmhero-battle-arena-design.md` | the mode being built: ruleset, ArenaState spec, tick pipeline, GekkoNet plan, render bridge, host-session model |
| `docs/bmhero-multiplayer-architecture.md` | overall design: two sim domains, determinism model, netcode topology, milestones |
| `docs/bmhero-player-movement-re.md` | decomp-sourced movement model behind `arena_tuning.h` |
| `docs/renderdoc-capture-reference.md` | capturing/inspecting real frames; the visual **ground truth** when a screenshot disagrees with a calculation |
| `docs/bmhero-recomp-patch-machinery-reference.md` | how patches/exports/symbols actually resolve |
| `docs/PROJECT-LOG.md` | milestone history (what was done when, and why). Archive — not current state |
| `README.md` | this repo's layout, build, and the five invariants |

## Where things stand (verified 2026-08-06)

**Sim:** `TUNE_VERSION` **21**, pinned hash **`fbdb0d08`** (v21: set-ahead —
grounded sets place 30u ahead like vanilla, user decision 2026-08-04; v20:
air-set fall arc — hands birth, 8-sample attach, bomb gravity 2.0; v19:
player↔settled-bomb pushout to the 30u stand gap). **The sim was not touched
by A3 either** — the one new file under `src/arena/` is `arena_script.h`
(header-only extraction, gate-proven behavior-neutral). Canonical `main` and
fork `master` both **pushed**, gate-green, SOAK GREEN. Per-slice feature
branches are retired — **work from `master`** on the fork.

**Done:** A0 (headless deterministic sim), A2 (GekkoNet SyncSession), the SDL
debug viewer, the tuning-loop toolkit, A1.0 → **A1.5**, feel rounds 4–13
(round 13 user-verified 2026-08-05: v19–v21 + real-bomber puppets + per-state
clips, one combined boot; facing + transition tails NOT flagged — they stay
deferred), and **oracle 2.0**: choreography lives in
tick-unit verb tables and gate check 17 diffs the arena's whole per-verb anim
timeline against vanilla's (§8.32–§8.38), with derived floors (#32/#33). All
of the differ's catches are fixed (§8.36–§8.38) — **10 of 10 verbs are live
assertions and the divergence register is EMPTY**. Check 18 fits the air-set
fall arc against the airset_* goldens (§8.38b). The bridge drives the
carry/throw/midair/jump clips from vanilla goldens; player 0's Pos.y is
sim-owned always. Puppets 1–3 also play **per-state clips** now (§8.41): a pure
state→clip chooser triggering the §8.40 anim funnel on change only, on the
engine's own frame clock — generic-pool anim instances were **measured** to
self-advance at +2/frame, so no manual frame pump exists or is needed.

**A1 is COMPLETE (2026-08-05, feel round 13 user-verified):** the differ
register is empty, v19–v21 shipped, **the real bomber puppets (§8.40)** play
**per-state clips (§8.41)** from their sim state — screenshot-, probe-, and
eyeball-verified. Deferred past A3, on purpose: **P2–P4 visual identity**
(tint spike negative — the light-colour override lands but the bomber
material has no SHADE term; §8.41 has the mechanism and the named next
candidate), the battle map (Nitros stand-in, user call), puppet facing
(yaw placeholder, not flagged in feel), and the missing transition tails
(not flagged). Do not promote any of these without a new feel report.

**A3 — online hardening (ROM-free): IMPLEMENTED (2026-08-06), close-out
pending.** All nine plan tasks shipped on canonical `main` (spec:
`docs/superpowers/specs/2026-08-05-a3-online-hardening-design.md`):
rendezvous+relay binary, lobby codes, custom one-socket GekkoNet adapter,
lobby client, 4P mesh green in four ctest variants (direct / forced-relay /
impaired-wan100 / desync-inject), `tools/net-soak.ps1` SOAK GREEN at its
numeric exit criteria, desync bundles + `replay_bundle` localization, viewer
`--host`/`--join CODE`. **NOW: the close-out** — deploy `arena_rendezvous`
somewhere reachable and run the human real-WAN checkpoint (handoff item 1),
and watch the first CI run (it compiles the POSIX socket branches for the
first time). After that: the fork slice (local = `local_slot`, puppets =
other slots — spec §G carries the contract).

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

Any change to the pinned hash or the tuning metrics **must** be an intentional
gameplay change with `TUNE_VERSION` bumped in the same commit; `tools/repin.ps1`
refuses otherwise (that's invariant #4 enforced).

## The working loop

```powershell
tools\tune-report.ps1 -Compare base,friction=0.020,friction=0.045   # pick a value from numbers
<edit arena_tuning.h + bump TUNE_VERSION>
tools\gate.ps1                                                      # everything CI runs, one command
tools\repin.ps1                                                     # rewrite both pins, shows the diff
<commit, push, bump the submodule in the fork>
.\build.ps1 -Config rwdi -Soak 5                                    # fork: PATH+patches+build+soak+oracle-gate
```

On the fork, `-Soak` also runs **`tools\oracle-gate.ps1`**: the arena checked
against the vanilla game's own numbers in `tools\oracle\goldens.json` — set/kick
pose index and length, bomb rest lift, throw impact detonation (§8.26). Rerun
`tools\oracle.ps1` (a scripted vanilla boot) when a new behaviour needs a
golden. **When a golden and an arena default disagree, the golden wins**, in the
same commit that turns the gate green. Check 17 additionally diffs the WHOLE
per-verb anim timeline against `tools\oracle\timelines.json` (§8.35), so a wrong
clip nobody has complained about still goes red. The third disposition, beside
"fix the arena" and "the golden wins", is `tools\oracle\known-divergences.json`:
a mute must cite its reason, and the gate goes **red when a registered verb
starts passing** — stale mutes get removed, not kept.

- `tools/gate.ps1` — all four suites + hash + metrics with CI's exact flags. Run
  it before claiming anything is green.
- `tools/tune-report.ps1` — objective feel metrics (top speed/ramp, stop
  distance+time, 180/90 turn time+radius, jump arcs, arena traverse). Choose tune
  values from these, not from a guess.
- `tools/trace-diff.ps1 -A base -B friction=0.020` — "the hash changed, *where*":
  first diverging tick and which field moved. `-OptA -O0 -OptB -O3` on one tuning
  is a determinism hunt (divergence there is an invariant #1/#2 bug).
- `tools/conv.ps1` — Q20.12 ↔ sim units ↔ Hero units (×120) ↔ game u/frame ↔
  BAM/degrees ↔ ticks/seconds. `-Table` decodes the whole tuning table.
- Fork `tools/arena-soak.ps1` — unattended boots with input injection; generic
  `-Expect` / `-Absent` / `-Rising` / `-Mode <n>`, so new probes need no harness
  edit. Fork `tools/arena-log.ps1` summarises `arena_bridge.log`.

**Rule: no build reaches the human without a green soak on that exact build.**
A failed build leaves the previous exe in place and the soak will happily test
it — confirm `BUILD OK` before trusting any probe result.

**Gotcha:** MSYS2 gcc invoked by absolute path fails silently (exit 1, no
diagnostic) unless its own `bin` is on PATH — the PS scripts prepend it.

## How this project has learned to work

Recorded because each was paid for in hours, and the handoff's trap list expands
on them:

- **Measure the geometry, not the player.** Any measurement taken by moving an
  entity is confounded by the bound you're trying to check.
- **Validate an instrument before trusting it.** `capture-game.ps1` cropped to a
  quarter frame for weeks and its output looked entirely plausible. When a model
  and an instrument disagree repeatedly, suspect the instrument.
- **A gate that asserts your own assumption cannot fail.** One asserted that anim
  index 29 was *playing*, never what 29 *drew*, and stayed green through three
  feel tests of the wrong animation. Pair a gate with something you didn't choose.
- **A call-graph search answers "is this used?", never "does this exist?"** For an
  asset, enumerate the table and render it.
- **A frozen sim is still deterministic** — the determinism suite cannot catch a
  liveness bug. Test rounds actually progressing.

## Repo plan

This repo stays **canonical and standalone** (sim/netcode/viewer keep their own
CI, tests, fast iteration); the fork consumes it as a git submodule and adds only
the integration layer. Note the fork's default branch is **`master`**, not `main`.
The recomp is GPL-3.0 — everything here ships GPL-compatible.

Specs and plans for each slice live in `docs/superpowers/{specs,plans}/`.

## Known intentional simplifications (v1)

No items/powerups (v2 appends `ArenaItem[16]` to state), no pits in arena 0,
sudden-death = wall shrink only, host migration deferred. `TUNE_RESPAWN_TICKS`
(respawn after death) is a **testing accommodation**, not the design — the
shipping mode is stock-based elimination; revisit before calling the ruleset done.
