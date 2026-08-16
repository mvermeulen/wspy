# Fixing Phoronix Test Suite installs on Ubuntu 26.04

This is a reference for a problem that has nothing to do with `wspy` itself: Phoronix Test Suite (PTS)
test-profiles routinely fail to *install* (download + build the workload binary, before `wspy` ever
gets to instrument it) on a bleeding-edge Ubuntu 26.04 dev box. Most of these test-profiles are years
old and were last verified against a much older toolchain; this system runs:

- **GCC 15.2.0** — default `-std` is `gnu23` (C23), and GCC 14 independently hardened
  `-Wimplicit-function-declaration`/`-Wimplicit-int` from warnings to hard errors in *every* C dialect.
- **CMake 4.2.3** — removed all compatibility with `cmake_minimum_required` below 3.5.
- **Python 3.14.4** — `distutils` is gone from the stdlib (setuptools' vendored copy fills the gap, but
  with different self-import behavior); very old pybind11 pokes CPython internals removed in 3.11+.
- **Boost 1.90.0** — `boost::system` has been header-only with no compiled library since 1.69, and by
  1.90 the deprecated `boost::asio::io_service`/`resolver::query` API is gone entirely.
- **Clang/LLVM 21.1.8**, **ROCm/HIP** installed via Ubuntu's own `libamdhip64-dev` package layout
  (`/usr`, multiarch `lib/x86_64-linux-gnu`) rather than a traditional `/opt/rocm` tree.

None of this is a `wspy` bug — every fix below lives entirely inside a PTS test-profile's own
`install.sh` (or, rarely, its vendored source), patched via `sed` at install time so the fix survives a
clean reinstall. This file exists so the *pattern* is recognizable next time a new test hits the same
wall, rather than re-diagnosing it from scratch. See `workload/phoronix/phoronix.tests.txt` for the
complementary list of tests that are broken for reasons *unrelated* to this toolchain (dead upstream
download hosts, Docker dependencies, root requirements, etc.) and can't be fixed this way at all.

## How the fixes are applied

Every fix here is a `sed`/`export` line added to the test's own `~/.phoronix-test-suite/test-profiles/
pts/<test>/install.sh`, right before the step it needs to affect (extraction, configure, cmake, make).
`install.sh` is what actually runs on install, and PTS re-extracts a test's *source* tarball fresh on
every install — but not the test-profile scripts themselves, so patching `install.sh` once is durable
across reinstalls; patching an already-extracted source file directly is not (`rm -rf` + reinstall wipes
it).

**PTS keeps two copies of every test-profile**, checked in this priority order:
1. `~/.phoronix-test-suite/test-profiles/pts/<test>/` — user-local, writable, always wins.
2. `/usr/share/phoronix-test-suite/ob-cache/test-profiles/pts/<test>/` — system-wide cache, root-owned.

Patching only the user-local copy is enough to fix installs for the current user. Keeping the two in
sync (`sudo cp ~/.phoronix-test-suite/test-profiles/pts/<test>/install.sh
/usr/share/phoronix-test-suite/ob-cache/test-profiles/pts/<test>/install.sh`) matters only if you want a
*different* user account on this machine, or a from-scratch `ob-cache`, to get the fix too — it needs
root, so it's a manual step, not something done automatically. A profile with no `ob-cache` counterpart
at all (a newer version than what was last cached system-wide) has nothing to sync into; that's fine,
PTS just falls through to the user-local copy.

## Recurring failure patterns

These account for the overwhelming majority of installs fixed so far. Recognize the symptom, apply the
same fix shape.

### 1. `cmake_minimum_required` below 3.5

**Symptom:**
```
CMake Error at CMakeLists.txt:N (cmake_minimum_required):
  Compatibility with CMake < 3.5 has been removed from CMake.
  ...
  Or, add -DCMAKE_POLICY_VERSION_MINIMUM=3.5 to try configuring anyway.
```
Sometimes it's the test's own top-level `CMakeLists.txt`; often it's a *vendored* dependency pulled in
via `FetchContent`/`ExternalProject_Add`/a bundled `third_party/` copy that predates the 3.5 floor —
CMake 4 refuses to configure *any* project in the tree with too low a floor, not just the outermost one.

**Fix:** set the documented escape hatch before the `cmake` invocation:
```sh
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cmake .. -D...
```
A no-op when a project's own floor is already ≥3.5, so it's safe to apply defensively even without
confirming which specific `CMakeLists.txt` is the culprit.

**Hit by:** `chia-vdf-1.1.0` (vendored pybind11 v2.6.2), `jpegxl-1.6.0` (vendored `third_party/sjpeg`),
`svt-hevc-1.2.1`, `svt-vp9-1.3.1`, `tnn-1.1.0`, `openjpeg-1.0.0`, `viennacl-1.1.0`, `caffe-1.5.0`,
`libgav1-1.2.1` (vendored abseil-cpp), `nekrs-1.2.0`, plus **320 other cached test-profiles** patched
defensively by a one-off sweep (grep every `install.sh` for a bare `cmake` invocation lacking the guard,
add it unconditionally) rather than waiting to hit each one individually.

### 2. Missing `#include <cstdint>`

**Symptom:** `'uint8_t'`/`'uint16_t'`/`'uint32_t'`/`'uint64_t' was not declared in this scope` (or, more
confusingly, a cascade of unrelated-looking parse errors like `expression list treated as compound
expression`, `template argument 1 is invalid`, or `'ialpha' was not declared in this scope` — GCC's
recovery from the first missing-type error can make everything downstream look broken too). Sometimes
GCC's own note names the fix directly: `'uint8_t' is defined in header '<cstdint>'; this is probably
fixable by adding '#include <cstdint>'`.

**Root cause:** the source relied on `<cstdint>` arriving *transitively* through some other standard
header (commonly `<vector>` or `<functional>`) — true on older libstdc++, no longer true on GCC 15's
leaner headers.

**Fix, single file:** `sed -i '1a #include <cstdint>' path/to/file.h` (or `.cpp`).

**Fix, multiple files in a bundled dependency:** force-include project-wide instead of chasing every
file — for a CMake project, `-DCMAKE_CXX_FLAGS="-include cstdint"`; for a Makefile build using an
already-exported `CXXFLAGS`, append `-include cstdint` to it (see pattern 5 below for why it has to be
an exported env var, not a `make VAR=...` command-line override).

**Hit by:** `botan-1.6.0` (`src/cli/cli.h`), `keydb-1.4.0` (bundled RocksDB's
`compaction_iteration_stats.h`, `data_block_hash_index.h`), `tnn-1.1.0` (`data_type_utils.cc`,
`mat_converter_utils.{cc,h}` — 3+ files, force-included), `libgav1-1.2.1` (bundled abseil-cpp's
`str_format/extension.h`, force-included), `nekrs-1.2.0` (`3rd_party/adios/.../emitterutils.cpp`),
`graph500-1.0.2` (a pre-existing install.sh patch injected a declaration using `int64_t` before
`<stdint.h>` was in scope — see pattern 7).

### 3. `-lboost_system` / `find_package(Boost COMPONENTS system ...)`

**Symptom, linker:** `cannot find -lboost_system: No such file or directory`.
**Symptom, CMake:** `Could not find a package configuration file provided by "boost_system"`.

**Root cause:** Boost's `system` component has been header-only with **no compiled library or CMake
config package at all** since Boost 1.69 (2018); this system's Boost 1.90 doesn't ship one. Any
test-profile that still explicitly links `-lboost_system` or lists `system` in a
`find_package(Boost ... COMPONENTS ...)` call fails outright — not a version mismatch, the library
simply doesn't exist to be found.

**Fix, Makefile:** delete `-lboost_system` from the link line.
**Fix, autoconf:** drop `LIBS="-lboost_system"` from the `./configure` invocation. Watch out — if it was
set as an environment/command-line override, it poisons *every* configure probe, including the very
first "does the compiler work" sanity check, which fails long before the project's own Boost detection
ever runs — a deeply misleading `C compiler cannot create executables` error that has nothing to do with
the compiler.
**Fix, CMake:** drop `system` from the `COMPONENTS` list; `${Boost_LIBRARIES}`/`Boost::*` targets are
populated purely from whichever components were actually found, so this is a clean no-op, not a stub.

**Hit by:** `povray-1.2.1` (autoconf `LIBS=` override), `chia-vdf-1.1.0` (`src/Makefile.vdf-client`),
`caffe-1.5.0` (`cmake/Dependencies.cmake`'s `find_package(Boost 1.54 REQUIRED COMPONENTS system thread
filesystem)`).

### 4. C23 hardening: implicit declarations, `true`/`false` as keywords, empty-`()` prototypes

GCC 15's C default is `-std=gnu23`, and two independent things changed:
- **GCC 14+, all C dialects:** `-Wimplicit-function-declaration`/`-Wimplicit-int` promoted from warning
  to hard error, so 1990s C calling an undeclared function, or a `main(argc,argv)` with no return type,
  now fails outright.
- **C23 specifically:** `true`/`false` became real keywords (not just `<stdbool.h>` macros) — code
  declaring a variable literally named `true` is now a syntax error — and an empty `()` in a function
  prototype means *zero arguments* (previously, in C89/C17, it meant "unspecified arguments", so a
  prototype like `int foo();` calling `foo(x)` used to be legal and now is a hard "too many arguments"
  error).

**Fix, prototype-mismatch class:** `-std=gnu17` restores the old empty-`()` semantics. Does **not** by
itself fix the separate implicit-declaration hardening (that one isn't standard-version-gated) — the two
often need to be fixed together.
**Fix, implicit-declaration class:** `-Wno-error=implicit-function-declaration
-Wno-error=implicit-int` downgrades them back to warnings without touching every call site.
**Fix, `true`/`false`-as-identifier:** rename the variable, or (if the offending code is in an
irrelevant part of the build, e.g. the project's own test harness) skip building that part entirely.

**Hit by:** `himeno-1.3.0` (missing includes compounding implicit-declaration errors — fixed with the
includes directly instead), `postmark-1.1.2` (`-std=gnu17` **and**
`-Wno-error=implicit-function-declaration -Wno-error=implicit-int`, both needed), `graph500-1.0.2`
(a pre-existing install.sh patch's own injected forward declaration used the now-broken empty-`()`
form), `gnupg-2.5.0` (`tests/asschk.c` declares `int true = 1;` — irrelevant to the actual `gpg`/`gpgsm`
binaries this benchmark runs, so skipped via `--disable-tests` rather than patched).

### 5. `make VAR=value` command-line overrides silently break every downstream `+=`

**Symptom:** a fix that looks correct (e.g. `make CXXFLAGS=-fpermissive`) makes the *targeted* error go
away, but produces new, seemingly unrelated failures elsewhere in the same build — a linker "multiple
definition" that wasn't there before, or a cascade of `fatal error: some/header.h: No such file or
directory` from a vendored sub-build that previously compiled fine.

**Root cause:** a variable set via `make VAR=value` on the command line has the *highest* possible
precedence in GNU Make and is passed down to every recursive sub-make through the environment, **still
carrying command-line origin**. A plain `VAR += more stuff` assignment anywhere else in the Makefile
tree — including in a vendored dependency's own, unrelated Makefile — is silently a no-op against a
command-line-origin variable (only `override VAR += ...` can touch it). Any flag that Makefile *appends*
via `+=` (a required `-D` define, an include path, anything) just silently vanishes, project-wide.

**Fix:** export the variable as a plain shell environment variable instead of a `make` command-line
argument:
```sh
# Wrong: freezes CXXFLAGS at command-line precedence for every sub-make.
make CXXFLAGS=-fpermissive -j...

# Right: an env var has lower precedence, so every Makefile's own `CXXFLAGS +=` still appends normally.
export CXXFLAGS=-fpermissive
make -j...
```

**Hit by:** `keydb-1.4.0` — this was found the hard way. An initial `make CXXFLAGS=-fpermissive` fix (for
a real, separate `-Wchanges-meaning` error) silently broke two unrelated things in the same build: KeyDB's
own `src/Makefile` has `CXXFLAGS+= -DASM_SPINLOCK` gating whether a C++ fallback implementation of
`fastlock_{lock,unlock,trylock}` compiles alongside the hand-written x86-64 assembly version (frozen out
→ both got compiled → linker "multiple definition"), and vendored RocksDB's own `Makefile` builds its
entire `CXXFLAGS` via `+=` starting from nothing (frozen out → `-I. -I./include` never applied → every
`#include "rocksdb/*.h"` failed). Switching to `export CXXFLAGS=...` fixed both at once.

### 6. Legacy `*-config` discovery scripts removed in favor of pkg-config

**Symptom:** `configure: error: ... You need <library> to build this program`, even though
`dpkg -l`/`pkg-config --exists <library>` confirm the library **is** installed.

**Root cause:** old autoconf macros (`AM_PATH_LIBGCRYPT` and similar, common across the GnuPG-family
libraries) look for a `<library>-config` executable on `$PATH` rather than a pkg-config `.pc` file.
Current Debian/Ubuntu packaging for `libgpg-error-dev`/`libassuan-dev`/`libksba-dev`/`libnpth0-dev`/
`libgcrypt20-dev` dropped those scripts entirely — but does ship `gpgrt-config`, a single unified
replacement that can emulate all of them (`gpgrt-config <module> --cflags`/`--libs`/`--modversion`).

**Fix:** write tiny shim scripts translating the old names/flags to `gpgrt-config <module> ...`, and
prepend their directory to `$PATH` before `./configure`:
```sh
mkdir -p .gpgrt-config-shims
cat > .gpgrt-config-shims/libassuan-config << 'EOF'
#!/bin/sh
MODULE="libassuan"
args=""
for a in "$@"; do
  case "$a" in
    --host) exit 1 ;;                    # unsupported by gpgrt-config; let the caller's
                                          # `|| echo none` fallback handle it
    --mt) ;;                             # gpgrt-config's output is already thread-safe; drop
    --version) args="$args --modversion" ;;
    *) args="$args $a" ;;
  esac
done
exec gpgrt-config "$MODULE" $args
EOF
chmod +x .gpgrt-config-shims/libassuan-config
export PATH="$PWD/.gpgrt-config-shims:$PATH"
```

**Hit by:** `gnupg-2.5.0` — needed shims for `gpg-error-config`, `libgcrypt-config`,
`libassuan-config`, `ksba-config`, `npth-config` (five separate legacy names, all routed through the one
real `gpgrt-config` binary).

### 7. ROCm/HIP paths assume a `/opt/rocm` layout

**Symptom (CMake):** `CMake Error ... unable to find /opt/rocm/include/hip/hip_common.h`, or a linker
`cannot find /usr/lib/libamdhip64.so` even though `libamdhip64-dev` is installed.
**Symptom (clang):** `cannot find ROCm device library; provide its path via '--rocm-path'`.
**Symptom (compiler selection):** `g++: error: language hip not recognized` — a `.hip` source routed to
the plain C++ compiler instead of the HIP-aware one.

**Root cause:** this system's HIP comes from Ubuntu's own `libamdhip64-dev`/`hipcc` packages, installed
under `/usr` with libraries in the multiarch `lib/x86_64-linux-gnu` subdirectory — not the traditional
standalone ROCm installer's `/opt/rocm` tree many older CMake `Find*.cmake` modules and `configure.ac`
scripts hardcode or default to.

**Fix, hardcoded library path:** replace a hardcoded `"${HIP_ROOT_DIR}/lib/libamdhip64.so"` with a real
`find_library(... PATH_SUFFIXES lib lib64 lib/x86_64-linux-gnu)` call.
**Fix, missing `ROCM_PATH`:** export it — but **scope it narrowly** to just the one `./configure`/build
step that needs it, not the whole build. `ROCM_PATH` is a name multiple independent tools key off of;
setting it globally to work around one tool's `/opt/rocm` default can break another tool's *own*,
already-working auto-detection that happens to check the same variable (see `nekrs` below).
**Fix, wrong device compiler:** if a project has separate CUDA and HIP code paths, check both use the
matching `CMAKE_<LANG>_COMPILER` — an asymmetry where CUDA correctly uses `CMAKE_CUDA_COMPILER` but HIP
was left pointing at plain `CMAKE_CXX_COMPILER` is a real, easy-to-miss upstream bug pattern, not
something to work around locally.
**Fix, missing MPI headers for the HIP compiler:** an MPI *compiler wrapper* (`mpicc`) injects its own
`-I`/`-L` flags automatically; a plain HIP-aware compiler (`clang++`) invoked directly does not. Get the
real include path from `mpicc -show` and add it explicitly to the HIP-only compile flags.

**Hit by:** `nekrs-1.2.0`, across five separate sub-issues in the same install — see below for the full
narrative, since this was the deepest chain in this pass and the fixes interact in a way worth
understanding as a whole rather than pattern-matching each one in isolation.

## Per-test fix log

Tests confirmed installing cleanly after these fixes (verified with a fresh `rm -rf` of the installed
test + `phoronix-test-suite batch-install pts/<test>` on this host):

| Test | Pattern(s) hit | Notes |
|---|---|---|
| `povray-1.2.1` | 3 | `LIBS="-lboost_system"` on the `./configure` line poisoned `AC_PROG_CC`'s own compiler-works probe |
| `botan-1.6.0` | 2 | Single file (`src/cli/cli.h`) |
| `chia-vdf-1.1.0` | 1, 2 (indirectly — pybind11 v2.6.2 predates Python 3.11's opaque frame object, needed bumping the pin to v3.1.0, not just `-include`), 3 | Also: `setup.py`'s `from setuptools import ..., setuptools` self-import broken by modern setuptools; `boost::asio::io_service`/`resolver::query` removed in Boost 1.90, ported to `io_context`/`resolve()`-returns-a-range |
| `jpegxl-1.6.0` | 1 | Vendored `third_party/sjpeg` |
| `himeno-1.3.0` | 4 | Missing `<string.h>`/`<stdlib.h>` directly, not a flag fix |
| `keydb-1.4.0` | 2, 5 | See pattern 5's narrative — the `CXXFLAGS` command-line-override bug hid this test's real fixes behind unrelated-looking failures twice |
| `memtier-benchmark-1.5.0` | (own) | Vestigial `AC_CHECK_LIB([pcre], [pcre_compile])` — nothing in the source actually links PCRE1, which no longer exists on this system at all; deleted the check |
| `postmark-1.1.2` | 4 | Both halves of pattern 4 needed together |
| `svt-hevc-1.2.1` | 1 | |
| `svt-vp9-1.3.1` | 1 | Guard exported before `./build.sh`, which invokes cmake internally |
| `tnn-1.1.0` | 1, 2 | |
| `webp2-1.2.1` | (own) | Unconditional `-Werror` in `cmake/compiler.cmake`; carved out `-Wno-error=array-bounds` (real GCC 15 finding in `context.cc`) and `-Wno-error=unused-result` (`std::remove_if`'s return value newly `[[nodiscard]]` in current libstdc++) |
| `openjpeg-1.0.0` | 1 | |
| `viennacl-1.1.0` | 1 | |
| `gnupg-2.5.0` | 4, 6, (own) | Also: this system's libassuan (3.0.2) exceeds gnupg 2.2.27's hardcoded `NEED_LIBASSUAN_API=2` — relaxed the exact-match version-API check to accept newer-or-equal; verified only for this benchmark's own throughput test, **not a statement that the resulting build is safe for real cryptographic use** |
| `nekrs-1.2.0` | 1, 2, 7 | Six-layer fix; see narrative below |
| `caffe-1.5.0` | 1, 3, (own) | Also: `CodedInputStream::SetTotalBytesLimit(int, int)` — the two-arg overload — removed from modern protobuf; only the one-arg form remains |
| `libgav1-1.2.1` | 1, 2 | Bundled `abseil-cpp` 20210324.1 |
| `graph500-1.0.2` | 2, 4 | A *pre-existing* install.sh patch (from before this pass) had injected `int isisolated();` to silence an old implicit-declaration warning; both broke under the current toolchain — see narrative below |
| `graphics-magick-2.2.0` | — | Not a toolchain bug: PTS's own PHP-based downloader couldn't follow an `http://`→`https://` redirect without the `php-curl` extension. Fixed with `sudo apt install php-curl`, no `install.sh` change |

### `nekrs-1.2.0` narrative (pattern 7, full chain)

Each fix below was real, verified forward progress (the build got measurably further each time), not
guessing — worth reading in order as an example of how deep a single vendored-dependency chain
(nekRS → bundled HYPRE → HIP/ROCm/MPI) can go on an unusually new toolchain:

1. `3rd_party/occa`'s vendored `FindHIP.cmake` hardcoded `"${HIP_ROOT_DIR}/lib/libamdhip64.so"`; this
   system's package puts it under the multiarch subdirectory instead → `find_library()` fix.
2. Bundled HYPRE's own `configure` (via `ExternalProject_Add`) defaults to `/opt/rocm` when `$ROCM_PATH`
   is unset → exported `ROCM_PATH=/usr`, but scoped to *just* HYPRE's own `CONFIGURE_COMMAND` line in
   `cmake/hypre.cmake` — an earlier attempt at exporting it globally broke clang's own, previously-working
   HIP device-library auto-discovery, which checks the *same* environment variable name for a different
   purpose.
3. `cmake/hypre.cmake`'s HIP code path set HYPRE's device compiler to `${CMAKE_CXX_COMPILER}` (plain
   g++) instead of `${CMAKE_HIP_COMPILER}` (clang++) — a real upstream asymmetry against the CUDA code
   path in the same file, which correctly uses `${CMAKE_CUDA_COMPILER}`.
4. Once clang++ actually ran, HYPRE's HIP sources' `#include <mpi.h>` failed: `CMAKE_C_COMPILER` is
   `mpicc` (which injects its own `-I` automatically), but `CMAKE_HIP_COMPILER` is plain `clang++` with
   no MPI awareness → added the real path from `mpicc -show` to HYPRE's HIP compiler flags.
5. HYPRE's own `_hypre_utilities.hpp` defines an empty `static __device__ void __syncwarp() {}` stub,
   dating from when HIP had no such intrinsic; this system's HIP headers (ROCm 7.1) now declare a real,
   non-static `__syncwarp()` themselves, so the stub's `static` redeclaration conflicted → deleted the
   now-redundant stub.

### `graph500-1.0.2` narrative (pattern 2 + 4 stacked)

`install.sh` already contained a line — from before this investigation pass — patching around an older
problem: `sed -i '1s/^/int isisolated(); /' main.c`, injecting a forward declaration for a function
(`isisolated`, taking one `int64_t` argument) that's defined in a different file with no header
declaring it anywhere. Under old C semantics, `int isisolated();` meant "unspecified arguments", which
suppressed the original warning just fine. Under GCC 15's C23 default, empty `()` means *zero*
arguments, so the real call site (`isisolated(root)`, one argument) became a hard "too many arguments"
error — the pre-existing fix broke under the newer toolchain it was never tested against.

The straightforward correction, declaring the real signature (`int isisolated(int64_t);`), immediately
hit pattern 2 in a form worth noting: because the declaration is injected as the file's literal first
line, `int64_t` isn't a known type yet at that point — no `<stdint.h>` has been `#include`d — so GCC's
parser falls back to treating it as an old-style K&R *parameter name* instead of a type
(`error: parameter names (without types) in function declaration`), a different and more confusing
diagnostic than the usual "was not declared" one. The final fix prepends `<stdint.h>` too:
```sh
sed -i '1s/^/#include <stdint.h>\nint isisolated(int64_t); /' main.c
```

## Not fixable this way: genuinely dead upstream sources

Two tests failed for reasons with no local fix — both added to `workload/phoronix/phoronix.tests.txt`'s
exclusion list rather than chased further:

- **`daphne-1.1.0`**: its data host (TU Darmstadt's Hessenbox) has been decommissioned. The download URL
  redirects to a page whose path literally contains `abschaltung` (German: "shutdown") — a notice page,
  not the dataset.
- **`ngspice-1.1.0`**: `ngspice-45.2.tar.gz` no longer exists on SourceForge at the pinned path. Verified
  with more than a default `curl -I` — both listed mirrors 404 after their redirect, and even the
  canonical `sourceforge.net/.../download` redirector with a browser User-Agent falls through to the
  generic project file-listing HTML page instead of serving the archive. The test-profile pins an exact
  MD5/SHA256 for the missing file, so even a differently-packaged current release wouldn't satisfy the
  pin without also updating the checksum — a bigger call than a local workaround.

## Applying this to a newly-failing test

1. Check the install log first: `~/.phoronix-test-suite/installed-tests/pts/<test>/install.log` (or
   `install-failed.log` after a failed attempt) has the real compiler/linker/configure output.
2. Match the symptom against the patterns above before assuming something new — most failures on this
   host are one of these seven.
3. Patch `~/.phoronix-test-suite/test-profiles/pts/<test>/install.sh` with a `sed`/`export` following the
   shape shown, with a comment explaining *why* (the next person hitting this, human or otherwise,
   shouldn't have to re-derive the reasoning from a bare `sed` line).
4. `rm -rf ~/.phoronix-test-suite/installed-tests/pts/<test>` and `phoronix-test-suite batch-install
   pts/<test>` to verify clean from scratch — a partial/stale install directory can mask whether the fix
   actually worked.
5. **Expect layering**: fixing the first visible error often just uncovers the next one underneath
   (`nekrs` took six rounds). Keep re-running rather than assuming one fix means done.
6. Once confirmed, consider `sudo cp`-ing the fixed `install.sh` to its `ob-cache` counterpart (see
   "How the fixes are applied" above) if you want the fix to survive for other accounts on this machine.
