# A1.0 — Recomp fork, local build, boot — design spec

**Date:** 2026-07-19 · **Status:** approved (design review in-session)
**Purpose:** stand up the BMHeroRecomp foundation for the A1 render bridge:
fork the recomp, build it locally on the documented Windows path, and boot
the campaign from our fork. Pure infrastructure — zero `bmhero-arena` code
changes. First of four A1 sub-milestones (A1.0 fork/build/boot → A1.1 mod
scaffold + submodule → A1.2 render bridge → A1.3 feel pass). A3 online
hardening follows A1 (user-ordered).

**Repo decision (amends CLAUDE.md):** `bmhero-arena` stays the canonical
repo; the fork will consume it as a **git submodule** starting in A1.1. The
fork adds only the integration layer (patches, render bridge, menu entry).

## 1. Upstreams (verified 2026-07-19)

- **Recomp (fork this):** https://github.com/RevoSucks/BMHeroRecomp —
  active (pushed 2026-07-18), N64Recomp + RT64, Windows releases exist,
  patch system present (`patches/`, `patches.toml`, `bmhero.toml`), mod
  support advertised. Build documented in `BUILDING.md`.
- **Decomp (read-only reference, never forked):**
  https://github.com/Bomberhackers/bmhero — constants/behavior mining only.
- **Recompiler:** https://github.com/N64Recomp/N64Recomp — built separately;
  its `N64Recomp.exe` + `RSPRecomp.exe` are run from the fork root.

## 2. Deliverables

1. `dcmshi/BMHeroRecomp` fork on GitHub (master tracks upstream; all our
   work on feature branches).
2. Local clone at `C:\Users\dshi\GitRepos\BMHeroRecomp`
   (`--recurse-submodules`).
3. Toolchain installed: Visual Studio 2022 Community (winget) with Desktop
   development with C++, C++ Clang Compiler for Windows, C++ CMake tools;
   `make` available (Chocolatey per their guide, or MSYS2's make if it
   suffices — decided at execution).
4. N64Recomp built from source; recompiler binaries in the fork root.
5. ROM pipeline: user's `Bomberman Hero (USA).z64` sha1-verified; a
   decompressed image produced matching sha1
   `a36364b7e59351f7551ab351cb3b41ebc4be285b` (the recompiler input). The
   decompression method is a discovery step: check the recomp's docs/FAQ,
   repo tooling, then the decomp repo's `LZSS.c`-based tooling; ask the
   project's community docs as fallback. The *standard* ROM remains what
   the built game loads at runtime.
6. Code generation (`./N64Recomp bmhero.toml`, `./RSPRecomp aspMain.toml`)
   and the `BMHeroRecompiled` CMake build (clang, per BUILDING.md).
7. **Boot gate (human):** user runs the exe from the fork root, supplies
   the standard ROM in its menu, and plays campaign level 1. Exit
   criterion for A1.0.

## 3. Non-goals

No arena code in the fork yet, no submodule yet (A1.1), no patches or menu
changes, no upstream PRs, no CI for the fork (integration CI is designed in
A1.1 when there is something of ours to gate). No ROM or decompressed
image is ever committed anywhere (gitignore verified in both repos).

## 4. Risks / notes

- VS 2022 install is ~10 GB and slow; winget can drive it but component
  selection may need the VS Installer UI — user does the clicking if so.
- Upstream moves fast; the fork snapshots it. We do not rebase mid-A1.
- ROM decompression is the one true unknown; it gets a bounded discovery
  step with named fallbacks rather than guesswork.
- First RT64 launch compiles shaders (one-time slowness, expected).
- If the documented VS path fails to build at the pinned commit, the
  fallback is building the same commit upstream CI uses (check their
  `.github/` workflows) before touching anything else.
