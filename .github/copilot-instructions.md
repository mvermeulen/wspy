This repository contains a small C utility (wspy) used to instrument a child
workload and collect runtime + performance counter metrics. The instructions
below are focused, actionable, and designed to help an automated coding
agent jump straight into meaningful edits.

Key files and architecture
- `wspy.c` — CLI, option parsing, launch/coordination of a traced child,
  CSV header generation and high-level orchestration (calls into counter
  setup/printing and system reads).
- `topdown.c` — all perf counter handling: event tables, parsing of raw AMD/
  Intel event descriptions, counter group constructors (`raw_counter_group`,
  `cache_counter_group`, `software_counter_group`), perf_event_open wrapper,
  printing logic (CSV and human-readable via print_* functions).
- `cpu_info.c` / `cpu_info.h` — CPU/vendor detection, core types, affinity and
  per-core availability. Many decisions (which counters to enable) depend on
  values computed here.
- `system.c` — /proc and /sys reads for load, CPU times, network, and (when
  built) GPU queries.
- `error.c` / `error.h` — centralized logging; use `warning()`, `error()`,
  `fatal()`, `debug()`, `debug2()` consistently when adding diagnostics.

Build, quick tests and environment
- Build: run `make` at the repo root. To include AMD GPU support set
  `AMDGPU=1` and ensure ROCm headers/libs are available: `make AMDGPU=1`.
- Binaries produced: `wspy`, `cpu_info`, `proctree` (and `gpu_info` if
  built). Quick smoke commands after build:
  - `./cpu_info` — exercises CPU detection paths.
  - `./wspy -- <cmd>` — runs `<cmd>` under instrumentation and prints CSV or
    human-readable output depending on flags.
- Privileges: many features (perf counters, ptrace) require root or proper
  perf/ptrace capabilities. Prefer local/manual smoke runs on a dev machine
  that can run perf (or use `cpu_info` to test detection logic without root).

Project-specific patterns and conventions
- Feature gating: GPU-specific code is guarded by the AMDGPU compile-time
  flag (see the `Makefile` and use of preprocessor guards). Follow this
  pattern for optional subsystems and preserve the existing guard style.
- Counter grouping: counters are represented by `struct counter_group` and
  linked lists. New counters should be added to the appropriate group via the
  helper constructors in `topdown.c` rather than ad-hoc arrays.
- Vendor splits: raw event tables are separated into `intel_raw_events` and
  `amd_raw_events`. Parsing differs (see `parse_intel_event` /
  `parse_amd_event`) — keep vendor-specific logic colocated with these
  tables.
- CSV vs human output: printing code has two modes. Add CSV column labels in
  the `PRINT_CSV_HEADER` cases inside the relevant `print_*` functions and
  provide matching values in `PRINT_CSV` paths.

Integration points and runtime assumptions
- /proc and /sys: `system.c` and `topdown.c` rely on specific files (e.g.
  `/proc/stat`, `/proc/loadavg`, `/proc/net/dev`, `/sys/devices/amd_l3/type`).
  Add checks and graceful fallbacks when introducing additional sysfs reads.
- perf_event_open: counters are opened with group leader semantics. When
  adding new raw counters, ensure `is_group_leader` and group_fd behavior
  mirrors existing code (see `setup_counters` for examples).
- ptrace: process tree recording uses PTRACE options (`ptrace_setup` and
  `ptrace_loop`) and expects traced children to call `PTRACE_TRACEME`. Be
  careful when modifying child launch semantics — the parent writes a start
  token into a pipe and the child waits on it before execve.

Concrete examples to guide edits
- Add a new raw topdown counter (AMD or Intel):
  1. Add an entry to `amd_raw_events` or `intel_raw_events` in `topdown.c`.
  2. Ensure the entry's `.use` mask includes the correct COUNTER_* flag
     defined in `wspy.h`.
  3. The parser (`parse_intel_event` / `parse_amd_event`) will convert the
     description into `.raw.config` during `setup_raw_events()` at startup.

- Add a CSV column: update the appropriate `print_*` function in `topdown.c`.
  Add the column label in the `PRINT_CSV_HEADER` branch and the matching
  value in the `PRINT_CSV` branch.

Where to change things
- CLI flags and orchestration: `wspy.c`.
- Counters and printing: `topdown.c` (primary). See helper functions named
  `print_ipc`, `print_topdown`, `print_branch`, `print_l2cache`, etc.
- CPU/platform logic: `cpu_info.c` and headers.
- System reads and CSV composition: `system.c`.

Quality and safety notes
- Many runtime checks already exist (file presence, fallbacks). Mirror their
  style when adding new sysfs/proc reads.
- Avoid changing group leader semantics for perf counters. Test on a
  machine with the target CPU (Intel vs AMD) and check `/proc/sys/kernel/nmi_watchdog` —
  when NMI watchdog is active, available perf counters are reduced and the
  code uses `nmi_running` to adapt.

If you want, I can: add a small example patch (e.g., add a sample counter and
CSV column), or extend guidance with a short checklist for adding GPU events.
Tell me which area you'd like expanded.
