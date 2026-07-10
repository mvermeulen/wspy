# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

wspy ("workload spy") is a C instrumentation wrapper: it launches a child workload as a subprocess and
collects runtime + hardware performance counter metrics around it (IPC, topdown, cache, TLB, branch,
software counters), plus optional system-wide (`/proc`, `/sys`) and AMD GPU metrics. Output is CSV or
human-readable. This is the author's internal research testbed, published for portability across
machines — treat it as a small, pragmatic C codebase rather than a polished library.

## Build & Test

- `make` — builds `wspy`, `cpu_info`, `proctree`, `wspy-validate`, `wspy-ledger` (default, no GPU support)
- `make AMDGPU=1 [ROCM_DIR=/path/to/rocm]` — builds with AMD GPU support, also builds `amd_smi`, `amd_sysfs`. If `ROCM_DIR` isn't given, the Makefile auto-detects it by checking for `amd_smi/amdsmi.h` under `/opt/rocm` (the traditional ROCm install location) then `/usr` (where distro packages like Debian/Ubuntu's `rocm-smi-lib` install it), preferring `/opt/rocm` if both exist
- `make test` — builds and runs `test_wspy`, `test_proctree`, `test_validate`, and `test_ledger` (unit-style tests; `test_wspy.c` `#include`s `wspy.c` directly with `TEST_WSPY`/`AMDGPU=0` defined to stub out `main()` and disable GPU code — the `test_wspy` Makefile target always compiles its own objects with `-DAMDGPU=0`, independent of whatever the top-level `AMDGPU` value is, so it can't pick up a mismatched GPU-enabled `topdown.o` from the same build tree; `test_validate.c` and `test_ledger.c` follow the same `#include "validate.c"`/`#include "ledger.c"` + `-DTEST_VALIDATE`/`-DTEST_LEDGER` pattern to reach each tool's static check functions)
- `./run_tests.sh` — full test script: builds, runs unit tests, then integration-smoke-tests the real `wspy`/`proctree` binaries (CSV output, `--tree` output, network interface naming, GPU flag warnings when built without `AMDGPU=1`, `wspy-run` profile launching), and additionally exercises the AMDGPU build if ROCm headers are found under `/opt/rocm` or `/usr`
- `./test_amd_smi.sh` — focused smoke test for the `amd_smi` module (requires `AMDGPU=1` build)
- `make clean` — remove `.o`/backup files; `make clobber` — also remove built binaries
- No root required for `./cpu_info` (CPU detection only). Perf counters and `--tree` (ptrace) require root, or `CAP_SYS_PTRACE` + `perf_event_paranoid <= 1` — use `scripts/setup_perf.sh` to check/adjust `nmi_watchdog` and `perf_event_paranoid` sysctls.

Quick manual checks:
```
./cpu_info                          # CPU/core detection, no privileges needed
sudo ./wspy -- sleep 1              # basic run, default IPC counters
sudo ./wspy --csv --topdown -- true # CSV output with topdown metrics
```

## Development workflow

Starting with the 4.0 development cycle (see `INVESTIGATION_4.0.md`), new feature work happens on a
short-lived feature branch rather than committing directly to `master`. Small housekeeping changes
(typo fixes, doc tweaks, README updates) can still go straight to `master` — this applies to actual
feature/behavior changes, especially anything tracked in `INVESTIGATION_4.0.md`'s phased plan.

- **Branch naming:** `feature/<slug>`, e.g. `feature/run-manifest`, `feature/gpu-path-scan`. Prefer
  one branch per inventory row/idea in `INVESTIGATION_4.0.md` rather than one branch per phase, so
  each PR stays reviewable and revertable independently.
- **Starting a feature:** `git checkout master && git pull && git checkout -b feature/<slug>`.
- **While working:** commit normally on the branch; rebase or merge `master` into the branch as
  needed to stay current, but don't rewrite history that's already been pushed.
- **Finishing a feature:** push the branch (`git push -u origin feature/<slug>`) and open a PR with
  `gh pr create` (origin is already `github.com/mvermeulen/wspy`). Merge through GitHub once it's
  ready — don't merge feature branches into `master` locally and push `master` directly.
- **Before opening the PR:** run `./run_tests.sh` (and `./test_amd_smi.sh` if the change touches GPU
  code) — the branch should be in a state that passes the existing test suite, and CSV column-order
  contracts in particular (see "CSV vs. human output" below) should be checked by hand since they
  aren't all covered by `run_tests.sh` yet.
- **Scope:** keep a feature branch to one inventory idea where practical; large phase-sized efforts
  (e.g. all of "Run artifact foundation") should still land as a series of small merged PRs rather
  than one long-lived branch, to avoid the drift and merge pain of a big-bang branch.

## Editor setup (VSCode: Claude Code / Cline / Copilot)

This repo is worked on with Claude Code, Cline, and GitHub Copilot interchangeably, so this file (not
tool-specific docs) is the canonical instructions source — keep the others as thin pointers here rather
than letting per-tool copies drift (a full standalone `.github/copilot-instructions.md` previously did
exactly that, going stale enough to reference a `gpu_smi.c` file that no longer exists).

- `.vscode/settings.json` and `.vscode/extensions.json` are checked in so all three tools get consistent
  tooling in VSCode (recommends `GitHub.copilot`/`GitHub.copilot-chat`, `saoudrizwan.claude-dev` (Cline),
  `anthropic.claude-code`, plus `ms-vscode.cpptools`/`makefile-tools` for the C tooling below).
- Claude Code reads this file automatically; `.clinerules` and `.github/copilot-instructions.md` are
  one-paragraph pointers telling Cline/Copilot to read it too — if you learn something worth persisting,
  update this file, not those.
- `make compile_commands.json` (or `./scripts/gen_compile_commands.py` directly) generates a compile
  database from `make -Bn` dry runs — covers the AMDGPU=1 variant too if ROCm headers are found under
  `/opt/rocm` or `/usr`. It's gitignored (absolute paths, per-checkout); regenerate it after pulling
  Makefile changes or when IntelliSense/clangd diagnostics look stale.

## Architecture

**Data flow:** CLI (`wspy.c`) → CPU detection (`cpu_info.c`) → counter setup (`topdown.c`) → fork/exec
child with optional ptrace → periodic/final counter reads → CSV or human-readable output.

**Files and responsibilities:**
- `wspy.c` — `main()`, `parse_options()` (long/short option parsing into globals like `counter_mask`, `system_mask`, `aflag`/`sflag`/`csvflag`/`treeflag`), orchestrates setup → start → read → print lifecycle
- `topdown.c` (largest file, ~68K) — vendor event tables (`intel_raw_events[]`, `amd_raw_events[]`), `perf_event_open` wrapper, counter group constructors, `setup_counters`/`start_counters`/`read_counters`, all `print_*` formatters, `launch_child`/`ptrace_setup`/`ptrace_loop` (child launch + fork/exec/exit tracing), `check_nmi_watchdog`. `ptrace_loop()`'s syscall-stop handling reads the traced process's registers through `ptrace_arch.h`'s `wspy_regs_t`/`ptrace_getregs()`/`PTRACE_SYSCALL_NUM`/`PTRACE_SYSCALL_ARG2` rather than `struct user_regs_struct`/`PTRACE_GETREGS`/`.orig_rax`/`.rsi` directly, so it isn't x86_64-hardcoded (see `ptrace_arch.h` below).
- `ptrace_arch.h` — architecture-neutral ptrace register access: `wspy_regs_t` (typedef for `struct user_regs_struct`, whose layout differs per arch despite the shared name), `ptrace_getregs(pid,regs)`, and the `PTRACE_SYSCALL_NUM(regs)`/`PTRACE_SYSCALL_ARG2(regs)` macros `ptrace_loop()` (`topdown.c`) uses for the syscall number and `SYS_openat`'s pathname-pointer argument. The `INVESTIGATION_4.0.md` "Arch-neutral ptrace register-access macros" item — a mechanical refactor of existing x86_64 logic (`PTRACE_GETREGS`/`.orig_rax`/`.rsi`) behind `#ifdef __x86_64__`, done ahead of an actual ARM64 backend so the abstraction doesn't need retrofitting once more logic depends on the current shape. The `#elif defined(__aarch64__)` branch (`PTRACE_GETREGSET`+`NT_PRSTATUS`, syscall number/arg2 from `regs[8]`/`regs[1]`) is modeled on documented arm64 ptrace/syscall-ABI conventions but has never been built or run on real ARM64 hardware — unverified prep, not a working ARM64 backend. Building for any other architecture fails at compile time (`#error`), same as `cpu_info.c`'s `<cpuid.h>` use already does.
- `cpu_info.c`/`cpu_info.h` — vendor/model detection (`inventory_cpu()`), core enumeration, hybrid-CPU handling (Intel Atom+Core mixes); defines `struct cpu_info`, `struct cpu_core_info`, the Intel/AMD raw perf-event format unions, and `struct counter_group`/`struct counter_info`
- `system.c` — system-wide metrics: load average, `/proc/stat` CPU time, `/proc/net/dev` network counters, AMD GPU busy % (if `AMDGPU=1`); `read_system()`/`print_system()`
- `amd_smi.c`/`amd_smi.h` — GPU metrics via ROCm's `libamd_smi` (only compiled with `AMDGPU=1`)
- `amd_sysfs.c`/`amd_sysfs.h` — GPU metrics (busy %, temp, activity, power, freq) read directly from sysfs, no ROCm dependency required at this layer but still gated by `AMDGPU=1`
- `proctree.c` — standalone `proctree` binary that parses the tree file produced by `wspy --tree` (not CSV: one of four line kinds — `<time> root <pid>`, `<time> start <pid> <ppid>`, `<time> exit <pid> <stat-fields>`, `<time> comm <pid> <name>` — see the comment at the top of `proctree.c`) and reconstructs the process hierarchy
- `error.c`/`error.h` — centralized logging: `fatal()`, `error()`, `warning()`, `notice()`, `debug()`, `debug2()`, gated by `set_error_level()`
- `manifest.c`/`manifest.h` — writes the optional JSON run manifest (`--manifest <file>`): command line, start/finish timestamps, child exit status (when known — not available in `--tree` mode since `ptrace_loop` reaps children itself), host/CPU info, the option flags used, and the list of output files produced. `MANIFEST_SCHEMA_VERSION` in `manifest.h` is the SemVer of the manifest's JSON *shape*, independent of `WSPY_VERSION_MAJOR`/`MINOR` (now in `wspy.h`); bump it when fields are added/removed/renamed. This is the run *record*, not a run *configuration* — the lightweight config-file form of a run configuration is `wspy-run` (below); the full config-driven experiment-definition system remains a separate `INVESTIGATION_4.0.md`/4.3 item.
- `run_index.c`/`run_index.h` — appends one compact JSON Lines (JSONL) record per run to a shared file (`--run-index <file>`), independent of `--manifest` (a run can use either, neither, or both — if both are given, the index record's `output_files.manifest_path` points at the manifest file for that run). Built from the same `struct manifest_info` populated in `wspy.c:main()`, but versioned independently via `RUN_INDEX_SCHEMA_VERSION` since it's a leaner, line-oriented projection of a run, not the manifest itself. This is what item 2 of the `INVESTIGATION_4.0.md` "minimal foundation slice" calls "run index generation" — it lets tooling query "all runs" by scanning one file instead of walking output directories. Appends are serialized with `flock(LOCK_EX)` so concurrent `wspy` processes sharing an index file don't interleave records.
- `json_util.c`/`json_util.h` — `json_write_string()` (escaping) and `format_iso8601()` (timestamp formatting) shared by `manifest.c` and `run_index.c`, the two hand-rolled JSON emitters in the tree.
- `json_reader.c`/`json_reader.h` — small recursive-descent JSON parser (`json_parse()`/`json_parse_file()` build a `struct json_value` tree; `json_free()`; `json_object_get()`/`json_array_get()`/`json_array_len()` plus `json_get_string()`/`json_get_number()`/`json_get_bool()` convenience accessors that return a default rather than asserting on a missing key or type mismatch). The read-side counterpart to `json_util.c`'s write helpers, and the first JSON *reader* in the tree — written for `validate.c`, not schema-specific to it.
- `validate.c` — `wspy-validate` binary: basic pre-publish quality checks against a `--manifest`-produced `manifest.json` (`INVESTIGATION_4.0.md`'s "Run artifact foundation" track, "Basic validation / quality checks pre-publish" item). Given one or more manifest paths, checks (independently, so one failure doesn't hide the others): the manifest's `schema_version` major version is one this build understands; every path in `output_files` exists; the `output` file is non-empty and, when `options.csv` is true, that every CSV row has the same column count as the header; `exit_status` reflects a clean exit (nonzero/signaled is a failure, `known: false` is a warning, mirroring `--exit-with-child`'s own "exit status not always known" caveat); `counter_coverage.measured < requested` is a warning, not a failure (partial coverage is `coverage.c`'s designed-in graceful degradation, not by itself a bad run); `timing.elapsed_seconds` is positive; and a small extensible `sanity_bounds[]` table (currently just `ipc`) plus a generic "numeric CSV column must be finite, non-negative, and not implausibly large" rule catch runaway/garbage values in known and unknown columns alike (percent-suffixed cells like `"26.61%"` get a looser bound since multi-core aggregate percentages legitimately exceed 100%). Reports each check as `[PASS]`/`[WARN]`/`[FAIL]`; exits 0 only if every manifest had zero `FAIL`s (`--strict` also fails on any `WARN`); `-q`/`--quiet` suppresses the full report for manifests with no `FAIL`. Scope is deliberately one manifest at a time, not a whole run index or output tree — that's the separate `ledger.c`/"coverage ledger" tool below.
- `ledger.c` — `wspy-ledger` binary: the "coverage ledger" item from `INVESTIGATION_4.0.md`'s "Run artifact foundation" track (workload status: done/skipped/unsupported/needs-tool-support, generated from the run index instead of hand-maintained). Not to be confused with `coverage.c`, which is per-run hardware *counter* coverage — this is suite-level *workload* coverage across many runs, scanned back out of one or more `--run-index` (JSONL) files via `json_reader.c`, the read side wspy-validate already introduced. Given a workload-list file (one workload name per line, optionally with a tab-separated `unsupported`/`needs-tool-support` annotation plus a free-text note) it matches each name as a substring against every run-index record's `command` array and infers a status: no matching record → `skipped`; at least one matching record with a clean exit → `done`; matching record(s) but none succeeded → `needs-tool-support`. An explicit annotation in the workload list always wins over inference (for cases known in advance, e.g. a workload that needs a GPU-enabled build not available on this host, mirroring the "Intel not supported" early-exit `workload/cpu2017/run_test.sh` already hand-rolls). Prints a human report or `--csv`; `--strict` exits 1 if anything is `skipped`/`needs-tool-support`.
- `coverage.c`/`coverage.h` — counter capability discovery + "measured vs unavailable" coverage reporting (`INVESTIGATION_4.0.md`'s "minimal foundation slice" item 4). `topdown.c:setup_counters()` calls `coverage_note()` for every counter it attempts to open via `perf_event_open`, success or failure, accumulating the global `coverage_requested`/`coverage_measured` counts and a `coverage_entries` list. A failed counter no longer aborts the run (`setup_counters()` used to call `fatal()` on any single `perf_event_open` failure — this made partial counter loss, e.g. from `perf_event_paranoid`/NMI-watchdog contention, indistinguishable from "wspy doesn't work here"); it's now just logged and reflected in coverage instead, so a run degrades gracefully rather than producing no output at all. `print_counter_coverage()` prints a concise "N/M measured" summary (plus a gap list in human output; `counters_measured,counters_requested` columns in CSV) alongside a real run's metrics, and the same counts/gaps are written into `--manifest`'s `counter_coverage` object and `--run-index`'s leaner `counter_coverage` counts-only field. `wspy --capabilities` is the standalone discovery half: it needs no workload command, forces `counter_mask` to `COUNTER_ALL` (ignoring any other counter flags — probing everything is the point), and prints `print_capability_report()` (every counter, available or not) before exiting.
- `provenance.c`/`provenance.h` — best-effort environment/provenance capture (`INVESTIGATION_4.0.md`'s "Reproducibility, comparability, statistics" track: host/guest role, kernel, microcode, BIOS/power, governor, memory profile, tool versions). `provenance_collect()` is called once in `wspy.c:main()` right before `write_manifest`/`append_run_index`, populating a `struct provenance_info` with the CPU's virtualization role (`cpuid` leaf 1's hypervisor-present bit, plus the leaf `0x40000000` vendor string when a guest), microcode version (`/proc/cpuinfo`), BIOS vendor/version/date (`/sys/class/dmi/id/*`), `cpu0`'s frequency governor/scaling driver (plus a `cpu_governor_uniform` flag checked against every other online core), total memory (`sysinfo(2)`), and the toolchain that built this binary (`__VERSION__`, `gnu_get_libc_version()`). Each of the 9 `PROVENANCE_TRACKED_FIELD_COUNT` fields degrades to unavailable-with-reason independently rather than failing the run — mirrors `coverage.c`'s "measured vs unavailable" pattern, per the same doc's recommendation to treat environment fields as optional-but-recommended rather than blocking publishability. `manifest.c` emits the full field set plus a gap list under `environment`/`environment_coverage`; `run_index.c` emits the same fields in its leaner compact form plus counts-only coverage, mirroring how `counter_coverage` is handled in each. x86_64-only (`cpuid`), like the rest of this codebase's CPU-vendor detection (`cpu_info.c`).
- `wspy-run` — bash profile-driven launcher wrapping `wspy`, not part of the C build (no compilation, just `chmod +x`). Runs one or more named "passes" (each a separate `wspy` invocation with its own flag set) against a single workload command, either from a builtin profile (`quick`, `deep-cpu`, `deep-gpu`, `tree-heavy`) or a `-c <file>` pass-list config file, writing each pass's output to `<outdir>/<prefix><pass-name>.<csv|txt>` (extension picked by whether that pass's flags include `--csv`). This is the "Common workload wrapper / profile-driven launcher" `INVESTIGATION_4.0.md` item — it collapses the repeated multi-invocation command lines historically hand-written in `workload/*/run_test.sh` (see below) into reusable profiles, and it's the lightweight precursor to the full config-driven experiment system (4.3), not that system itself. `--manifest-dir`/`--run-index` pass those `wspy` flags through per-pass, reusing `manifest.c`/`run_index.c` rather than inventing a second run-record format. Each pass is a separate `wspy` process — native single-process multi-pass execution (running several counter groups within one launch of the workload) is a distinct, not-yet-built `INVESTIGATION_4.0.md` item ("Native multi-pass counter execution", 4.1). Note `wspy`'s own exit status reflects whether `wspy` itself ran, not the launched command, unless `--exit-with-child` is passed (opt-in, since existing scripts may rely on `wspy` always exiting 0 regardless of the child — it exits with the child's own exit code, or 128+signal if the child was killed by a signal, using `child_exit_known`/`child_exit_code`/`child_signaled`/`child_term_signal` declared in `wspy.h` and populated by `wspy.c`'s `wait4()` path or `topdown.c`'s `ptrace_loop()` for `--tree`). `wspy-run` passes do not set this flag today, so a pass's own exit code still can't be used to detect workload-command failure — only `--manifest`/`--run-index` `exit_status` can, unless the flag is added to a pass's flag string by hand.
- `rocm/` — separate small C++ utilities (`smi_monitor`, `smi_info`) with their own Makefile, exploring the ROCm SMI API directly (not linked into `wspy`)
- `workload/` — driver scripts for external benchmark suites (SPEC CPU2017, pbbsbench, Phoronix) used to exercise wspy against real workloads; not part of the build. Each `run_test.sh` currently hand-rolls the same 7-8 `wspy` invocations that `wspy-run`'s `deep-cpu`/`tree-heavy` profiles now express declaratively — migrating these scripts to call `wspy-run` is a natural follow-up but hasn't been done yet, so don't assume the two are already in sync.
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

**New manifest field:** add it to `struct manifest_info` in `manifest.h`, populate it at the call site in
`wspy.c:main()` (near the end, guarded by `if (manifest_path || run_index_path)`), and emit it in
`manifest.c:write_manifest()`. Adding a field is a backward-compatible change — bump the MINOR component
of `MANIFEST_SCHEMA_VERSION`; removing or renaming one is a MAJOR bump, since existing readers may depend
on the old shape.

**New run-index field:** most fields come from the same `struct manifest_info` used by the manifest (see
above), so if the field already exists there just emit it in `run_index.c:append_run_index()` too. A
field that's specific to the index record (not the manifest) needs its own plumbing through
`append_run_index()`'s signature. Either way, bump `RUN_INDEX_SCHEMA_VERSION` in `run_index.h` (MINOR for
an added field, MAJOR for removed/renamed) — it's versioned independently of `MANIFEST_SCHEMA_VERSION`.

**New `wspy-run` builtin profile:** add a `case` arm in `load_builtin_profile()` in `wspy-run` with a
`PASS_NAMES`/`PASS_FLAGS` array pair (one entry per pass, flags as a single space-separated string), and
add it to the `BUILTIN_PROFILES` list at the top of the file so `--list`/`--help` pick it up. Keep pass
names filesystem-safe (they become part of the output filename) and don't rely on flag ordering within a
pass beyond what `wspy`'s `getopt_long` already tolerates.

**New `wspy-validate` sanity bound:** add an entry to `sanity_bounds[]` in `validate.c` (column name plus
a `{min,max}` pair) when a specific CSV column deserves a tighter check than the generic "finite,
non-negative, not implausibly large" rule every numeric column already gets. Percent-suffixed cells
(e.g. `"26.61%"`) are matched by their trailing `%`, not by column name, and use the separate
`PERCENT_SANITY_MAX` bound instead of this table.

## Notable runtime behavior

- NMI watchdog: if `/proc/sys/kernel/nmi_watchdog` is active, one hardware counter is reserved system-wide; `check_nmi_watchdog()` in `topdown.c` detects this and reduces available counter slots accordingly rather than failing outright.
- AMD L3 cache events depend on `/sys/devices/amd_l3/type` and are silently skipped if absent.
- Vendor detection distinguishes AMD Zen/Zen5 and Intel Atom/Core (hybrid) cores; core-specific vs. system-wide counter setup depends on `aflag` (`--per-core`) and vendor/core type.
- `amd_sysfs_initialize()` scans `/sys/class/drm/card*/device/vendor` for the lowest-numbered card with AMD's vendor ID (`0x1002`) via `find_amd_drm_card()`, rather than assuming a fixed `card1` path — on a multi-GPU machine (e.g. an NVIDIA card enumerated as `card1` ahead of the AMD card at `card2`) the old hardcoded path silently read nothing; the scan fixes that. If no AMD card is found, `--gpu-busy`/`--gpu-metrics` warn and read zero rather than erroring. Multi-GPU AMD systems still only use the lowest-numbered match — per-device selection (`--gpu-device=<idx>`) is a separate, not-yet-built follow-on (`INVESTIGATION_4.0.md`).
- `amd_sysfs_gpu_metrics()` branches on the `gpu_metrics` binary blob's `format_revision` (1/2/3, structs in `amd_sysfs.h`) since the layout is GPU-generation-dependent; unrecognized revisions are logged and ignored (`gpu_metrics_state.valid` stays 0, so callers must check `amd_sysfs_gpu_metrics_valid()` before reading temp/activity/power/freq). See `doc/sysfs.gemini.txt` for background on the blob format and where the kernel defines it (`kgd_pp_interface.h`).
