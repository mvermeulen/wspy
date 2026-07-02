# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

wspy ("workload spy") is a C instrumentation wrapper: it launches a child workload as a subprocess and
collects runtime + hardware performance counter metrics around it (IPC, topdown, cache, TLB, branch,
software counters), plus optional system-wide (`/proc`, `/sys`) and AMD GPU metrics. Output is CSV or
human-readable. This is the author's internal research testbed, published for portability across
machines — treat it as a small, pragmatic C codebase rather than a polished library.

## Build & Test

- `make` — builds `wspy`, `cpu_info`, `proctree` (default, no GPU support)
- `make AMDGPU=1 [ROCM_DIR=/path/to/rocm]` — builds with AMD GPU support, also builds `amd_smi`, `amd_sysfs`. If `ROCM_DIR` isn't given, the Makefile auto-detects it by checking for `amd_smi/amdsmi.h` under `/opt/rocm` (the traditional ROCm install location) then `/usr` (where distro packages like Debian/Ubuntu's `rocm-smi-lib` install it), preferring `/opt/rocm` if both exist
- `make test` — builds and runs `test_wspy` and `test_proctree` (unit-style tests; `test_wspy.c` `#include`s `wspy.c` directly with `TEST_WSPY`/`AMDGPU=0` defined to stub out `main()` and disable GPU code — the `test_wspy` Makefile target always compiles its own objects with `-DAMDGPU=0`, independent of whatever the top-level `AMDGPU` value is, so it can't pick up a mismatched GPU-enabled `topdown.o` from the same build tree)
- `./run_tests.sh` — full test script: builds, runs unit tests, then integration-smoke-tests the real `wspy`/`proctree` binaries (CSV output, `--tree` output, network interface naming, GPU flag warnings when built without `AMDGPU=1`), and additionally exercises the AMDGPU build if ROCm headers are found under `/opt/rocm` or `/usr`
- `./test_amd_smi.sh` — focused smoke test for the `amd_smi` module (requires `AMDGPU=1` build)
- `make clean` — remove `.o`/backup files; `make clobber` — also remove built binaries
- No root required for `./cpu_info` (CPU detection only). Perf counters and `--tree` (ptrace) require root, or `CAP_SYS_PTRACE` + `perf_event_paranoid <= 1` — use `scripts/setup_perf.sh` to check/adjust `nmi_watchdog` and `perf_event_paranoid` sysctls.

Quick manual checks:
```
./cpu_info                          # CPU/core detection, no privileges needed
sudo ./wspy -- sleep 1              # basic run, default IPC counters
sudo ./wspy --csv --topdown -- true # CSV output with topdown metrics
```

## Architecture

**Data flow:** CLI (`wspy.c`) → CPU detection (`cpu_info.c`) → counter setup (`topdown.c`) → fork/exec
child with optional ptrace → periodic/final counter reads → CSV or human-readable output.

**Files and responsibilities:**
- `wspy.c` — `main()`, `parse_options()` (long/short option parsing into globals like `counter_mask`, `system_mask`, `aflag`/`sflag`/`csvflag`/`treeflag`), orchestrates setup → start → read → print lifecycle
- `topdown.c` (largest file, ~68K) — vendor event tables (`intel_raw_events[]`, `amd_raw_events[]`), `perf_event_open` wrapper, counter group constructors, `setup_counters`/`start_counters`/`read_counters`, all `print_*` formatters, `launch_child`/`ptrace_setup`/`ptrace_loop` (child launch + fork/exec/exit tracing), `check_nmi_watchdog`
- `cpu_info.c`/`cpu_info.h` — vendor/model detection (`inventory_cpu()`), core enumeration, hybrid-CPU handling (Intel Atom+Core mixes); defines `struct cpu_info`, `struct cpu_core_info`, the Intel/AMD raw perf-event format unions, and `struct counter_group`/`struct counter_info`
- `system.c` — system-wide metrics: load average, `/proc/stat` CPU time, `/proc/net/dev` network counters, AMD GPU busy % (if `AMDGPU=1`); `read_system()`/`print_system()`
- `amd_smi.c`/`amd_smi.h` — GPU metrics via ROCm's `libamd_smi` (only compiled with `AMDGPU=1`)
- `amd_sysfs.c`/`amd_sysfs.h` — GPU metrics (busy %, temp, activity, power, freq) read directly from sysfs, no ROCm dependency required at this layer but still gated by `AMDGPU=1`
- `proctree.c` — standalone `proctree` binary that parses the tree file produced by `wspy --tree` (not CSV: one of four line kinds — `<time> root <pid>`, `<time> start <pid> <ppid>`, `<time> exit <pid> <stat-fields>`, `<time> comm <pid> <name>` — see the comment at the top of `proctree.c`) and reconstructs the process hierarchy
- `error.c`/`error.h` — centralized logging: `fatal()`, `error()`, `warning()`, `notice()`, `debug()`, `debug2()`, gated by `set_error_level()`
- `rocm/` — separate small C++ utilities (`smi_monitor`, `smi_info`) with their own Makefile, exploring the ROCm SMI API directly (not linked into `wspy`)
- `workload/` — driver scripts for external benchmark suites (SPEC CPU2017, pbbsbench, Phoronix) used to exercise wspy against real workloads; not part of the build
- `archive/wspy2.0/` — old version of the tool kept for reference, not built or maintained

**Key data structures** (`cpu_info.h`):
- `struct cpu_info` — root descriptor: vendor/family/model, core count, `systemwide_counters` or per-core counters
- `struct counter_group` — linked list of related counters (IPC, topdown, branch, cache, ...), chained via `.next`
- `struct counter_info` — one perf counter: fd, label, config, running/enabled times, `is_group_leader` flag
- `struct raw_event` — vendor-specific descriptor (`intel_raw_events[]`/`amd_raw_events[]` tables in `topdown.c`), parsed by `parse_intel_event()`/`parse_amd_event()` at startup into a `.raw.config` value via the `union intel_raw_cpu_format`/`union amd_raw_cpu_format` bitfields

**Counter mask bits** (`wspy.h`): `COUNTER_IPC`, `COUNTER_TOPDOWN[2]`, `COUNTER_TOPDOWN_{FE,BE,OP}`, `COUNTER_BRANCH`, `COUNTER_{D,I,L1,L2,L3}CACHE`, `COUNTER_MEMORY`, `COUNTER_TLB`, `COUNTER_OPCACHE`, `COUNTER_SOFTWARE`, `COUNTER_FLOAT` — combined into the global `counter_mask`, set via CLI flags in `parse_options()`. `system_mask` (`wspy.h`: `SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK` by default, `system.c`) is always populated; `--system`/`-s` (`sflag`) just controls whether `print_system()` is called, while `--gpu-busy`/`--gpu-metrics`/`--gpu-smi` additionally OR in `SYSTEM_GPU` regardless of `sflag`.

**Child launch protocol** (`topdown.c`): parent creates a pipe and forks; if `--tree` the child calls
`PTRACE_TRACEME` and blocks on the pipe; the parent sets up and starts counters, sleeps 2s, then writes
`"start\n"` to the pipe; the child reads it and `execve`s the real workload. With `--tree`, the parent
runs `ptrace_loop()` (setting `PTRACE_O_TRACEFORK|TRACEEXIT`) to record fork/exit events with timestamps
to the tree file; otherwise it just `wait4()`s on the child.

**GPU support (`AMDGPU=1` only):** two independent GPU paths, both gated by `system_mask & SYSTEM_GPU`
and only invoked when explicitly requested via CLI flag:
- `--gpu-smi` → `amd_smi.c` (ROCm `libamd_smi` library; includes `<amd_smi/amdsmi.h>`, so `ROCM_DIR/include/amd_smi/amdsmi.h` and `-lamd_smi` must exist under `ROCM_DIR`)
- `--gpu-busy` / `--gpu-metrics` → `amd_sysfs.c` (direct sysfs reads: busy %, temp, activity, power, freq)
When built without `AMDGPU=1`, these flags are still recognized by the option parser but print a
"GPU support not built" warning instead of erroring, so the binary behaves consistently either way.

**CSV vs. human output:** every `print_*` function switches on `enum output_format` (`PRINT_NORMAL`,
`PRINT_CSV`, `PRINT_CSV_HEADER`). When adding a column, the header case and value case must be added
together and stay in the same order — `run_tests.sh` checks exact CSV column ordering (e.g.
`elapsed,utime,stime,gpu_busy,ipc`), so verify column position when adding new metrics.

## Common edits

**New raw perf counter:** add an entry to `intel_raw_events[]`/`amd_raw_events[]` in `topdown.c` with an
`event=...,umask=...` description string and the appropriate `COUNTER_*` mask from `wspy.h` (add a new
bit there if needed); the parser converts the description to `.raw.config` in `setup_raw_events()`. Add
a CLI flag pair (`--foo`/`--no-foo`) in `wspy.c:parse_options()` if it should be user-toggleable.

**New counter group:** write a constructor in `topdown.c` following the existing pattern (e.g.
`software_counter_group()`, `cache_counter_group()`), allocate `struct counter_group` + `struct
counter_info[]`, and link it into the list in `setup_counter_groups()` based on `counter_mask`. Each
check there *prepends* to `*counter_group_list` (`cgroup->next = *counter_group_list; *counter_group_list
= cgroup;`), so groups print in the reverse of the order they're checked in `setup_counter_groups()` —
place a new `if (counter_mask & COUNTER_FOO)` block earlier/later in that function to control where it
lands in output, and check nearby groups' relative order if you need it to appear next to a specific one.

**New system metric:** parse it in `system.c:read_system()`, store it in `struct system_state`, print it
in `system.c:print_system()` with matching CSV/header/normal cases, and add a `SYSTEM_*` bit + CLI flag
if it should be independently toggleable.

## Notable runtime behavior

- NMI watchdog: if `/proc/sys/kernel/nmi_watchdog` is active, one hardware counter is reserved system-wide; `check_nmi_watchdog()` in `topdown.c` detects this and reduces available counter slots accordingly rather than failing outright.
- AMD L3 cache events depend on `/sys/devices/amd_l3/type` and are silently skipped if absent.
- Vendor detection distinguishes AMD Zen/Zen5 and Intel Atom/Core (hybrid) cores; core-specific vs. system-wide counter setup depends on `aflag` (`--per-core`) and vendor/core type.
- **`amd_sysfs.c` hard-codes `/sys/class/drm/card1/device/...`** for `gpu_busy_percent` and `gpu_metrics` (not `card0`, not auto-detected). On a single-GPU machine where the AMD GPU enumerates as `card0` (e.g. no discrete display card ahead of it), `--gpu-busy`/`--gpu-metrics` will silently read nothing useful. If you need multi-GPU or differently-numbered systems to work, this needs to become a scan over `/sys/class/drm/card*/device/vendor` (AMD vendor ID `0x1002`) instead of a fixed path.
- `amd_sysfs_gpu_metrics()` branches on the `gpu_metrics` binary blob's `format_revision` (1/2/3, structs in `amd_sysfs.h`) since the layout is GPU-generation-dependent; unrecognized revisions are logged and ignored (`gpu_metrics_state.valid` stays 0, so callers must check `amd_sysfs_gpu_metrics_valid()` before reading temp/activity/power/freq). See `doc/sysfs.gemini.txt` for background on the blob format and where the kernel defines it (`kgd_pp_interface.h`).
