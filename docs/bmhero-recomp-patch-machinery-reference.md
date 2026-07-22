# BMHeroRecomp — static patch / overlay machinery: definitive reference

**Date:** 2026-07-21 · **Source:** deep-dive of `lib/N64ModernRuntime`
(librecomp + ultramodern + N64Recomp) and the fork's build wiring, from source.
Produced during A1.2c slice-2 debugging; supersedes several folklore beliefs
recorded earlier (corrections flagged inline). Fork:
`C:\Users\dshi\GitRepos\BMHeroRecomp`.

## A. The patch pipeline (verified)

1. **Compile** each `patches/*.c` with clang → MIPS (`patches/Makefile:6-9`;
   `-mips2 -mabi=32 -mno-abicalls -G0 -nostdinc`).
2. **Link** with `ld.lld` (`Makefile:10`): `-nostdlib -T patches.ld -T syms.ld
   -Map patches.map --unresolved-symbols=ignore-all --emit-relocs` →
   `patches/patches.elf`.
   - `patches.ld`: sections at `RAMBASE = 0x80801000` (order
     `.ctors/.dtors/.text/.recomp_patch/.recomp_export/.recomp_event/.rodata/
     .data`, `.bss (NOLOAD)`), `ASSERT(. <= 0x81000000)`.
   - `syms.ld`: the manual patch symbols at `0x8F000000+` (dummy addresses the
     recompiler turns into native-function calls). `--unresolved-symbols=
     ignore-all` lets base-game symbols stay undefined in the ELF.
3. **Generate** with `N64Recomp.exe patches.toml`:
   - **`func_reference_syms_file="BMHeroSyms/dump.toml"`** and
     **`data_reference_syms_files=["BMHeroSyms/data_dump.toml"]`** are the
     symbol tables used to resolve relocations.
   - `output_binary_path="patches/patches.bin"` — the recompiler itself writes
     the loadable binary so it can patch MIPS32 relocs.
   - `allow_exports=true`, `strict_patch_mode=true`, `use_absolute_symbols=true`.
4. **Output** `RecompiledPatches/patches.c`, `patches_bin.c` (blob), `funcs.h`,
   `recomp_overlays.inl` (section/reloc/export tables).
5. **Load**: `src/main/register_patches.cpp:12-16` → `recomp::overlays::
   register_patches(...)` etc.; at startup the patch binary is copied into
   RDRAM at `patch_rdram_start` (`overlays.cpp:182-186`), and
   `init_overlays` → `load_patch_functions` registers patch functions into
   `func_map` (`overlays.cpp:274-293`).

`librecomp/src/patcher.cpp` is a BPS ROM patcher for **mods** — unrelated to
static patches.

## B. Relocation resolution — and the D_* correction

**Relocations resolve at GENERATION time, not load time**, by NAME lookup in
the two TOML symbol files. The base ELF and `symbol_addrs.txt` play no role at
load.

- Symbol name missing from the reference symbols → **hard build failure**:
  `Undefined symbol: {}, not found in input or reference symbols!` to stderr,
  recompilation aborts (`N64Recomp/src/elf.cpp:427-434`). A truly missing
  symbol is a *loud build error*, not a silent load crash.
- Reference HI16/LO16 relocs into a non-relocatable section are **baked** into
  the instruction and the reloc deleted (`elf.cpp:585-612`;
  `recompilation.cpp:184-197`). `R_MIPS_32` into a non-relocatable section is
  baked into the data word; into a *relocatable* section it's a hard error
  (`elf.cpp:446-450`, "Cannot reference {} in a statically initialized
  variable").
- **`BMHeroSyms/dump.toml` marks NO section relocatable** for this ROM, so all
  base-game data references bake to constants at generation. The generated
  `RecompiledPatches/patches.c` uses zero runtime HI16/LO16 relocs; the only
  runtime `RelocEntry`s are `R_MIPS_26` (calls), consumed only by mod hook
  regeneration.
- **CORRECTION of earlier folklore:** the generator does NOT treat auto-named
  `D_*` differently from named globals. `data_dump.toml` has 11,474 `D_*`
  symbols; `D_801163DC` is present (`data_dump.toml:5216`) and bakes fine. The
  real divergence is **presence in `data_dump.toml`** (decided per-address by
  the disassembly project), not naming style. An address never labeled there
  (e.g. a pointer into the middle of an array) → build error, not corruption.
  (The A1.2b "data symbol corrupts the patch" episode is therefore
  unexplained-as-described; the literal-address workaround is still *valid*,
  just not *necessary* for symbols present in `data_dump.toml`.)
- If a section ever *were* marked relocatable, its HI16/LO16 relocs would be
  kept for runtime and the static-patch loader applies **no data relocs** —
  that would silently corrupt. Dormant today.

## C. Patch-local `.data`/`.bss` — usable (correction)

`patches.ld` places `.data`/`.bss` in extram; the current build already has
working `.bss` (0x920 bytes, mostly `required_patches.o`) and runs. RDRAM is
one 4GB `VirtualAlloc(PAGE_NOACCESS)` with the first 512MB made RW
(`recomp.cpp:844-849`); the patch region (8–16MB offsets) is inside the RW
window, `.data` is copied in at startup, `.bss` reads as zeros.

**So patch statics are NOT categorically forbidden.** The A1.2b "stateless
patches" rule likely misattributed a different bug (prime suspect per §D: an
ABI mismatch between a patch `DECLARE_FUNC` signature and the native
`_arg<>/_return<>` usage — reads garbage registers → corruption-class
`0xC0000409`). Until re-verified empirically, keeping state native remains the
*safe convention*, but it is not a hard machine constraint.

## D. Native-export ABI (`librecomp/include/librecomp/helpers.hpp`)

- `_arg<index,T>`: `static_assert(index < 4)` — args only in `r4..r7` (a0–a3).
  - floats: only index 0 (`ctx->f12.fl`); index 1 float via `_arg_float_a1`
    (raw a1 bits→float); second FP arg via `_arg_float_f14` (`ctx->f14.fl`).
  - no double pointers; integrals `sizeof(T) <= 4` (no 64-bit args).
- `_arg_string<i>`: copies a NUL-terminated string out of RDRAM.
- `_return<T>`: `sizeof(T) <= 4`; float→`f0`, integral→`r2`. No 64-bit/struct
  returns.
- Anything wider: pass by RDRAM pointer.
- **A signature mismatch between the patch call and the shim's `_arg` reads is
  silent garbage** — prime suspect for "corruption-class" crashes.

## E. Abort / error paths — what prints where

**Build nuance:** shipped build is Release (`/O2 /DNDEBUG`) → `assert()` is
compiled OUT. The `assert(false); exit(1)` pairs below *print their message
then exit(1)* — they do NOT raise `0xC0000409`. A **`0xC0000409` in Release =
Windows fastfail**: an uncaught C++ exception (→`std::terminate`→`abort`) or a
`/GS` stack-cookie / heap-integrity trip. MSVC's default terminate handler
prints nothing → such crashes are silent without a debugger.

Runtime error sites (selected):
| Site | Message | Stream |
|---|---|---|
| `overlays.cpp:364-371` `get_function` | `Failed to find function at 0x%08X` (indirect call to unregistered address) | stderr |
| `recomp.cpp:460-464` | `Switch-case out of bounds …` | stdout |
| `recomp.cpp:466-469` | `Encountered break at original vram 0x%08X` | stdout |
| `error_handling.cpp:11-29` | any `message_box` / `quick_exit` text | stderr (+GUI box) |
| `recomp_api.cpp:36-49` `recomp_error` (patch-callable, sym `0x8F000060`) | patch-supplied string | GUI box, then quick_exit |
| ultramodern throws: `events.cpp:614` renderer-init fail; `threads.cpp:165` **"No threads left to run!"**; `threads.cpp:92` invalid priority | exception `what()` | terminate → **0xC0000409**, effectively silent |

Generation-time (N64Recomp) failures print to stderr and fail the build:
undefined symbols (`elf.cpp:431`), static-init references (`elf.cpp:446`),
strict-mode name mismatches (`main.cpp:730-775`, deletes the output .c).

The stochastic **load-window crash** we see on arena warps (0xC0000409,
identical stacks, no in-level markers) is consistent with one of the
ultramodern throws above (identical stacks = same throw site; timing race =
stochasticity). Unconfirmed until a symbolized dump names it (§H).

## F. Debug facilities that exist

- **`--show-console`** — the ONLY CLI flag (`main.cpp:691-705`): allocates a
  console and reopens stdio. Without it (subsystem WINDOWS) all printed
  diagnostics go nowhere. `play.bat --show-console` works.
- **No debug env vars.** `debug_printf` is a no-op unless
  `ultramodern.hpp:156` is uncommented (then patch-load + PI DMA tracing
  lights up).
- **`recomp_printf`** (patch export) → stdout (needs console/redirect). Note:
  stdout to a redirected FILE is fully buffered — a hard crash loses it (our
  own `arena_bridge.log` survives only because every write is fflushed).
- **`recomp_error("msg")`** — patch-callable loud failure (GUI box + exit):
  the intended "fail loudly from a patch" primitive.
- **`breakpoint_me(int)`** (patch export `0x8080EFE4`) — native breakpoint site.
- `strict_patch_mode=true` validates patch FUNCTION names at generation (not
  data symbols).
- No crash handler / minidump writer is installed — crashes rely on WER.

## G. Function-pointer dispatch

- Direct `jal` to known functions → plain C calls in the generated code (host
  linker resolves). Indirect calls (`jalr`/function pointers) go through
  `LOOKUP_FUNC` → `func_map` (`overlays.cpp:364-372`); patch functions and the
  `0x8F00xxxx` manual symbols are registered there. Calling a pointer whose
  address isn't in `func_map` → `Failed to find function at 0x…` + abort.
  (This is why `gDebugRoutine2 = &arena_render_routine` works: patch functions
  are registered.)

## H. Build configs / symbolized dumps

- Shipped: Release, no PDB → unsymbolized dumps, asserts off.
- **`RelWithDebInfo` is already wired** (`CMakeSettings.json`
  `x64-ReleaseWithDebInfo`; `/Zi /O2 /Ob1 /DNDEBUG`) → PDB with full
  optimization → **crash dumps symbolize**; still subsystem WINDOWS (pair with
  `--show-console`).
- Debug config: `/SUBSYSTEM:CONSOLE`, asserts live — best for reproducing §E
  paths, slow.

## Actionable levers

1. Always launch with `--show-console` during development.
2. Build `RelWithDebInfo` for symbolized dumps (negligible runtime cost).
3. Uncomment `ultramodern.hpp:156` for patch-load/DMA tracing when needed.
4. Treat Release `0xC0000409` as *uncaught exception or memory corruption* —
   symbolize the dump; consider adding a `SetUnhandledExceptionFilter` +
   minidump writer in `main.cpp` (none exists).
5. N64Recomp build errors are loud — but ensure the build wrapper never yields
   a stale exe after a failed generation (check exit codes; we also always
   `make -C patches clean` first).
6. Use `recomp_error()` / `breakpoint_me()` as first-class patch debugging
   primitives.
7. Stay inside the export ABI (≤4 int/ptr args, floats via f12/f14 helpers,
   32-bit returns; wider data via RDRAM pointer). **Watch for silent
   signature/`_arg` mismatches** — garbage reads, corruption-class crashes.
8. Patch-local `.data`/`.bss` is memory-backed and usable (precedent exists);
   if a static write "crashes", instrument the computed address — don't assume
   the section is unbacked.
