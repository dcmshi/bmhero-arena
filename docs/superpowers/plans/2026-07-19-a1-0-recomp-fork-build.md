# A1.0 Recomp Fork + Build + Boot Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fork BMHeroRecomp, build it locally on the documented Windows path, and boot the campaign from the fork.

**Architecture:** Pure infrastructure — no `bmhero-arena` code changes. Fork on GitHub (master tracks upstream), local clone beside this repo, VS 2022 toolchain, N64Recomp built from source, recompiler run over the user's (already-decompressed-hash) ROM, CMake/clang build, human boot gate. Spec: `docs/superpowers/specs/2026-07-19-a1-0-recomp-fork-build-design.md`.

**Tech Stack:** VS 2022 Community (Desktop C++ + Clang + CMake components), CMake ≥3.20, N64Recomp (C++20), RT64, Ninja.

## Global Constraints

- Fork target: `dcmshi/BMHeroRecomp` from `RevoSucks/BMHeroRecomp`; work never lands on the fork's `master` (upstream-tracking); A1.0 makes NO commits to the fork at all.
- Local clone path: `C:\Users\dshi\GitRepos\BMHeroRecomp` (`--recurse-submodules`).
- **No ROM or generated ROM-derived C code is ever committed** — verify the fork's `.gitignore` covers them before any later work; never `git add` in the fork during A1.0.
- Facts already pinned: user's `Bomberman Hero (USA).z64` sha1 = `a36364b7e59351f7551ab351cb3b41ebc4be285b` — exactly the recompiler-input ("decompressed") hash from `BUILDING.md`; no decompression step needed.
- Residual unknown (bounded): whether the built game's plug-and-play menu accepts this same file at runtime ("standard ROM" per BUILDING.md). If rejected, read the fork's ROM-validation source to learn the expected hash and report to the user — do not hunt for ROMs; the user supplies their own copies.
- `bmhero-arena` repo: only doc updates (CLAUDE.md status) at the end.
- Long installs run in the terminal with the user warned; if winget needs elevation or the VS Installer needs UI choices, hand off to the user with exact click-path instructions.

---

### Task 1: Fork + clone

**Files:** none in this repo (operations only)

- [ ] **Step 1: Fork on GitHub**

```bash
gh repo fork RevoSucks/BMHeroRecomp --clone=false
gh repo view dcmshi/BMHeroRecomp --json parent --jq '.parent.nameWithOwner'
```
Expected: fork created; parent prints `RevoSucks/BMHeroRecomp`.

- [ ] **Step 2: Clone with submodules**

```bash
cd /c/Users/dshi/GitRepos
git clone --recurse-submodules https://github.com/dcmshi/BMHeroRecomp.git
cd BMHeroRecomp && git remote add upstream https://github.com/RevoSucks/BMHeroRecomp.git && git remote -v
```
Expected: clone completes with submodules (BMHeroSyms, lib/*); both `origin` (dcmshi) and `upstream` (RevoSucks) remotes listed.

- [ ] **Step 3: Verify ROM hygiene**

```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp && grep -iE "z64|rom" .gitignore
```
Expected: ROM patterns ignored. If not, note it — we still never `git add` in the fork, and A1.1 adds patterns before any commit.

---

### Task 2: Toolchain — VS 2022 + make

- [ ] **Step 1: Install VS 2022 Community with required workloads**

Run (PowerShell; large download, ~30-60 min):
```powershell
winget install --id Microsoft.VisualStudio.2022.Community -e --override "--quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --add Microsoft.VisualStudio.Component.VC.Llvm.Clang --add Microsoft.VisualStudio.Component.VC.Llvm.ClangToolset --add Microsoft.VisualStudio.ComponentGroup.NativeDesktop.Llvm.Clang --includeRecommended"
```
If winget/elevation fails: tell the user to run the VS Installer manually and check **Desktop development with C++**, **C++ Clang Compiler for Windows**, **C++ CMake tools for Windows**, then continue.

- [ ] **Step 2: Verify the toolchain**

```powershell
& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
```
Expected: prints the VS 2022 path. Then verify clang-cl exists under `<vs>\VC\Tools\Llvm\x64\bin\clang-cl.exe`.

- [ ] **Step 3: make**

MSYS2 can provide make without Chocolatey:
```powershell
$env:MSYSTEM='UCRT64'; C:\msys64\usr\bin\bash.exe -lc 'pacman -S --noconfirm --needed make && make --version | head -1'
```
Expected: GNU Make version line. (Their guide suggests choco; MSYS2 make is equivalent for the recompiler-invoking makefiles. If the build later fails on make specifics, install via `choco install make` as the fallback.)

---

### Task 3: Build N64Recomp

- [ ] **Step 1: Clone recursively**

```bash
cd /c/Users/dshi/GitRepos
git clone --recurse-submodules https://github.com/N64Recomp/N64Recomp.git
```

- [ ] **Step 2: Build with the VS toolchain**

Run from a VS developer environment (PowerShell):
```powershell
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64"
cd C:\Users\dshi\GitRepos\N64Recomp
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```
Expected: `build/N64Recomp.exe` and `build/RSPRecomp.exe` exist.

- [ ] **Step 3: Stage the recompiler binaries in the fork root**

```powershell
Copy-Item C:\Users\dshi\GitRepos\N64Recomp\build\N64Recomp.exe, C:\Users\dshi\GitRepos\N64Recomp\build\RSPRecomp.exe C:\Users\dshi\GitRepos\BMHeroRecomp\
```
(They're exe artifacts in the fork's working tree; the fork's gitignore or our never-add rule keeps them uncommitted.)

---

### Task 4: Recompile + build BMHeroRecompiled

- [ ] **Step 1: Stage the ROM for the recompiler**

Check what filename `bmhero.toml` expects:
```bash
cd /c/Users/dshi/GitRepos/BMHeroRecomp && grep -iE "elf_path|rom|z64" bmhero.toml | head -5
```
Copy the user's ROM to that exact name/location (it already has the required sha1 `a36364b7e59351f7551ab351cb3b41ebc4be285b`):
```powershell
Copy-Item "C:\Users\dshi\GitRepos\bmhero-arena\Bomberman Hero (USA).z64" "C:\Users\dshi\GitRepos\BMHeroRecomp\<name from toml>"
```

- [ ] **Step 2: Generate the recompiled C**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
.\N64Recomp.exe bmhero.toml
.\RSPRecomp.exe aspMain.toml
```
Expected: generated C sources appear (per the toml's output dirs). If the recompiler reports a hash/format error, STOP and report exactly what it said.

- [ ] **Step 3: Configure + build (clang per BUILDING.md)**

In the VS dev shell (as Task 3 Step 2):
```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp
cmake -S . -B build-cmake -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_C_COMPILER=clang-cl -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-cmake --target BMHeroRecompiled --parallel
```
Expected: `build-cmake/BMHeroRecompiled.exe`. This is the long pole (RT64 + generated code; possibly 10-30 min). If configure fails with clang-cl, retry with `-DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang` (their doc's exact flags) before any other triage. If it still fails at HEAD, check out the commit upstream's own CI last built green (`gh run list -R RevoSucks/BMHeroRecomp`) and rebuild — report the pin to the user.

- [ ] **Step 4: Smoke-launch**

```powershell
cd C:\Users\dshi\GitRepos\BMHeroRecomp   # must run from repo root (assets/)
.\build-cmake\BMHeroRecompiled.exe
```
Expected: the launcher/menu opens (no ROM loaded yet is fine). Close it.

---

### Task 5: Boot gate (human) + docs

- [ ] **Step 1: Human checkpoint — play the campaign**

Ask the user to: launch `BMHeroRecompiled.exe` (from the fork root), supply `Bomberman Hero (USA).z64` in the menu when prompted, and play into level 1. If the menu rejects the ROM (it may want a standard retail dump rather than the decompressed image): find the ROM-validation code in the fork (`grep -rn "a36364\|sha1\|0x[0-9a-f]\{40\}" src/ include/ | head`), report what hash it wants, and let the user supply that copy. Do not proceed to A1.1 until the game plays.

- [ ] **Step 2: Update CLAUDE.md in bmhero-arena**

Append to the status section:
```markdown
**A1.0 complete (2026-07-19).** Fork `dcmshi/BMHeroRecomp` builds locally
(VS 2022 + clang, N64Recomp from source) and boots the campaign at
`C:\Users\dshi\GitRepos\BMHeroRecomp`. The repo's ROM is already the
recompiler-input image (sha1 `a36364…`). Next: A1.1 mod scaffold — arena
submodule + Battle menu entry + map-shell load.
```
And in "Next milestones", replace the A1 item's parenthetical "(needs user's ROM + local BMHeroRecomp build, fork of …)" with "(fork built and booting — A1.0 done; continue with A1.1 scaffold → A1.2 bridge → A1.3 feel)".

- [ ] **Step 3: Commit (bmhero-arena only)**

```bash
cd /c/Users/dshi/GitRepos/bmhero-arena
git add CLAUDE.md
git commit -m "docs: A1.0 complete - recomp fork builds and boots locally"
git push
```

---

## Plan self-review notes

- Spec coverage: deliverables 1-2 → Task 1; 3 → Task 2; 4 → Task 3; 5 (ROM) → resolved fact + Task 4 Step 1; 6 → Task 4; 7 (boot gate) → Task 5. Non-goals respected (no fork commits, no arena code, no CI).
- The one spec unknown (runtime ROM acceptance) is Task 5 Step 1's bounded branch: read the validator, report, user supplies their copy — never ROM-hunting.
- Ops-heavy plan: verification is command-exit + artifact-exists + human boot, not unit tests — appropriate for pure infrastructure.
