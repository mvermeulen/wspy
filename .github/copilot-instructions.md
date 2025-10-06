The repository is a small C utility (wspy) for collecting runtime and
performance counter metrics for a child workload. Keep guidance brief and
actionable so an automated coding agent can be productive quickly.

1) Big picture
- Binary targets: `wspy`, `cpu_info`, `proctree`, optionally `gpu_info` (if built
  with `AMDGPU`). Core logic lives in `wspy.c` (CLI, orchestration),
  `topdown.c` (perf counter handling and print logic), and `cpu_info.*` (CPU
  inventory and platform quirks). `system.c` reads /proc and /sys for system
  metrics; `error.c` centralizes logging and verbosity.

2) Build / test / debug
- Build: `make` at the repository root. `Makefile` compiles with `gcc -g` and
  produces `wspy`, `cpu_info`, `proctree`. To include AMD GPU support set
  `AMDGPU=1` in the make invocation and ensure ROCm headers/libraries are
  available: `make AMDGPU=1`.
- Quick smoke: run `./cpu_info` (or `./cpu_info` after build) to exercise CPU
  detection paths. Run `./wspy -- <cmd>` to run under instrumentation.
- Logs / verbosity: use `-v`/`--verbose` to increase error subsystem level. The
  `error.c` helpers (warning, error, fatal, debug, debug2) gate output by level.

3) Project-specific patterns to follow
- Feature flags via macros: e.g., `#if AMDGPU` wraps GPU-specific code and the
  `Makefile` sets `AMDGPU`. Preserve existing guard style when adding GPU code.
- Counter groups: performance counters are grouped by `struct counter_group`.
  Use existing helper constructors `raw_counter_group`, `cache_counter_group`,
  `software_counter_group` in `topdown.c` when adding counters.
- Vendor-specific logic: tables and parsing differ for AMD vs Intel
  (see `topdown.c`’s `amd_raw_events` / `intel_raw_events` and parse_* helpers).
  Keep platform-specific code co-located with these tables.

4) Integration points & runtime assumptions
- Relies heavily on Linux /proc, /sys, perf_event, and ptrace. Changes that
  introduce new sysfs or proc reads should add graceful fallbacks (many places
  already check for file presence and warn).
- Performance counters open via `perf_event_open` system call wrappers.
  Changes must respect group leader semantics (see how `is_group_leader` and
  `group_id` are used when calling `perf_event_open`).

5) Where to make common edits
- CLI and orchestration: `wspy.c` (option parsing, child launch, CSV output).
- Counter definitions and printing: `topdown.c` (add/adjust events or output
  format). Example labels: `"instructions"`, `"cpu-cycles"`, `"op_cache_hit_miss.op_cache_miss"`.
- CPU detection / affinity: `cpu_info.c` / `cpu_info.h`.

6) Small examples
- Add a topdown counter: add an entry to `intel_raw_events` or `amd_raw_events`
  and ensure `use` mask matches one of the COUNTER_* flags defined in
  `wspy.h`. The code calls `parse_intel_event` / `parse_amd_event` to convert
  description strings to raw `config` values.
- To add a CSV column: update the print_* functions in `topdown.c` and ensure
  the header case (`PRINT_CSV_HEADER`) prints the new column label.

7) Safety & testing notes
- Many subsystems assume root or appropriate perf/ptrace privileges. Tests that
  exercise perf counters or ptrace should be isolated and may require elevated
  privileges; use `cpu_info` and `wspy` smoke runs to validate changes.

If anything here is unclear or you'd like more detail (examples of adding a
counter or a new CLI flag), tell me which area to expand and I will iterate.
