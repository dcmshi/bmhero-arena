# bmhero-arena — A0 headless battle-arena sim

Deterministic, rollback-ready simulation core for the Bomberman Hero multiplayer
battle mode. Destined for `src/arena/` in a BMHeroRecomp fork; buildable and
testable standalone (no ROM, no recomp, no dependencies).

Design docs: `bmhero-multiplayer-architecture.md`, `bmhero-battle-arena-design.md`.

## Layout

    src/arena/arena_math.h    Q20.12 fixed-point (no floats anywhere), isqrt, sin/cos table, iatan2
    src/arena/arena_state.h   ArenaState (944B POD; the snapshot AND wire format), input packing, RNG, hash
    src/arena/arena_tuning.h  every gameplay constant; placeholders marked TODO(feel) pending decomp transcription
    src/arena/arena_geom.h    static arena geometry (not in state; hashed into version handshake)
    src/arena/arena_sim.c     the tick pipeline (fixed order = determinism contract)
    tests/test_determinism.c  replay, rollback-stress, snapshot, liveness gates

## Build & test

    cmake -S . -B build && cmake --build build && ctest --test-dir build
    # or simply:
    gcc -std=c11 -O2 -o test src/arena/arena_sim.c tests/test_determinism.c && ./test

## Invariants (do not break)

1. No floats in `src/arena/`. All math integer/fixed-point via int64 intermediates.
2. No reads outside `ArenaState`, the tick's inputs, and `static const` data.
3. Fixed iteration orders (players 0..3, bombs 0..15, pairs 01,02,03,12,13,23).
4. Struct layout changes = netcode version bump (static_asserts will catch you).
5. Padding stays zeroed: `arena_init` memsets; only whole-struct copies.

## Next (per design docs)

- GekkoNet `SyncSession` wrapper (couch/online/stress configs) — A2/A3
- `TODO(feel)` constant transcription from bomberhackers/bmhero — A1
- Render bridge into the recomp's `gObjects` — A1
