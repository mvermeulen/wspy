# wspy - workload spy

A C utility for instrumenting child workloads and collecting runtime + performance counter metrics.
This guide helps AI coding agents make productive edits immediately.

## Architecture Overview

**Data flow:** CLI (`wspy.c`) → CPU detection (`cpu_info.c`) → counter setup (`topdown.c`) → child launch with ptrace → periodic reads → CSV/human output

**Core subsystems:**
- **CLI & orchestration** (`wspy.c`): option parsing, launches child process, coordinates counter lifecycle (setup → start → read → print)
- **Performance counters** (`topdown.c`): event tables for Intel/AMD, `perf_event_open` wrapper, counter group management, output formatters
- **CPU detection** (`cpu_info.c/.h`): vendor/model identification, core enumeration, hybrid CPU handling; drives counter selection
- **System metrics** (`system.c`): `/proc` and `/sys` readers for load, CPU time, network, GPU (if `AMDGPU=1`)
- **Process tree** (`topdown.c:ptrace_loop`): traces fork/exec/exit via ptrace for hierarchical process tracking
- **Logging** (`error.c/.h`): centralized diagnostics with `fatal()`, `error()`, `warning()`, `debug()`, `debug2()`

**Key data structures:**
- `struct counter_group`: linked list of counter groups (e.g., IPC, topdown, branch, cache); created by `*_counter_group()` constructors in `topdown.c`
- `struct counter_info`: individual counter metadata (fd, label, value, last_read, time_enabled); lives within `counter_group`
- `struct raw_event`: vendor-specific event descriptors in `intel_raw_events[]` and `amd_raw_events[]` tables; parsed at startup by `parse_intel_event()` / `parse_amd_event()`
- `struct cpu_info`: root CPU descriptor; holds `systemwide_counters` or per-core `core_specific_counters` depending on `aflag`

**Execution flow:**
1. Parse CLI (`wspy.c:parse_options`) and set `counter_mask` bits (e.g., `COUNTER_IPC`, `COUNTER_TOPDOWN`)
2. Detect CPU (`inventory_cpu()`) and check NMI watchdog status (`check_nmi_watchdog()`)
3. Parse event tables (`setup_raw_events()`) to convert vendor descriptions → perf config values
4. Build counter groups (`setup_counter_groups()`) based on enabled masks; call `setup_counters()` and `start_counters()`
5. Fork child, optionally attach ptrace (`ptrace_setup()`), write start token to pipe, child execs
6. If `--tree`: parent runs `ptrace_loop()` handling fork/exit events; else waits on child
7. If `--interval`: timer fires periodically (`timer_callback`) to read/print counters mid-run
8. On child exit: final counter read, `print_metrics()` outputs CSV or human-readable format

## Build & Test

**Standard build:** `make` produces `wspy`, `cpu_info`, `proctree`
**AMD GPU build:** `make AMDGPU=1` (requires ROCm headers in `/opt/rocm`)

**Quick smoke tests:**
- `./cpu_info` — CPU detection (no root needed)
- `sudo ./wspy -- sleep 1` — basic run with IPC counters (default)
- `sudo ./wspy --csv --topdown -- myapp` — CSV output with topdown metrics

**Privileges:** perf counters and ptrace require root or `CAP_SYS_PTRACE` + `perf_event_paranoid ≤ 1`. Test counter availability on target hardware (Intel vs AMD).

## Project-Specific Patterns

**Feature gating:** GPU-specific code uses the AMDGPU compile-time flag (see `Makefile`, `system.c`, `wspy.h`). GPU metrics go through `gpu_smi.c` and ROCm's `amd_smi` library.

**Vendor-specific counters:** Intel and AMD use different event formats (unions in `cpu_info.h`). When adding counters:
- Add to `intel_raw_events[]` or `amd_raw_events[]` in `topdown.c`
- Set `.use` mask (e.g., `COUNTER_TOPDOWN|COUNTER_BRANCH`) defined in `wspy.h`
- Parser (`parse_intel_event` / `parse_amd_event`) converts `"event=0xc0"` → `.raw.config` during `setup_raw_events()`

**Counter grouping:** Counters are linked lists managed by constructors:
- `software_counter_group()` — page faults, context switches
- `cache_counter_group()` — L1/L2/L3 cache events
- `raw_counter_group()` — vendor-specific raw events from event tables
- Groups are chained via `.next` pointer (see `setup_counter_groups()` in `wspy.c`)

**Perf group leaders:** First counter in a group has `is_group_leader=1`; subsequent counters pass leader's fd as `group_fd` to `perf_event_open`. See `setup_counters()` in `topdown.c` for reference.

**CSV vs human output:** All `print_*` functions (e.g., `print_ipc`, `print_topdown`, `print_branch`) handle three modes:
- `PRINT_CSV_HEADER` — column labels
- `PRINT_CSV` — comma-separated values
- `PRINT_NORMAL` — formatted human output
Match header/value order exactly when adding columns.

**Process tree tracking:** `--tree` flag enables ptrace-based recording. Child calls `PTRACE_TRACEME` before exec; parent sets options (`PTRACE_O_TRACEFORK|TRACEEXIT`) and loops on `wait4()` to capture fork/exit events. Output format in `treefile`: `<time> <event> <pid> [details]` (see `ptrace_loop()` in `topdown.c`).

**NMI watchdog handling:** When `/proc/sys/kernel/nmi_watchdog` is active, one hardware counter is reserved. Code adapts by setting `nmi_running` and reducing available counters (see `check_nmi_watchdog()` in `topdown.c`).

## Common Edits

**Add a new raw counter:**
1. Identify vendor (Intel/AMD) and event description (e.g., `"event=0x1a0,umask=0x1"`)
2. Add to `intel_raw_events[]` or `amd_raw_events[]` in `topdown.c`:
   ```c
   { "my_event", "event=0x...,umask=0x...", PERF_TYPE_RAW, COUNTER_TOPDOWN, {0} },
   ```
3. Ensure `COUNTER_*` flag exists in `wspy.h` or reuse an existing one
4. Add CLI flag in `wspy.c:parse_options()` if needed

**Add a CSV column:**
1. Find the appropriate `print_*` function in `topdown.c` (e.g., `print_topdown`)
2. Add header case: `case PRINT_CSV_HEADER: fprintf(outfile, "my_column,"); break;`
3. Add value case: `case PRINT_CSV: fprintf(outfile, "%lu,", my_value); break;`
4. Maintain order consistency between header and value outputs

**Add a system metric:**
1. Add read logic in `system.c:read_system()` (parse `/proc` or `/sys` file)
2. Store value in `system_state` struct
3. Add print logic in `system.c:print_system()` with CSV/header cases
4. Add `SYSTEM_*` flag to `wspy.h` and CLI option to `wspy.c`

**Add a new counter group:**
1. Create constructor in `topdown.c` (pattern: `my_counter_group(char *name)`)
2. Allocate `struct counter_group`, set `.label`, `.type_id`, `.ncounters`, `.mask`
3. Allocate `struct counter_info` array, set `.label`, `.config`, `.corenum`, `.is_group_leader`
4. Link into group list in `setup_counter_groups()` based on `counter_mask` check

## Integration Points

**sysfs/procfs dependencies:**
- CPU stats: `/proc/stat`, `/proc/loadavg`
- Network: `/proc/net/dev`
- AMD L3: `/sys/devices/amd_l3/type` (checked at runtime, gracefully skipped if missing)
- Perf: `/sys/devices/cpu_core/format` (describes Intel counter fields)

**External libraries:** ROCm's `libamd_smi` when `AMDGPU=1` (see `gpu_smi.c`)

**Child launch protocol:**
1. Parent creates pipe, forks child
2. Child (if `--tree`): calls `ptrace(PTRACE_TRACEME)`, waits on pipe read
3. Parent: sets up counters, sleeps 2s, writes `"start\n"` to pipe
4. Child: reads pipe, calls `execve(argv[0])` to replace itself with workload

**Timing:** Uses `clock_gettime(CLOCK_REALTIME)` for absolute timestamps. Process tree events use elapsed time since `start_time`.

## Testing & Debugging

**Check counter availability:** Run on target hardware; missing counters print warnings. AMD L3 events require `/sys/devices/amd_l3/type`; some Intel events need specific CPU models.

**Debug flags:** Set `vflag=1` (verbose) or call `set_error_level(ERROR_LEVEL_DEBUG2)` to enable debug prints.

**Verify CSV consistency:** Headers must match value order. Test with `--csv` and parse output with `proctree` utility.

**Validate ptrace:** Enable with `--tree <file>`; check tree file for `fork`/`exit` events. Child must support ptrace (some containers/sandboxes block it).

**NMI watchdog:** If counters fail with ENOSPC, check `/proc/sys/kernel/nmi_watchdog`. Disable (`echo 0 > ...`) or code adapts automatically via `nmi_running` flag.
