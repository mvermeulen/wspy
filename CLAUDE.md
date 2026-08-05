# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

For design history/rationale/validation narratives, see `INVESTIGATION.md` (active backlog),
`doc/INVESTIGATION_ARCHIVE.md` (shipped write-ups), `doc/ARTIFACT_CONTRACT.md` (JSON schemas),
`doc/PROFILE_COOKBOOK.md` (reading verdict/confidence/phase output), `doc/NEW_WORKLOAD_COOKBOOK.md` (a
step-by-step tutorial for onboarding a new benchmark suite, end to end through the whole toolchain),
`doc/METRICS.md` (every metric's name/derivation/source-function/database status),
`doc/REPORT_HIERARCHY.md` (directory convention for curated `*.md` workload reports — root/suite/test/
test-point/machine levels; convention only as of 4.3, not yet wired to any tool), and
`git log`/`git blame`. This file covers *current*
mechanism/behavior only.

## Project overview

wspy ("workload spy") is a C instrumentation wrapper: it launches a child workload as a subprocess and
collects runtime + hardware performance counter metrics around it (IPC, topdown, cache, TLB, branch,
software counters), plus optional system-wide (`/proc`, `/sys`) and AMD/NVIDIA GPU metrics. Output is
CSV or human-readable. This is the author's internal research testbed — treat it as a small, pragmatic
C codebase rather than a polished library.

## Build & Test

- `make` — builds `wspy`, `cpu_info`, `proctree`, `wspy-validate`, `wspy-ledger`, `wspy-store`,
  `wspy-summary`, `wspy-plot`, `wspy-core-report`, `wspy-archetype` (default, no GPU support).
  `wspy-store`/`wspy-summary`/`wspy-archetype` link against system SQLite (`-lsqlite3`, needs
  `libsqlite3-dev` or equivalent). `wspy-plot` shells out to a `gnuplot` binary at runtime only (not a
  build dependency).
- `make AMDGPU=1 [ROCM_DIR=/path/to/rocm]` — adds AMD GPU support (`amd_smi`, `amd_sysfs`).
  `ROCM_DIR` auto-detects under `/opt/rocm` then `/usr`.
- `make NVIDIA=1` — adds NVIDIA GPU support (`nvidia_nvml`). No build-time header/lib dependency —
  `nvidia_nvml.c` `dlopen()`s `libnvidia-ml.so.1` at runtime. `AMDGPU`/`NVIDIA` are independent axes;
  both can be set together.
- `make test` — builds and runs the unit-style test binaries (`test_wspy`, `test_proctree`,
  `test_validate`, `test_ledger`, `test_ibs`, `test_power`, `test_phase`, `test_store`, `test_summary`,
  `test_plot`, `test_affinity`, `test_gpu_fusion`, `test_core_report`, `test_cgroup`, `test_archetype`).
  Most of these follow one convention: `test_X.c` directly `#include`s `X.c` (with a `-DTEST_X` macro
  when `X.c` has a real `main()` to stub out) so static/internal functions are testable without a
  separate build target, and several drive parsing logic against a fake sysfs tree under `/tmp/test_*`
  instead of needing real hardware. `test_wspy.c` additionally defines `AMDGPU=0 NVIDIA=0` so it never
  needs a GPU-enabled build of `topdown.o`/`wspy.o`.
- `./run_tests.sh` — full test script: builds, runs unit tests, integration-smoke-tests the real
  `wspy`/`proctree` binaries, runs `tests/golden_output.sh`, `tests/capability_matrix.sh`,
  `tests/wspy_queue_smoke.sh`, `tests/doc_version_check.sh`, and `tests/arm_topdown_microbench.sh`
  (self-skips off-ARM), then repeats the golden-output/capability-matrix checks against the AMDGPU build
  (if ROCm headers are found) and the NVIDIA build (if `libnvidia-ml.so` resolves via `ldconfig`).
- `tests/golden_output.sh` — pins the *shape* of `wspy` output: exact CSV header/column-order checks
  (vendor-gated), a generic column-count check across the flag matrix, a `--tree` grammar/footer check.
- `tests/capability_matrix.sh` — runs ~35 flag-bundle combinations, asserts graceful degradation (no
  `fatal error`/crash) except `--passes`' documented incompatibility checks, which must fail.
- `tests/doc_version_check.sh` — grep-based doc/version drift check (`*_SCHEMA_VERSION` mentions vs. real
  `#define`s; every `Makefile` binary mentioned in `README.md` and vice versa). No build required.
- `scripts/release_prep.sh` — release-prep checklist (branch/clean checks, merged-PR audit, version bump,
  stale-version grep, full test matrix, release-notes draft). Never runs public/hard-to-reverse steps
  itself (`git tag`/`push`, `gh release create` are print-only).
- `scripts/estimate_tree_timeout.py` — estimates a Phoronix workload's run time to size `wspy-run`'s
  `--tree` pass timeout; prints seconds and exits 0, or prints nothing and exits 1 (caller falls back to 3600).
- `./test_amd_smi.sh` / `./test_nvidia_nvml.sh` — focused GPU-module smoke tests (need matching build; NVIDIA one also needs real hardware).
- `make clean` — remove `.o`/backups; `make clobber` — also remove built binaries.
- No root required for `./cpu_info`. Perf counters and `--tree` (ptrace) need root, or `CAP_SYS_PTRACE` +
  `perf_event_paranoid <= 1` — use `scripts/setup_perf.sh` to check/adjust. `--power`'s RAPL access needs
  root or `CAP_PERFMON` specifically — `scripts/setup_perf.sh` also grants that via `setcap` on the
  `wspy` binary; it's a file capability tied to that exact binary's inode, so it's dropped on every
  rebuild and needs re-running.

Quick manual checks:
```
./cpu_info                          # CPU/core detection, no privileges needed
sudo ./wspy -- sleep 1              # basic run, default IPC counters
sudo ./wspy --csv --topdown -- true # CSV output with topdown metrics
```

Web launcher/report browser: `python3 web/server.py`, then open `http://127.0.0.1:8765/`. Stdlib-only,
no build step; not covered by `make`/`run_tests.sh`. See `web/` below.

## Development workflow

Starting with the 4.0 development cycle (see `INVESTIGATION.md`), new feature work happens on a
short-lived feature branch rather than committing directly to `master`. Small housekeeping changes
(typo fixes, doc tweaks, README updates) can still go straight to `master` — this applies to actual
feature/behavior changes, especially anything tracked in `INVESTIGATION.md`'s phase-tagged priority
lists.

- **Branch naming:** `feature/<slug>`, e.g. `feature/run-manifest`. One branch per inventory row/idea in
  `INVESTIGATION.md`, not one branch per phase, so each PR stays reviewable and revertable independently.
- **Starting a feature:** `git checkout master && git pull && git checkout -b feature/<slug>`.
- **While working:** commit normally; rebase or merge `master` in as needed, but don't rewrite already-
  pushed history.
- **Finishing:** push the branch and open a PR with `gh pr create` (origin is `github.com/mvermeulen/wspy`).
  Merge through GitHub — don't merge feature branches into `master` locally and push `master` directly.
- **Before opening the PR:** run `./run_tests.sh` (and `./test_amd_smi.sh`/`./test_nvidia_nvml.sh` for
  GPU-touching changes). Extend `tests/golden_output.sh`/`tests/capability_matrix.sh` when a change
  adds/reorders/removes a CSV column, rather than relying on manual checking alone. Update
  `doc/METRICS.md` too, for the same change — see "New CSV column / derived metric" under Common edits
  below; `tests/doc_version_check.sh` (part of `run_tests.sh`) only WARNs if a whole new metric-formatter
  *function* has no `doc/METRICS.md` mention at all, it can't catch a column added to an existing one.
- **Scope:** keep a feature branch to one inventory idea where practical; large phase-sized efforts
  should land as a series of small merged PRs, not one long-lived branch.

## Editor setup (VSCode: Claude Code / Cline / Copilot)

This repo is worked on with Claude Code, Cline, and GitHub Copilot interchangeably, so this file (not
tool-specific docs) is the canonical instructions source — `.clinerules` and
`.github/copilot-instructions.md` are one-paragraph pointers telling Cline/Copilot to read this file; if
you learn something worth persisting, update this file, not those. `.vscode/settings.json`/
`extensions.json` are checked in for consistent tooling across all three.

`make compile_commands.json` (or `./scripts/gen_compile_commands.py`) generates a compile database from
`make -Bn` dry runs (covers `AMDGPU=1` too if ROCm headers are found). Gitignored (absolute paths, per-
checkout); regenerate after Makefile changes or when IntelliSense/clangd diagnostics look stale.

## Architecture

**Data flow:** CLI (`wspy.c`) → CPU detection (`cpu_info.c`) → counter setup (`topdown.c`) → fork/exec
child with optional ptrace → periodic/final counter reads → CSV or human-readable output.

**Files and responsibilities:**

| File | Responsibility |
|---|---|
| `wspy.c` | `main()`, `parse_options()` (flags into globals: `counter_mask`, `system_mask`, `aflag`/`sflag`/`csvflag`/`treeflag`), orchestrates setup → start → read → print. `--passes=<list>` does native multi-pass counter execution (`multipass.c`); `--affinity=<spec>` (`affinity.c`) resolves placement pre-launch; `--counters=<list>` replaces the ~20 per-group boolean flags (soft-deprecated). |
| `topdown.c` | Largest file. Vendor event tables, `perf_event_open` wrapper, counter group constructors, `setup_counters`/`start_counters`/`read_counters`, all `print_*` formatters, `launch_child`/`ptrace_loop`, `check_nmi_watchdog`. `read_counters()` scales each counter by its multiplex ratio so values stay correct under oversubscription. `ptrace_loop()` tracks per-tracee syscall state (`struct ptrace_pid_entry`) for the `--tree-{futex,io-wait,io,schedstat,vmsize,connect,wait,poll,nanosleep}` flags. `print_topdown()`/`print_topdown_be()` emit an L1→L2 breakdown as trailing CSV columns. |
| `ptrace_arch.h` | Architecture-neutral ptrace register access. x86_64 is live; aarch64 is unverified prep; other arches fail at compile time. |
| `cpu_info.c`/`cpu_info.h` | Vendor/model detection, core enumeration, hybrid-CPU (Intel Atom+Core)/ARM topology; defines `struct cpu_info`/`cpu_core_info`/`counter_group`/`counter_info`. |
| `system.c` | System-wide metrics: load average, `/proc/stat`, per-interface network, per-block-device disk I/O (excludes loop/ram/zram), `/proc/meminfo`, AMD GPU busy %. |
| `amd_smi.c`/`amd_smi.h` | GPU metrics via ROCm `libamd_smi` (`AMDGPU=1`). Device index `-1` = device 0. |
| `amd_sysfs.c`/`amd_sysfs.h` | GPU metrics via direct sysfs reads, no ROCm dependency (`AMDGPU=1`). |
| `nvidia_nvml.c`/`nvidia_nvml.h` | NVIDIA GPU metrics via NVML (`NVIDIA=1`), `dlopen()`d at runtime, no build-time CUDA dep. |
| `proctree.c` | Parses the `--tree` output file into a process hierarchy. Many flags gate optional fields, present only if the matching `wspy --tree-*` flag was used. `--json` emits schema-versioned JSON; `--diff a.json b.json` structurally diffs two trees. |
| `error.c`/`error.h` | Centralized logging: `fatal()`/`error()`/`warning()`/`notice()`/`debug()`/`debug2()`. |
| `manifest.c`/`manifest.h` | Writes the optional JSON run manifest (`--manifest`). Bump `MANIFEST_SCHEMA_VERSION` MINOR (additive) / MAJOR (removed/renamed). |
| `run_index.c`/`run_index.h` | Appends one compact JSONL record per run (`--run-index`), `flock`-serialized. Own `RUN_INDEX_SCHEMA_VERSION`. |
| `json_util.c`/`json_reader.c` (+`.h`) | Write-side (string-escaping/timestamps, for `manifest.c`/`run_index.c`) and read-side (recursive-descent parser, for `validate.c` and downstream tools) JSON helpers. |
| `validate.c` | `wspy-validate`: pre-publish checks against a manifest — schema version, output existence, CSV shape, exit status, counter coverage, `sanity_bounds[]`. `[PASS]`/`[WARN]`/`[FAIL]`; `--strict` fails on WARN. |
| `ledger.c` | `wspy-ledger`: suite-level *workload* coverage (done/skipped/unsupported) inferred from `--run-index` files — distinct from `coverage.c`'s per-run counter coverage. |
| `store.c` | `wspy-store`: SQLite normalized store. Idempotently ingests `--run-index` JSONL into `runs` (+child tables), enriches from manifests, ingests CSV into long/tall `metric_values` (aggregate/`--interval`/`--per-core` handled uniformly via header-driven typing). `PRAGMA user_version` gates schema/migrations. `extract_run_features()` derives a fixed feature vocabulary for the archetype tools. |
| `summary.c` | `wspy-summary`: per-`(group,metric)` min/max/mean/median/stddev/outlier/CI95/verdict (`PASS`/`WARN:thin,noisy,mixed-pmu`) from the store. `--group-by`/`--group-by-option` bucket; `--trace` gives traceability back to manifest/CSV/plots/tree. `--run-id <hostname>:<run_id>` (repeatable) filters to an exact run set via a `TEMP TABLE`/`EXISTS` join, distinct from `--command`/`--hostname`'s substring/exact-column matches — needed since two runs can share identical command+hostname (a redo) and still need separate buckets; `wspy-testpoint aggregate` is the intended caller. Read-only. |
| `plot.c` | `wspy-plot`: matches a CSV header against built-in templates or a `--plot NAME=col1,col2` spec, renders PNGs via piped `gnuplot`. Column *identity* decides template membership. `--rundir <dir>` writes into `<dir>/plots`. |
| `core_report.c` | `wspy-core-report`: post-hoc per-core imbalance/hot-cold-core diagnostics over a `--per-core --csv` file, optional core-class breakdown. Must run on the collecting host's own topology. |
| `archetype.c` | `wspy-archetype`: classifies a run along 4 axes (`resource_dominance` top-2 ranked, plus `parallelism_shape`/`control_flow_style`/`runtime_stability`) with overall confidence. See `doc/PROFILE_COOKBOOK.md`. |
| `coverage.c`/`coverage.h` | Counter capability discovery + measured-vs-unavailable reporting. A failed `perf_event_open()` no longer aborts the run. `wspy --capabilities` is the standalone probe. |
| `preflight.c`/`preflight.h` | Estimates PMU-budget fit before any `perf_event_open()` call. `wspy --preflight [<flags>]` standalone; `main()` runs it too and warns if tight. |
| `multipass.c`/`multipass.h` | Testable half of `--passes`: bin-packs counter groups into budget-respecting passes, or one oversubscribed pass (`--multiplex`). `COUNTER_IBS` excluded. |
| `affinity.c`/`affinity.h` | Core/thread affinity. Discovers SMT siblings, L3-sharing domains, ARM `MIDR`-based core-type clusters; resolves spec grammar `all`/`thread=<id>`/`nosmt`/`domain=<id>`/`coretype=<id>`/`cpuset=<list>` against it. `--list-affinity` prints discovery. |
| `phase.c`/`phase.h` | Per-`--interval`-tick phase classification (`warmup`/`steady`/`degraded`) from IPC via a hysteresis state machine with EMA baseline drift. Gated on `interval && phase_flag && (counter_mask & COUNTER_IPC) && !aflag`. |
| `ibs.c`/`ibs.h` | AMD IBS: capability discovery plus `--ibs-basic`/`--ibs-memory-deep` profiles built from parsed sysfs format/location strings, not hardcoded offsets. System-wide only. |
| `power.c`/`power.h` | CPU energy via `power`/`power_core` perf PMUs. `--power` (default off, needs root/`CAP_PERFMON`). Per-core (`power_core`) needs `--power --per-core` together. `--passes` fatals against `--power`. First `--interval` tick's `pkg_joules` includes the pre-launch `sleep(2)` window; `pkg_watts` is always correct. |
| `provenance.c`/`provenance.h` | Best-effort environment capture (virtualization role, microcode, BIOS, governor, memory, toolchain), called once pre-manifest. Each field degrades independently. x86_64-only. |
| `cgroup.c`/`cgroup.h` | Best-effort cgroup v2 identity/limits/throttling — `cpu.max`/`cpu.weight`/`memory.max`/`memory.high` plus a `cpu.stat` throttling delta over the run. Every field degrades independently. |
| `wspy-run` | Bash profile-driven launcher (not in the C build). Runs named "passes" from a builtin profile (`quick`, `deep-cpu`, `deep-cpu-intel`, `deep-gpu`, `tree-heavy`, `ibs-basic`, `ibs-memory-deep`, `gpu-compute`, `zen-portable`, `zen4plus-deep`) or a `-c <file>` list; `--suite`/`--benchmark` writes the unified `<outdir>/<suite>/<benchmark>/<run-id>/` layout. Each pass is its own `wspy` invocation; `--affinity`/`--config-option` pass through to all of them. |
| `wspy-sweep` | Python sweep runner. Cross-products axis handlers (v1: only `"affinity"`) against workload commands via `wspy-run`, tagging each cell's swept value for later `--group-by-option` analysis. |
| `wspy-analyze` | Python local-LLM (Ollama) narrative analysis. Renders a run's already-computed numbers into a prompt, queries Ollama for prose — never re-derives classification. `--compare-rundir` for two-run comparison. |
| `wspy-bundle` | Python reproducibility bundle export. Tars a run directory's artifacts plus a checksummed `bundle_manifest.json` index; shared implementation with `web/server.py`'s download link. |
| `wspy-queue` | Python job queue. A job is a portable, spec-only JSON doc (no reference to the creating machine) moving `pending`→`running`→`done`/`failed` via atomic `os.rename()`, processed serially. Shares logic with `web/server.py` via `web/joblib.py`. |
| `wspy-publish`/`web/wp_client.py` | Python WordPress + report-root connectivity (4.3 Tier 3 static-site publishing, sub-steps 2-3). `wp_client.py` is a minimal stdlib-`urllib` WP REST client (Application Password Basic Auth, page lookup/create by `(slug, parent)`, `doc/REPORT_HIERARCHY.md`'s levels as nested Pages) shared for later reuse by `web/server.py`. `wspy-publish configure` writes `~/.config/wspy/publish.json` (mode 600, via `getpass`); `test-connection` proves WP auth/capabilities and report-root git access without building real report content yet. Sends the Application Password under both `Authorization` and a custom `X-WSPY-Authorization` header — some hosts drop the former before PHP sees it; `scripts/wp-auth-bridge.php` (installed as a plugin on the target WP site, not part of this repo's build) recovers the latter. See `doc/INVESTIGATION_ARCHIVE.md`'s "Non-obvious implementation traps" for why. Its report-root git plumbing (`ensure_report_root()`/`run_git()`, clone-if-missing/verify-remote, commits always local-only) lives in `web/report_root.py`, shared with `wspy-testpoint` below. |
| `wspy-testpoint` | Python run selection/role-assignment, aggregation, and README rendering for one test point + machine (`doc/REPORT_HIERARCHY.md`'s `<suite>/<test>/<test-point>/<machine>/`, INVESTIGATION.md's test-point README deep-dive, Tier 3 item 5 pieces 1-4). `select-runs` enumerates a test point's linked runs (reusing `web/server.py`'s `load_run_history_entry()`; Phoronix test/test-point identity resolved via `web/joblib.py`'s `list_materialized_phoronix_test_points()`), defaults each to a role — `stats-pool`/`supplementary`/`excluded`, plus one `primary` — from status and pass-set majority, and persists the result as `runs.json` under the report-root (via `web/report_root.py`), committed locally (never pushed). A human's `--set`/`--primary` override is recorded `human_set`/`primary_human_set` so a later re-run's default-heuristic pass never silently overwrites it — the same source-pointer-vs-commentary separation the report studio's `new_block()` established, applied to which runs feed a report rather than which artifacts within one run do. `aggregate` shells out to `wspy-summary --run-id <hostname>:<run_id>` (one per present `stats-pool` run, read-only-precheck against the store first — missing runs are warned about, never auto-ingested) rather than `--command`/`--hostname`, since those can't separate a redo from what it's redoing when they share identical command text. `render` parses `aggregate`'s own `--csv` output, buckets metrics by counter group (`web/joblib.py`'s `resolve_column_group()`), writes one generated markdown file per group under `sections/`, and reuses `web/server.py`'s curation-studio functions unmodified (`load_curation()`/`new_block()`/`export_block_content()`/`render_export_markdown()` — they only ever take a plain directory path) to maintain a `curation.json` and render `README.md` — a block for a section that already existed keeps its human-edited title/depth/commentary/position, only the section file's content refreshes. `commit_paths()` (shared by every subcommand's commit step) checks the git index directly rather than string-matching `git commit`'s own "nothing to commit" text, which varies with unrelated repo state (a real bug found and fixed during this piece's own testing). `render` also runs `wspy-archetype --run <hostname>:<run_id>` once per stats-pool run (no bulk filter needed — `--run` already names an exact run) and adds a "Workload characterization" section reporting whether `resource_dominance` agrees across the pool or diverges (the archetype cross-run stability fast-follow both `aggregate`/`render` originally deferred). |
| `web/` | `server.py`/`joblib.py`: stdlib `ThreadingHTTPServer` (`127.0.0.1` default) — tabbed Run/Validate/Store & Summary/Discovery launcher plus a report browser with curation, cross-run compare, tree viewer/diff, and publish-ready export. `joblib.py` holds argv builders/executors shared with `wspy-queue`; see in-code docstrings for the endpoint list. `report_root.py` and `wp_client.py` are thin shared clients (git and WordPress REST respectively) used by `wspy-publish`/`wspy-testpoint`. `machine_registry.py` (4.3 Tier 3 item 5) is a read-only hostname→machine-slug lookup, scanned from `scripts/publish_machine_page.py`'s existing `<report-root>/machine/<short-name>/machine.json` catalog — it never writes; the catalog script is the only place a hostname/slug association is recorded. The report page's "Publish test-point report" card (`render_testpoint_card()`, Tier 3 item 7) runs `wspy-testpoint select-runs` + `render` as a background-thread subprocess pipeline (`execute_testpoint_publish()`, its own `TESTPOINT_RUNS` SSE registry, same shape as the "AI narrative analysis" card's `ANALYZE_RUNS`) — `--report-root`/`--report-root-remote` CLI flags override the whole server instance's target clone (unset by default, so it falls through to `wspy-testpoint`'s own real-deployment default chain). |
| `rocm/` | Separate C++ ROCm SMI utilities (`smi_monitor`, `smi_info`); own Makefile, not linked into `wspy`. |
| `workload/` | Driver scripts for external benchmark suites (SPEC CPU2017, pbbsbench, Phoronix); call `wspy-run --suite <name> --benchmark <name>`. |
| `archive/wspy2.0/` | Old version, reference only — not built or maintained. |

**Key data structures** (`cpu_info.h`):
- `struct cpu_info` — root descriptor: vendor/family/model, core count, `systemwide_counters`/per-core counters
- `struct counter_group` — linked list of related counters (IPC, topdown, branch, cache, ...), via `.next`
- `struct counter_info` — one perf counter: fd, label, config, running/enabled times, `is_group_leader`
- `struct raw_event` — vendor-specific descriptor (`intel_raw_events[]`/`amd_raw_events[]` in `topdown.c`), parsed into a `.raw.config` bitfield

**Counter mask bits** (`wspy.h`): `COUNTER_IPC`, `COUNTER_TOPDOWN[2]`, `COUNTER_TOPDOWN_{FE,BE,OP}`,
`COUNTER_BRANCH`, `COUNTER_{D,I,L1,L2,L3}CACHE`, `COUNTER_MEMORY`, `COUNTER_TLB`, `COUNTER_OPCACHE`,
`COUNTER_SOFTWARE`, `COUNTER_FLOAT` — combined into the global `counter_mask`, set via CLI flags in
`parse_options()`. `system_mask` (`SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK` by default) is always
populated; `--system`/`-s` just controls whether `print_system()` is called, while the GPU flags OR in
`SYSTEM_GPU` regardless of `sflag`.

**Child launch protocol** (`topdown.c`): parent creates a pipe and forks; if `--tree` the child calls
`PTRACE_TRACEME` and blocks on the pipe; the parent sets up/starts counters, sleeps 2s, then writes
`"start\n"`; the child reads it and `execve`s the workload. With `--tree`, the parent runs `ptrace_loop()`
(`PTRACE_O_TRACEFORK|TRACEEXIT|TRACEEXEC`) to record fork/exec/exit events with timestamps; otherwise it
just `wait4()`s. `PTRACE_O_TRACEEXEC` turns a post-`execve()` trap into a decodable event instead of an
unclassifiable generic line.

**GPU support:** three paths across two independent build axes (`AMDGPU=1`/`NVIDIA=1`, either/both/
neither), gated by `system_mask & SYSTEM_GPU`, only invoked when explicitly requested:
- `--gpu-smi` (`AMDGPU=1`) → `amd_smi.c` (ROCm `libamd_smi`, linked at build time)
- `--gpu-busy`/`--gpu-metrics` (`AMDGPU=1`) → `amd_sysfs.c` (direct sysfs reads)
- `--gpu-nvidia` (`NVIDIA=1`) → `nvidia_nvml.c` (NVML, `dlopen()`d at runtime, `nv_`-prefixed columns so
  AMD+NVIDIA can coexist on one host)

Without the matching `AMDGPU=1`/`NVIDIA=1`, these flags still parse but warn "GPU support not built"
instead of erroring.

**CSV vs. human output:** every `print_*` function switches on `enum output_format` (`PRINT_NORMAL`,
`PRINT_CSV`, `PRINT_CSV_HEADER`). Add the header case and value case together, same order — `run_tests.sh`
checks exact CSV column ordering. Two pitfalls that have shipped as real bugs before: (1) the
`PRINT_CSV` value row must emit exactly the columns the header declared, with **every field
comma-terminated** (a bare `fprintf` with no trailing comma silently fuses into the next field); (2) the
value row must be emitted **unconditionally** — gating it behind a counter-value check (rather than only
vendor/group applicability) means a permission-denied run silently drops columns the header still claims.

## Common edits

- **New raw perf counter:** add an entry to `intel_raw_events[]`/`amd_raw_events[]` in `topdown.c`
  (`event=...,umask=...` + a `COUNTER_*` mask from `wspy.h`, adding a bit if needed); `setup_raw_events()`
  converts it. Add a `--foo`/`--no-foo` flag pair in `parse_options()` if user-toggleable. For an
  Intel E-core (Gracemont) equivalent, add to `intel_atom_raw_events[]` instead — its encodings
  routinely differ from `intel_raw_events[]`'s P-core ones even for the same event number (confirmed
  live: `l2_request.all` is `umask=0xff` on P-core, `umask=0x0` on Gracemont), so never assume the two
  tables can share a value; verify against a real `cpu_atom` PMU (`/sys/devices/cpu_atom/events/<name>`
  where a named event exists, else `perf stat -e cpu_atom/<name>/ -vv` to read the resolved `config`)
  rather than guessing.
- **New counter group:** write a constructor in `topdown.c` (pattern: `software_counter_group()`,
  `cache_counter_group()`), link it in `setup_counter_groups()` based on `counter_mask`. Each check
  *prepends* to `*counter_group_list`, so groups print in reverse check order — reorder the `if` blocks
  to control output position.
- **New system metric:** parse in `system.c:read_system()`, store in `struct system_state`, print in
  `print_system()` (CSV/header/normal cases), add a `SYSTEM_*` bit + flag if independently toggleable.
- **New manifest field:** add to `struct manifest_info` (`manifest.h`), populate in `wspy.c:main()`, emit
  in `manifest.c:write_manifest()`. Bump `MANIFEST_SCHEMA_VERSION` MINOR (additive) or MAJOR (removed/renamed).
- **New run-index field:** if it's already in `struct manifest_info`, just emit it in
  `run_index.c:append_run_index()`; index-only fields need their own plumbing. Bump
  `RUN_INDEX_SCHEMA_VERSION` independently of the manifest's.
- **New normalized-store field (`store.c`):** add the column to `SCHEMA_DDL`, bind it in the relevant
  `upsert_*`/`enrich_from_manifest()`/`ingest_csv_metrics()`, bump `STORE_SCHEMA_VERSION`. Also add a
  `MIGRATION_V<N-1>_TO_V<N>` DDL string + `ensure_schema()` dispatch branch — it runs *either* fresh
  `SCHEMA_DDL` *or* one migration step, never both, so `ADD COLUMN` never re-runs on an existing column.
- **New CSV column / derived metric (any of the above, plus `run_features`/IBS-sample/`wspy-core-report`
  additions):** add or update its entry in `doc/METRICS.md` — name (prefer the CSV/text name verbatim),
  derivation formula, source function, and the `[raw]`/`[feature]`/`[environment]`/`[manifest-only]`/
  `[human-only]`/`[categorical]` database-status tag. That file is the single index of every metric wspy
  produces, read by both people and AI agents summarizing wspy output — it's also how we're tracking
  which raw metrics deserve promotion into `run_features`. Guidance on what counts as high/low is only
  worth adding where it's genuinely meaningful (see that file's own intro), not for every metric.
- **New `wspy-summary --group-by` column:** a `runs` column needs a case in `group_by_column()` +
  `parse_group_by()` + a new `enum group_by` value (`e.<column>` for `run_environment`). Only for a small
  fixed set — an open-ended value belongs in `--group-by-option`'s `run_config_options` join instead,
  since the whitelist is what makes interpolating the result into raw SQL safe.
- **New `wspy-run` builtin profile:** add a `case` arm in `load_builtin_profile()` with a
  `PASS_NAMES`/`PASS_FLAGS` pair, list it in `BUILTIN_PROFILES`. Keep pass names filesystem-safe.
- **New `wspy-validate` sanity bound:** add to `sanity_bounds[]` (column + `{min,max}`) when the generic
  finite/non-negative/not-implausible rule isn't tight enough. `%`-cells use `PERCENT_SANITY_MAX`.
- **New AMD IBS filter (`ibs.c`):** don't hardcode a bit offset — look it up via
  `ibs_pmu_format(pmu,"<field>")` in `ibs_build_{fetch,op}_event()`; set `*_requested` unconditionally,
  only set `*_applied` if the lookup succeeds, so unsupported hardware degrades to "unfiltered" rather
  than failing. Surface it in `print_ibs()` too if it changes what counts as a "sample."

## Notable runtime behavior

- NMI watchdog: if active, one hardware counter is reserved system-wide; `check_nmi_watchdog()`
  (`topdown.c`) detects this and reduces available slots rather than failing.
- AMD L3 cache events depend on `/sys/devices/amd_l3/type` and are silently skipped if absent.
- Vendor detection distinguishes AMD Zen/Zen5, Intel Atom/Core (hybrid), ARM; on both ARM and Intel
  hybrid parts, `cpu_info.c` tracks PMU cluster/type per core (`coreinfo[i].pmu_type`, resolved from
  `/sys/bus/event_source/devices/<pmu>/type`) so `--per-core` binds raw counters to the correct PMU
  type. On Intel, `raw_counter_group()`/`setup_counter_groups()` (`topdown.c`) also take a `core_class`
  parameter so P-cores and E-cores (Gracemont) pull from separate raw event tables
  (`intel_raw_events[]`/`intel_atom_raw_events[]`) — Gracemont has no `slots`/fixed-counter register at
  all, so `print_topdown()` synthesizes one from `cpu-cycles * 5` when building an E-core's row.
- `amd_sysfs_initialize()` scans `/sys/class/drm/card*/device/vendor` for AMD cards rather than assuming
  a fixed `card1` (a multi-GPU machine can enumerate an NVIDIA card first). `--gpu-device=<idx>` selects
  a card/device for either AMD backend.
- `amd_sysfs_gpu_metrics()` branches on the `gpu_metrics` blob's `format_revision` (1/2/3); unrecognized
  revisions are logged/ignored — check `amd_sysfs_gpu_metrics_valid()` before reading (see
  `doc/sysfs.gemini.txt` for blob-format background).
- A `getopt_long` `val` collision that silently misrouted bad flags, and a `power` PMU dynamic-type
  collision that broke a hand-rolled capabilities probe, are written up in
  `doc/INVESTIGATION_ARCHIVE.md`'s "Non-obvious implementation traps" — read before adding a new
  long-only flag or a probe-without-a-full-run feature.
