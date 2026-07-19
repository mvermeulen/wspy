# wspy Investigation

A rolling roadmap: what's shipped, what's actively planned, and the design reasoning behind both.
Formerly `INVESTIGATION_4.0.md` — renamed once its content outgrew a single release (4.0/4.1 are done,
most of 4.2 has shipped too; see below). Full design write-ups and validation narratives for work
that's fully shipped now live in `doc/INVESTIGATION_ARCHIVE.md`, out of the way of the open backlog.

Status (2026-07-19): **4.0 and 4.1 are released and done.** 4.2 is in progress — most of its scope has
already shipped (see "Shipped since 4.1" below); the remaining backlog is in "4.2 — remaining work."
4.3 and 4.4 are planned, not started.

## Purpose
This document captures ideas for improvements focused on making benchmark collection, organization,
and publication easier and more repeatable.

## How to use this document
- "What shipped in 4.0" / "What shipped in 4.1" / "Shipped since 4.1" are pointer lists, not feature
  logs — `CLAUDE.md` documents each module's actual behavior in detail, `doc/INVESTIGATION_ARCHIVE.md`
  holds full design/validation write-ups for fully-shipped work, and `git log` has history. Don't
  restate mechanism here, link to it.
- **When an item ships:** move it out of its phase's open backlog and into that phase's "Shipped"
  rollup as a one- or two-sentence pointer (name the file/tool, not the mechanism). If its design
  merited a multi-paragraph write-up while it was being built, move that write-up to
  `doc/INVESTIGATION_ARCHIVE.md` rather than leaving it inline — the open backlog should only ever
  contain open work.
- **Cross-references are by name, not number.** Item numbers inside a single tier list are fine as a
  local index, but don't reference an item elsewhere in this file (or from `CLAUDE.md`/commit
  messages) as "4.2 #27" — describe it by name instead ("AMD IBS sampling-mode support"). Numbers shift
  every time a tier is reorganized; names don't.
- "4.2 — remaining work" / "4.3 priorities" / "4.4 priorities" are ordered backlogs, one per phase,
  grouped into dependency tiers (earlier tiers unlock later ones within the same phase). Add or
  reorder an item there rather than inventing a parallel table.
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, topdown, the
  preset/configuration/option vocabulary). Each points back at the priority-list items it informs.
  Deep-dives for work that has since fully shipped live in `doc/INVESTIGATION_ARCHIVE.md`, not here.
- "Open questions" carry a recommendation each; re-open one by editing its entry, not by appending new
  prose elsewhere in the file.

## What shipped in 4.0
Grouped by the same track names `CLAUDE.md` and PR history use, so existing cross-references still
resolve. This is a pointer list — see `CLAUDE.md`'s entry for each named file for actual behavior.

**Run artifact foundation:** run manifest + SemVer schema (`manifest.c`, `MANIFEST_SCHEMA_VERSION`);
run index (`run_index.c`, JSONL, `RUN_INDEX_SCHEMA_VERSION`); profile-driven launcher (`wspy-run`:
builtin profiles including `deep-cpu-intel`, `-c <file>` config execution, comma-composed profiles,
per-pass timeouts); pre-publish validation (`wspy-validate`/`validate.c`); coverage ledger
(`wspy-ledger`/`ledger.c`); unified output layout (`wspy-run --suite/--benchmark`, `doc/
ARTIFACT_CONTRACT.md`'s "Unified output layout" section); `workload/cpu2017`, `workload/phoronix`,
and `workload/pbbsbench` migrated to call `wspy-run` instead of hand-rolling per-suite invocations.

**Reproducibility, comparability, statistics:** counter capability discovery + coverage reporting
(`coverage.c`, `wspy --capabilities`); environment/provenance capture (`provenance.c` — virt role,
microcode, BIOS, governor, memory, toolchain).

**Topdown quality:** confidence envelope + decomposition sanity checks on the level-1 breakdown
(`topdown.c`'s `print_topdown()`).

**Zen5 / IBS:** capability-driven IBS probing (`ibs.c`, readdir-driven sysfs discovery); `ibs-basic`/
`ibs-memory-deep` collection profiles with sampling-skew/quality annotations.

**Process / `getrusage` / `/proc` telemetry:** CSV/normal output parity fix for `print_usage()`;
expanded `getrusage` coverage (`maxrss`/`minflt`/`majflt`/`nswap`).

**Existing-capability extensions:** counter-fit preflight (`preflight.c`, `wspy --preflight`);
interval automatic phase-boundary detection (`phase.c`, `phase` CSV column + boundary summary).

**Portability and robustness:** opt-in child exit status propagation (`--exit-with-child`);
arch-neutral `ptrace` register access (`ptrace_arch.h` — both x86_64 and `__aarch64__` branches
are fully verified and validated on real hardware); run-index schema validation on ingest
(`wspy-ledger`); collector-plugin schema seam (`manifest.h`/`run_index.h`'s `collector` field,
default `"wspy"` — no non-wspy collector implementation exists yet, that's real 4.3+ scope).

**AMD GPU track:** dynamic GPU path scan (`amd_sysfs_scan_devices()`, replacing the old `card1`
hardcode); `--gpu-device=<idx>` override + full multi-GPU enumeration across both the sysfs and SMI
backends. GPU *kernel*-level instrumentation (CUDA/Vulkan profiling — tracing individual compute
kernels/shaders, not point-in-time busy%/VRAM monitoring) is **cut from this roadmap** (project scope
decision, 2026-07-08) — this codebase has no CUDA/Vulkan profiling code; revisit only if the project's
mission changes to include GPU kernel-level profiling.
**Revised 2026-07-18:** that decision was specifically about kernel-level instrumentation, not
cross-vendor GPU *monitoring* — the narrower, AMD-parity capability (busy%/VRAM via a vendor
management API, exactly what `amd_smi.c`/`amd_sysfs.c` already do for AMD) shipped as `--gpu-nvidia`
(`nvidia_nvml.c`, `NVIDIA=1` build flag): NVML is `dlopen()`d at runtime rather than linked at build
time, so unlike the AMD path there's no ROCm-equivalent header/toolkit dependency at build time. See
`CLAUDE.md`'s "GPU support" section and `nvidia_nvml.c`'s entry in the Architecture list for detail.

**Testing and documentation:** golden output-contract tests + capability-matrix smoke tests
(`tests/golden_output.sh`, `tests/capability_matrix.sh`) — building these surfaced and fixed five
independent, pre-existing output-contract bugs and one crash (see `CLAUDE.md`'s "Build & Test" for
specifics, not repeated here); `doc/ARTIFACT_CONTRACT.md` artifact-contract doc + troubleshooting
runbook. `--per-core` CSV column-count mismatch fixed: `wspy.c`'s `per_core_csv` re-architects the
aflag/csv print flow into one row per active core (a `core` column identifies which), so header and
row column counts now match like any other flag combination; `--per-core` combined with `--interval`
still keeps the old, separately-caused mismatch (`timer_callback()` never reads per-core counters —
see `wspy.c`'s `per_core_csv` comment and `doc/ARTIFACT_CONTRACT.md`'s CSV section).

## What shipped in 4.1
Grouped the same way as "What shipped in 4.0" above — a pointer list, not a feature log. See
`CLAUDE.md`'s entry for each named file/tool for actual behavior, and git history for how each piece
evolved.

**Normalized store & reporting:** canonical SQLite schema + idempotent ingest (`wspy-store`/`store.c`,
`STORE_SCHEMA_VERSION` — run catalog plus a long/tall `metric_values` table parsed from each run's
CSV, covering aggregate/`--interval`/`--per-core` shapes uniformly); summary table generator
(`wspy-summary`/`summary.c` — min/max/mean/median/stddev/outlier flags per `(group,metric)` bucket,
grouped by command/hostname/`cpu_vendor`, `--show-runs`/`--trace` traceability back to
manifest/CSV/tree/plot artifacts); shared plotting templates (`wspy-plot`/`plot.c` — gnuplot-rendered
PNGs from any wspy CSV via column-identity matching, replacing the old per-suite `gnuplot.sh` script,
plus `--plot`/`--only-custom` for hand-picked groupings).

**Multi-pass counter execution:** `wspy --passes=<groups>` (`multipass.c`) bin-packs requested
counter groups into automatically-sized passes and merges the result into one CSV/manifest/run-index
record instead of requiring N separate `wspy` invocations; `--multiplex` trades that bin-packing for
a single oversubscribed pass. Paired with a correctness fix: `read_counters()` (`topdown.c`) now
scales every counter's raw *value* (not just its confidence envelope) by that read's multiplex ratio,
so an oversubscribed run no longer silently undercounts.

**Web launcher & report browser (`web/server.py`):** Run/Validate/Store & Summary/Discovery tabs; a
preset-dropdown-or-configuration-checklist launcher with live SSE-streamed output and an
always-shown, copy/paste-able command line; a report page with a curation studio (reorderable,
per-block + whole-report commentary, `none`/`summary`/`excerpt`/`full` inclusion depth) and a
multi-run compare view; publish-ready export (WordPress block markup / self-contained HTML /
Markdown); a historical run browser/search page (`/history`); an estimated-runtime "Check" button
(perf/`nmi_watchdog` sysctls, Phoronix runtime estimates, and a real AMD IBS `perf_event_open()`
probe before launching).

**Structured configuration provenance:** `--preset-name`/`--config-name`/`--config-option <k>=<v>`
(`wspy.c`, metadata-only) record which named preset/configuration/option a front end chose, threaded
through `manifest.h`/`run_index.h`'s `configuration_provenance` object
(`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` `1.4.0` → `1.5.0`); the report browser's
"customize & run again" reads it back to restore exact launcher state (preset or full checklist) from
a report instead of re-parsing a flat command line.

**Deployment / job queue:** a portable, spec-only **job** file format (`web/joblib.py`'s
`build_job()`/`validate_job()`) captured before any run directory exists, processed through a
`pending`/`running`/`done`/`failed` lifecycle by `wspy-queue` (standalone CLI, headless, no
dependency on the web server) or created from the Run tab's "Queue instead of running it now"
checkbox; a job file can be copied to a second machine with wspy checked out and processed there.

## Shipped since 4.1 (4.2 in progress)
4.2 as a phase isn't closed yet — see "4.2 — remaining work" below for what's still open — but enough
has landed that leaving it as scattered inline "done" markers through a priority list stopped being
readable. This section will fold into a proper "What shipped in 4.2" once that phase's backlog is
empty. Full design/validation detail for each item below lives in `doc/INVESTIGATION_ARCHIVE.md`.

**Critical-path / synchronization-latency instrumentation:** all six originally-scoped syscall-latency
candidates shipped — `--tree-futex`, `--tree-io-wait` (paired with `--tree-io`'s `/proc/<pid>/io` byte
counters), `--tree-connect`, `--tree-nanosleep`, `--tree-wait`, `--tree-poll` (`topdown.c`'s
`ptrace_loop()`, `proctree.c`'s matching `-X`/`-B`/`-I`/`-K`/`-J`/`-L`/`-Z` toggles), plus
`--tree-schedstat` (run-delay/timeslice, `-D`) and `--tree-vmsize` (peak RSS/RSS composition/swap,
`-R` — repurposing a long-dead no-op flag rather than adding a new one). Together these give a
degraded interval phase a three-way explanation: blocked in the kernel (futex/io-wait), runnable but
not scheduled (`run_delay`), or a genuine on-CPU hardware stall (neither).

**Core/thread affinity control:** `--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|
cpuset=<c0,...>` (`affinity.c`) — SMT/L3-domain/core-type topology discovery, `--list-affinity`,
manifest/run-index provenance, `wspy-run`/web launcher/`wspy-queue` wiring. Shipped ahead of its
originally-planned phase.

**AMD IBS:** real-Zen5-hardware validation surfaced and fixed a MaxCnt/`sample_period` bug that had
silently broken every IBS counter since the feature shipped; `ibs-basic`/`ibs-memory-deep` now default
to `--interval 1` and render real gnuplot PNGs (`plot.c`'s `ibs`/`ibs-accepted-ratio` templates); the
web launcher's Check button gained a live `perf_event_open()` probe, not just a sysfs-presence check.

**CPU energy/power:** `--power`/`--no-power` (`power.c`, new `COUNTER_POWER` bit) reports package
`pkg_joules`/`pkg_watts` via the `power`/`power_core` perf PMUs — dedicated web launcher card,
custom-plot column autofit, and a live `EACCES`-aware Check-button probe with remediation guidance.

**Combined GPU-workload profiling:** `wspy-run gpu-compute` builtin profile (tree tracing + system +
power + both GPU backends + topdown on one shared `--interval` timeline, for workloads a
separate-re-execution-per-category profile like `deep-gpu` can't correlate tick-for-tick); surfaced
and fixed two pre-existing CSV correctness bugs along the way (GPU columns silently dropped whenever
`--system` was also requested; per-tick print order not matching the CSV header when a counter group
was combined with a GPU flag under `--interval`); `cpu_temp` system metric (`SYSTEM_TEMP`); GPU-aware
plot templates (`gpu-utilization`/`gpu-vram`/`gpu-thermal`/temp-pairing charts) + stable per-metric
line colors; dual process-tree output (`process.tree.simple.txt` alongside the annotated summary);
web launcher GPU-build verification in the Check button.

**ARM64:** real CPU topology/topdown/ptrace support (not just register-access prep) — `cpu_info.c`
fallback inventory, PMU-cluster discovery, ARM raw-event topdown decomposition, per-core PMU-type
binding — validated on real ARM64 hardware.

**Local LLM (Ollama) narrative analysis:** `wspy-analyze` turns already-computed/already-validated
numbers into prose via a local model — classification-by-code/narration-by-model, versioned prompt
templates, multi-model sweep + prompt critique, curation-studio integration with an always-visible
"AI-generated" marker, comparative mode (`--compare-rundir`).

**Testing:** `wspy-store`'s schema-migration/idempotency test coverage (`test_store.c`) closed the
"reproducibility/idempotency tests" item.

**Data-quality fix found via real use:** `wspy-ledger` no longer permanently misreports a workload's
status after its output directory is deleted (PR #81) — it now checks recorded artifact paths against
disk and degrades stale-but-matched records back toward `skipped`, tracked separately as `runs_stale`.

**`--gpu-smi --interval` CSV column-count fix:** `timer_callback()` (`topdown.c`) never read `amd_smi`
per tick, only in aggregate mode, so a periodic `--gpu-smi --interval` row was silently missing the 4
columns (`gpu_smi_temp`/`gpu_smi_activity`/`gpu_smi_vram_used`/`gpu_smi_vram_total`) the header claims.
Fixed to mirror `wspy.c`'s aggregate/tail-row `gpu_smi_requested` block exactly, positioned between
`gpu_metrics` and NVIDIA to match column order. Confirmed live (header/row column counts now match)
and via `tests/golden_output.sh`/`tests/capability_matrix.sh` against an `AMDGPU=1` build.

**`--interval` tail-print/last-tick signal race fixed:** `wspy.c` now blocks `SIGALRM` and disarms the
periodic timer (`sigprocmask`/`alarm(0)`) as the very first thing after the blocking wait for the child
returns, before any of the final tail row's `fprintf()` calls — `is_still_running=0` alone only stopped
the *next* re-arm, it didn't retract a `SIGALRM` the kernel had already queued, so a signal delivered
partway through the tail row let `timer_callback()` (`topdown.c`) splice a full periodic row (including
its own trailing newline) into the middle of it. **Validation note, stated plainly:** three escalating
black-box reproduction attempts (natural near-boundary timing, external `SIGALRM` injection around
child exit, sustained injection across the whole process lifetime — several dozen trials, thousands of
signal deliveries each) did not trigger the malformed-line symptom even on the pre-fix binary, so this
fix is verified by code-level reasoning and the full test suite (`make test`, `tests/golden_output.sh`,
`tests/capability_matrix.sh`, all passing unchanged) rather than by an empirical repro of the race
itself — consistent with this codebase's existing precedent for inherently-timing-dependent features
(futex/io-wait/schedstat validation, `doc/INVESTIGATION_ARCHIVE.md`) not having a golden-output-style
test. The narrowed window (a handful of instructions between the wait returning and the
`sigprocmask()` call) is not claimed to be mathematically zero.

**`deep-gpu` now carries `--power`:** `wspy-run`'s `deep-gpu` systemtime pass was missing `--power`
even though it's the exact same zero-hardware-counter shape as `deep-cpu`'s systemtime pass (which
already carried it) — a pre-existing asymmetry, not a deliberate difference. Added, matching
`deep-cpu` exactly; also fixed `web/server.py`'s `POWER_PRESET_NAMES` (the Check button's power probe
had been silently skipping `deep-gpu`) and `web/joblib.py`'s `PROFILE_PLOTTABLE_COLUMNS` (so a custom
plot referencing `pkg_joules`/`pkg_watts` under `deep-gpu` doesn't spin up a redundant supplementary
pass for data the profile's own systemtime pass already collects).

**Web launcher custom GPU checklist card gained an NVIDIA checkbox:** the "GPU metrics" checklist card
(`web/server.py`'s Run tab) only exposed AMD's `--gpu-busy`/`--gpu-metrics`/`--gpu-smi` checkboxes, so a
custom (non-preset) run had no way to opt into `--gpu-nvidia`; only presets that hardcode it
(`gpu-compute`) got NVIDIA data. Added the missing checkbox, wired through `web/static/app.js`'s
request-body builder and `web/joblib.py`'s `build_configuration_passes()` (`web/server.py`'s
`gpu_flags_for_request()`/`check_gpu_build()` were already forward-compatible — a stale defensive
comment there is now accurate rather than aspirational). The existing "Device index" field stays
AMD-only (`--gpu-device`); NVIDIA always uses its default device (`--gpu-nvidia-device` has no
checklist field yet), noted inline in the card.

**AMD IBS filtered-vs-unfiltered validation, and a real bug it surfaced:** attempting the
long-carried-forward "compare filtered vs. unfiltered IBS sample distributions on real hardware"
validation immediately hit `--ibs-memory-deep`'s filtered `ibs_op` counter failing to open
(`errno=22`/`EINVAL`) on real Zen5 hardware (family 1a model 70). A bit-by-bit `perf_event_open()`
sweep of the `ldlat` config field (bypassing wspy entirely) found a clean, reproducible threshold:
every value 100–127 is rejected, every value ≥ 128 succeeds — the kernel enforces a real minimum
load-latency threshold of 128 for `ibs_op`. `ibs.h`'s `IBS_DEFAULT_LDLAT_THRESHOLD` was **120**,
below that minimum, so every `--ibs-memory-deep` run that didn't explicitly override `--ibs-ldlat`
had been silently failing to open the filtered counter (degrading to 2/3 measured — not a fatal
error, so this went unnoticed). Fixed: default bumped to 128. `IBS_DEFAULT_FETCHLAT_THRESHOLD`
(also 120) is deliberately left unchanged — no hardware available during this validation exposed a
working `fetchlat` sysfs format field on `ibs_fetch` to test the same way (fetch-latency and
op-load-latency are different hardware mechanisms, not assumed to share a minimum; see "Known gaps"
below). With the fix, the originally-requested comparison itself now works: a deliberately
cache-unfriendly pointer-chase workload (256MB randomized permutation cycle, defeats prefetching)
showed `ibs_op_accepted_ratio` averaging ~6.8% across 3 trials (0.0630/0.0662/0.0750) versus ~2.6%
for an idle `sleep` baseline (0.0425/0.0190/0.0176) over 3 trials each — non-overlapping ranges,
confirming the l3missonly+ldlat filter's accepted-ratio signal genuinely tracks real memory-bound
behavior rather than just sampling noise.

**`wspy-validate`/`wspy-ledger` exercised at accumulated real scale, including interrupted runs and
mixed schema versions:** built up a real `--run-index`-accumulated file (100+ genuine `wspy` runs,
mixed successful/failing/varied workloads) rather than relying only on `test_ledger.c`/
`test_validate.c`'s small synthetic fixtures. **Interrupted runs:** a process killed well before
reaching the manifest/run-index write phase leaves no trace (clean, expected) across 150 trials at
randomized early-startup timing; a further ~250 trials with a precise `clock_nanosleep`-timed
`SIGKILL` deliberately swept across the exact `sleep(2)`-pre-launch-boundary/record-write window
(1995ms–2030ms, then a tight 2000–2010ms 1ms-increment sweep) — every resulting run-index line
across the whole accumulated file (100+ real records, several kill-mid-flight attempts included)
remained valid JSONL with zero corruption, consistent with `run_index.c`'s buffered-then-single-flush
write pattern being effectively atomic in practice for typical record sizes (not claimed
mathematically provable, same epistemic honesty as the `--interval` signal-race validation above).
**Mixed schema versions:** hand-stamped real records with a same-major/older-minor version (1.4.0,
1.0.0, predating structured configuration provenance/affinity) were silently tolerated with no
warning, exactly as designed ("a MINOR/PATCH bump only adds fields"); a genuinely different-major
version (2.0.0) triggered `wspy-ledger`'s one-time-per-distinct-version warning (not per-record,
confirmed via 2 records producing exactly 1 warning) without affecting `--strict`'s exit code; a
record with no `schema_version` field at all triggered its own one-time warning; a hand-truncated
malformed JSON line was correctly skipped with a line-numbered error rather than aborting the rest
of the file. The manifest-level equivalent (`wspy-validate` against 5 manifests spanning current/
old-minor/major-mismatch/missing-field/truncated variants in one batch) behaved identically:
major-mismatch is `WARN` not `FAIL`, missing-field is `FAIL` ("doesn't look like a wspy manifest"),
truncated JSON fails with a precise parse-error location, and every other manifest in the batch
still gets a full, independent report. No bugs found — this validation confirms existing designed
behavior rather than fixing anything, unlike the IBS validation above.

**GPU multi-device enumeration/selection exercised on a real multi-GPU host, and a real bug it
surfaced:** built `AMDGPU=1 NVIDIA=1` and ran against a real laptop with both an AMD iGPU (Strix
880M/890M, sysfs `card2`) and an NVIDIA dGPU (RTX 5070 Laptop GPU) present simultaneously.
`--gpu-device=2`/`--gpu-nvidia-device=0` (the correct indices) select the right card on each backend
and report real, distinct data (`gpu_*` and `nv_*` CSV columns coexisting on the same row, confirmed
under `--interval` too); an out-of-range index (`--gpu-device=1`, the NVIDIA card's DRM index — not
an AMD one) and a nonexistent NVIDIA index (`--gpu-nvidia-device=1`, only device 0 exists) both
degrade gracefully (logged error, zero-valued columns, run continues) rather than crashing or
silently reading the wrong device. Running both GPU backends' counters alongside real IPC/topdown
hardware counters on the same run confirmed no interaction between the GPU and PMU collection paths.
Surfaced one real bug along the way: `wspy --capabilities`' AMD sysfs device list never showed which
device was selected (unlike the AMD SMI/NVIDIA NVML lists right next to it in the same report),
because `run_capabilities_probe()` (`wspy.c`) called `amd_sysfs_print_capability_report()` without
ever calling `amd_sysfs_initialize()` first — the other two GPU backends' probes were paired
correctly, this one wasn't. Fixed by adding the missing `amd_sysfs_initialize(-1)`/
`amd_sysfs_finalize()` pair around the print call, matching the `amd_smi_*`/`nvidia_nvml_*` pattern
immediately below it. Also confirms/records a real-hardware finding that is not a wspy bug: on this
specific AMD Strix APU, the ROCm `amd_smi` backend's `gpu_metrics` blob query
(`amdsmi_get_gpu_metrics_info`) fails with `AMDSMI_STATUS_UNEXPECTED_DATA` (43) — `--gpu-smi`'s
`gpu_smi_temp`/`gpu_smi_activity` degrade to 0 as designed while `gpu_smi_vram_used`/
`gpu_smi_vram_total` (a separate ROCm query) still succeed; the plain-sysfs backend
(`--gpu-busy`/`--gpu-metrics`) is unaffected and reports real temp/activity/power/freq for the same
card. Confirmed via `./test_amd_smi.sh`/`./test_nvidia_nvml.sh` and the full `./run_tests.sh` matrix
(default + `AMDGPU=1` + `NVIDIA=1` builds), all passing.

**Repeatability policy + confidence metadata:** `wspy-summary` (`summary.c`) now reports a 95%
confidence interval of the mean (`ci95_low`/`ci95_high`, Student's t) and a repeatability verdict
(`PASS`/`WARN:thin`/`WARN:noisy`/`WARN:thin,noisy`) alongside its pre-existing mean/stddev/CV, all
default output — "thin" reuses the existing `n>=3` outlier-flagging threshold, "noisy" is a new
`--max-cv` flag (default 5.0%); `--strict` now also fails on any `WARN` verdict, matching
`wspy-validate`'s own `--strict` convention. See `doc/INVESTIGATION_ARCHIVE.md`'s "Concrete design:
repeatability policy + confidence metadata" for the full design, including the documented caveat that a
workload wrapping its own multi-trial benchmark harness (Phoronix, SPEC) can trigger `WARN:noisy` from
the harness's own internal repeat-count variance as much as from real measurement noise.

**Comparison matrix mode:** `wspy-summary` can now group by `affinity_mode`/`preset_name`/
`config_name`/`cpu_governor`/`virt_role` (previously invisible in the store despite being ingested
since 4.0/4.1) and compose a second, arbitrary grouping axis via `--group-by-option <name>` (a
`--config-option` key, parameterized not interpolated); `store.c` gained the ingestion these rely on
(`preset_name`/`config_name`/`affinity_*` columns, a new `run_config_options` child table,
`STORE_SCHEMA_VERSION` 2→3). New `wspy-run --config-option <k>=<v>` (repeatable, top-level passthrough
like `--affinity`) and a new tool, `wspy-sweep`, cross-product `--affinity=<spec>` values (covering
SMT on/off, L3-domain placement, and core-type comparisons via one generic axis handler) against one
or more workloads, tagging each cell for later comparison via `wspy-summary --group-by-option`.
Deliberately scoped down from the original "sweep compiler/kernel/governor/SMT/VM-native" backlog line
to just the axis wspy can actually control in one sitting — compiler/kernel/governor/VM-native are
uniform, human-supplied context tags, never swept; see `doc/INVESTIGATION_ARCHIVE.md`'s "Concrete
design: comparison matrix mode" for the full design and the scope rule ("only automate axes that are
process-scoped and side-effect-free outside the measured run") governing what could ever be added here.

## Known gaps (still open)
Real-hardware/real-scale validation this project's hand-testing hasn't covered yet. Not release
blockers — just don't assume these are confirmed:
- **AMD IBS `fetchlat` threshold minimum unverified:** unlike `ibs_op`'s `ldlat` field (see above,
  now fixed), no host available during that validation exposed a working `fetchlat` sysfs format
  field on `ibs_fetch` to test whether it has an analogous hardware-enforced minimum below
  `IBS_DEFAULT_FETCHLAT_THRESHOLD`'s current value of 120. Worth the same bit-sweep treatment once
  hardware with a live `fetchlat` field is available.

## Track deep-dives
Reasoning that doesn't fit a single backlog line, for tracks with genuinely open work. Deep-dives for
work that's since fully shipped (blocking I/O, schedstat, vmsize, connect/wait/poll/nanosleep, power,
the LLM narrative-analysis design, the full critical-path-instrumentation candidate rationale,
repeatability policy/confidence metadata, and comparison matrix mode) have moved to
`doc/INVESTIGATION_ARCHIVE.md`.

### Zen5/IBS deep-dive
What appears confirmed from current Linux perf/PMU behavior for AMD Family 1Ah (Zen5):
1. Zen5-specific IBS load-latency filtering enables L3-miss-only filtering via a Zen5 feature check.
2. Generic `PERF_COUNT_HW_*` mapping on Family 1Ah still follows the Zen4 event-map path in current
   kernel PMU logic — there isn't yet a distinct "Zen5-only" generic hardware-event map.
3. IBS capability extensions (L3-miss-only, load-latency/fetch-latency filters, richer memory-source
   decoding) are the strongest near-term source of additional signal.
4. L3-miss-only filtering is documented to skew sampling-period behavior — runs using it need
   explicit annotation (shipped — see `topdown.c`'s `print_ibs()`).
5. Zen5's topdown dispatch baseline shifted from Zen4's 6 slots/cycle to 8 — already implemented
   (`topdown.c`'s `CORE_AMD_ZEN`/`CORE_AMD_ZEN5` slot-multiplier branch). But the finer per-scheduler
   breakdown events AMD introduced alongside that width change aren't in `amd_raw_events[]` yet:
   split ALU/AGU scheduler-stall counters, and op-cache/execution-queue events that would separate
   `Frontend Latency` from `Frontend Bandwidth`. `IBS_LD_L1_DTLB_REFILL_LAT` also isn't named
   anywhere in the IBS capability-probing rows. Both are candidate inputs for a future "platform
   formula registry" (see the Topdown deep-dive's item 8) once Zen5-specific formulas are actually
   versioned there — no standalone backlog item yet.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Informs 4.2's "Zen-family preset packs," "PMU-capability-aware comparability warnings," and "AMD IBS
sampling-mode support" (icache/TLB/dcache/L2/L3/branch rate estimates decoded from real per-sample tag
data, not just counting-mode sample counts); and 4.3's "IBS-derived memory-path bottleneck
decomposition," which depends on IBS sampling-mode support existing first.

### Topdown deep-dive
Advancements worth adopting, in priority order for `wspy` specifically:
1. ~~Multiplex-aware confidence~~ — shipped (see "What shipped in 4.0").
2. ~~Decomposition consistency/sanity checks~~ — shipped alongside #1.
3. Hierarchical (L1→L2→L3) parent/child schema with explicit raw-vs-contention-adjusted
   denominators — needed before drill-down reporting means anything.
4. SMT/contention-aware normalization — publish both denominators, document which one drives
   classification.
5. Phase-aware topdown (warmup/steady/degraded) — depends on interval phase segmentation (shipped,
   `phase.c`).
6. Hybrid/heterogeneous core-class summaries — don't mix Atom+Core topdown into one headline number.
7. Cross-signal attribution (topdown + cache/TLB/IBS) — composite bottleneck rules over single-
   counter heuristics.
8. Platform formula registry — versioned event/formula mapping per CPU family/model, for
   auditability.

**MVP acceptance criteria** (still the right bar):
- ≥95% of topdown fields in standard profiles include confidence metadata. **Met** for the level-1
  breakdown (shipped).
- Reports clearly mark low-confidence topdown rows and avoid strong claims from them. **Met.**
- One benchmark run demonstrates phase-specific topdown shifts in generated summary output. **Not
  met** — phase detection exists (`phase.c`) but nothing correlates it with topdown output yet
  (items 5/6 above, and 4.3's "Phase-aware topdown" entry).

→ Items 3-8 map to 4.2's "Hierarchical topdown schema" and "Core-class-aware topdown," and 4.3's
"Phase-aware topdown" and "Composite attribution."

### Preset / Configuration / Option hierarchy deep-dive
A three-level vocabulary for describing what wspy can be asked to do, surfaced while iterating the
4.1 web-interface mockup (2026-07-11) — the goal is for the CLI, `wspy-run`, and the web UI to
describe the same thing the same way, rather than each inventing its own mental model.

- **Configuration** — one thing wspy can be asked to measure, typically corresponding to a single
  wspy run/operation (though a run can combine more than one). Examples: a process tree, an interval
  measurement of performance counters, an interval measurement of other system metrics, an overall
  (non-interval) system measurement, an overall performance-counters measurement. Each configuration
  has a natural output representation — a table, a gnuplot, a tree diagram — which is part of what
  makes it a distinct configuration rather than a variant of another one.
- **Option** — a way to customize a configuration. Options apply to specific configurations, not
  universally: an interval-seconds option only makes sense on an interval configuration;
  `--tree-cmdline` only makes sense on a process-tree configuration.
- **Preset** — a configuration, or combination of configurations, common enough to deserve a name.
  Presets are exactly the things that showed up hand-rolled in `workload/*/run_test.sh` before 4.0 and
  are now `wspy-run --profile` entries. Presets can be hierarchical — a preset can itself specify
  particular configurations at particular option values (`deep-cpu` selects several configurations at
  fixed options). Not every reachable configuration/option combination has, or needs, a preset name;
  presets are the well-worn paths, not the full space.

The load-bearing rule this implies: **a preset names a configuration+option combination `wspy-run`
already knows how to run; the moment a preset's options are customized away from what it names, the
result has left the set of things `wspy-run --profile` can express, and has to run as one or more
direct `wspy` command lines instead.** This isn't a new rule invented for the web UI — it's the same
fatal-combination behavior the real CLI already has (`--passes`, which is what `wspy-run`'s profiles
bin-pack onto, rejects `--interval`/`--per-core`/`--tree`/IBS/GPU flags outright; see `CLAUDE.md`'s
`wspy.c` entry). The mockup's "customize a checkbox → separate command lines with an explanatory note"
fallback (shipped in the web launcher's Run tab) is this rule, discovered bottom-up from the real
constraints before being named top-down here. Worth treating as the general rule going forward rather
than a fact specific to counters/tree/interval, since it will recur every time a new configuration or
option is added.

Realized as shipped 4.1 features (see "What shipped in 4.1" above): the web launcher's preset-first
framing with a live "customized away from preset" indicator, and structured configuration provenance
recording which preset/configuration/option choice actually produced a run so a report can say "this
was `deep-cpu`, with the TLB group swapped for L3" rather than re-deriving it from a flat argv.

Cross-cutting goal, not yet committed to: the same preset/configuration/option vocabulary should
eventually describe `wspy`'s own CLI options (today an unstructured flat flag list) and `wspy-run`'s
profile format (today hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in `load_builtin_profile()`), not
just the web UI. Nothing here commits to that refactor — see "Open questions for prioritization" below
— but this is the vocabulary to design against as any later CLI/`wspy-run` restructuring proceeds, so
it doesn't independently invent a different model for the same thing. There is real leeway to adjust
existing options/commands toward this if it produces a cleaner architecture.

### Critical-path / synchronization-latency: what's left
All six originally-scoped syscall-latency candidates (futex, blocking I/O, connect, nanosleep, wait,
poll) have shipped — see "Shipped since 4.1" above and `doc/INVESTIGATION_ARCHIVE.md` for the full
motivation and per-syscall design rationale. What remains open from this track:
- The *general*, table-driven mechanism (`tree_open`'s "syscall name → number → decode →
  log-vs-aggregate" generalization, 4.3's "General syscall-level critical-path instrumentation" entry)
  was deliberately not built — six syscall families were still cheap enough as individual `if`
  branches in `ptrace_loop()`'s dispatch. Revisit only if a seventh syscall family comes up.
- `ptrace` itself imposes a real stop-the-world cost on every syscall of the traced process, so
  absolute latency numbers collected this way are inflated relative to an untraced run. The *relative*
  split (fraction of wall time in futex-wait vs. read-wait vs. on-CPU) stays informative even when
  absolute numbers are skewed, but this is an inherent limitation of the mechanism — 4.3's "Low-overhead
  tracing alternative to ptrace" entry is the eventual fix, not a documentation note.

## 4.2 — remaining work
Everything from 4.2's original scope that hasn't shipped yet (see "Shipped since 4.1" above for what
has). Ordered in dependency tiers; items within a tier are independently startable.

**Tier 3 — topdown/IBS refinement (interdependent; sets up 4.3's attribution work):**

3. Hierarchical (parent→child) topdown schema + explicit raw-vs-contention-adjusted denominators +
    formula/version metadata.
4. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — depends on per-core
    collection (shipped) plus the hierarchical schema above.
5. Zen-family preset packs (`zen-portable`, `zen4plus-deep`) — convenience layer now that IBS
    capability probing exists.
6. PMU-capability-aware comparability warnings.
7. AMD IBS *sampling*-mode support: mmap'ing the perf ring buffer and requesting `PERF_SAMPLE_RAW`
    so each individual IBS sample's tagged register data is available, not just a count of how many
    fired — a genuinely new capability, not an extension of the counting-mode `ibs-basic`/
    `ibs-memory-deep` profiles. Nothing in wspy today reads a perf mmap ring buffer at all; every
    existing counter group (including IBS as currently implemented) is `read()`-based counting. Each
    `IbsOpData`/`IbsOpData2`/`IbsOpData3`/`IbsDcLinAd` (op samples) and `IbsFetchCtl`/`IbsFetchLinAd`
    (fetch samples) record carries tag bits this item would decode into per-tick rate estimates
    comparable to (but independently sourced from) the equivalent hardware-counter groups already
    reported: `DcMiss`/`L2Miss`/`NbIbsReqSrc` → dcache/L2/L3 miss and memory-source-of-fill rates;
    `DcL1TlbMiss`/`DcL2TlbMiss` → data-TLB miss rates; fetch-side `IcMiss`/`L1TlbMiss`/`L2TlbMiss` →
    icache/iTLB miss rates; `BrnMisp` (on branch ops) → branch misprediction rate. Valuable
    specifically because it's a second, independently-sampled measurement of the same phenomena the
    counting-mode groups already report — a real disagreement between the two is itself a signal (PMU
    multiplexing skew, a counting-mode blind spot, or an IBS sampling-rate artifact), not just a fifth
    way to get the same number. Format/sysfs-field discovery already exists (`ibs.c`'s `ibs_probe()`)
    and generalizes directly; the new work is the mmap/ring-buffer read path itself, per-sample record
    parsing, and the rate-aggregation/report layer built on top. Feeds 4.3's "IBS-derived memory-path
    bottleneck decomposition," which assumes this sampling capability already exists.
8. Per-core energy (`power_core`) support: `--power` currently reports package-level `pkg_joules`/
    `pkg_watts` only — `power_core`'s own `cpumask` (one representative logical CPU per physical core)
    means a real per-core breakdown needs opening N events, one pinned per primary-thread CPU, and
    aggregating them into `--per-core`'s existing per-core row shape, a separate unit of work layered
    on top of the shipped package-level design (see `doc/INVESTIGATION_ARCHIVE.md`'s power deep-dive).
    `power_core` is currently probed for `--capabilities` discovery only, never opened as a real
    counter.

**Tier 4 — GPU fusion:**

9. ROCm SMI + sysfs fusion layer (one stream, source precedence, per-metric validity flags) —
    merges the two existing independent GPU paths (`amd_smi.c`, `amd_sysfs.c`).
10. Same manifest/index/profile pipeline extended to GPU runs (busy/clocks/power/temp/memory
    activity) — reuses the 4.0 foundation rather than a parallel GPU-only pipeline. Closes the
    "Minimum metadata set for publishable" open question's GPU caveat (see "Open questions" below).

**Tier 5 — `/proc` and tree enrichment remainder (independent, moderate value, low risk):**

11. cgroup identity + limits in manifest, `cpu.stat` throttling stats — needed for fair comparison in
    containerized environments.
12. Per-core (`--per-core`) → imbalance/hot-core/migration diagnostics, core-class summaries.
13. `proctree` → JSON/Graphviz export + run-to-run tree diff.

**Tier 6 — characterization prerequisites:**

14. Feature normalization prerequisites (fixed feature set from counters/topdown/faults/context-
    switch/I-O) — needs 4.1's normalized store schema (`wspy-store`) to draw features from.
15. Archetype scorecard (parallelism shape, resource dominance, control-flow style, runtime
    stability) + confidence + top-2 alternatives.

**Tier 7 — launcher/infra follow-ups:**

16. Collapse `wspy-run`'s builtin profiles (`deep-cpu` et al.) onto native `--passes` bin-packing.
    They still shell out to `wspy` once per pass today; 4.1's multi-pass execution work scoped this
    collapse as a documented follow-up, not part of that item.
17. Job-browsing view in the web UI. A queued job (`wspy-queue add`, or the Run tab's "Queue instead
    of running it now" checkbox) is visible today only via `wspy-queue list`/`show`, not from the web
    UI itself. Bundle in sharing structured configuration provenance with the job format
    (`web/joblib.py`'s job schema and `manifest.h`'s `configuration_provenance` are designed to be
    close in shape but aren't wired together yet).
18. Give the report compare view (`GET /compare`) its own curation/annotation layer. It's deliberately
    raw/filename-aligned today (comparing actual artifacts across runs, curated or not); annotating a
    comparison itself, or aligning curated block titles across the compared runs, is still open.

**Tier 8 — docs/testing/release process:**

19. Profile cookbook + interpretation playbook (how to read confidence/phase/comparability/cluster
    output).
20. Reproducibility bundle export (tarball: manifest + raw + derived per batch).
21. Size `wspy-run`'s `--tree` pass timeout from an actual run-time estimate instead of a fixed 3600s
    constant (e.g. `phoronix-test-suite` reportedly has a run-time-estimate command) — today's
    constant is a blunt stand-in; the real constraint is capping process-record data volume for
    publishing, not workload runtime, so a per-workload estimate would size it more accurately than
    one constant across every suite.
22. Doc/version consistency check — an automated check (script, or an addition to `run_tests.sh`)
    that catches the class of drift found during the v4.0 release audit: `doc/ARTIFACT_CONTRACT.md`'s
    schema-version examples had silently fallen behind `MANIFEST_SCHEMA_VERSION`/
    `RUN_INDEX_SCHEMA_VERSION`, and `README.md` was missing a whole tool's section. Concretely:
    grep-based checks that doc-quoted schema versions and the documented tool/flag list match the
    actual header constants and `Makefile` binary list, so this doesn't require a manual audit at
    every release again.
23. Release-prep checklist/script — capture the v4.0 release process (bump `WSPY_VERSION_MAJOR`/
    `MINOR`, grep for stale version-string references across docs, run the full test matrix including
    the `AMDGPU=1` variant, tag, label every merged PR since the last tag, draft release notes from
    the merged-PR list) as a repeatable script or documented checklist instead of redoing it by hand,
    since every future phase will need this same sequence again.

**Dropped, not deferred:** "Deeper Phoronix Test Suite awareness in the web UI" — its "read a Phoronix
benchmark article and inventory its benchmarks" sub-item conflicts with Phoronix's site use policy
(scraping/parsing their articles), so that half is off the table outright; the rest of the item's
motivation — tracking which Phoronix tests have been run — is now covered by `wspy-ledger`'s existing
workload-coverage tracking, which lowers the value of building Phoronix-specific web UI on top of the
general launcher enough that the item isn't worth carrying forward.

## 4.3 priorities
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and a lower-overhead tracing backend.

**Tier 1 — needs 4.1's normalized store/history:**

1. Baselines and regression/anomaly detection.
2. Machine/environment comparability scoring — depends on provenance capture (shipped, `provenance.c`)
   existing across enough runs to score against.
3. Distribution-first reporting (quantiles, clustering prep).
4. Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace
   only when data coverage differs).

**Tier 2 — topdown/attribution, needs 4.2's hierarchical schema + phase detection (shipped) + IBS
sampling mode:**

5. Phase-aware topdown (warmup/steady/degraded segmentation, drift signal).
6. Composite attribution (topdown + cache/TLB/IBS signals) — the "no blocking-syscall activity" vs.
   "heavy blocking-syscall activity" split from the critical-path work (shipped, see "Shipped since
   4.1") is a direct input here, alongside topdown/cache/TLB/IBS.
7. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) — needs 4.2's IBS
   sampling-mode support first; today's counting-mode IBS has no per-sample tag data to decompose.

**Tier 3 — publishing/reporting expansion, needs 4.1's report studio:**

8. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1's per-run curation studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats (WordPress/HTML/Markdown,
   4.1) rather than inventing a fourth.
9. Characterization badges + similarity panels in reports — a new block type in 4.1's curation studio
   once 4.2's archetype scorecard exists to draw a badge from, not a separate report surface.
10. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1's
    static inclusion-depth mechanism (none/summary/excerpt/full) for the tree/interval blocks
    specifically; that mechanism stays the right default for a published, non-interactive report even
    once this exists.

**Tier 4 — report-layer additions on data already collected in 4.0:**

11. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
    process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
12. System (`--system`) → per-interface network attribution and local-vs-system-pressure
    attribution, plus steal-time capture (user/system/iowait are already captured and printed —
    `system.c`'s existing `/proc/stat` parsing — this item is the missing steal column and the
    analysis layer on top of what's already there, not the raw mix itself).
13. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
    `comm`-pattern role tagging).

**Tier 5 — GPU deeper profiling:**

14. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
    heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
15. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) —
    depends on 4.2's GPU fusion layer providing consistent per-metric data first.
16. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`,
    extended once GPU runs feed the same index.
17. Fold into general environment-comparability scoring (power cap, memory clock, thermal state,
    driver version) — no separate "GPU comparability score" needed; one scoring mechanism, not two.

**Tier 6 — infra:**

18. Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for
    `--tree`/`--tree-open` — `ptrace` context-switches on every syscall entry/exit, which skews the
    very counters being measured for I/O-heavy or fork-heavy workloads. Also the eventual fix for the
    observer-effect caveat noted under "Critical-path / synchronization-latency: what's left" above.
19. Collector-plugin implementation (perf stat / trace-cmd / GPU tools as collectors behind the
    `collector` field, normalization path) — the schema seam shipped in 4.0; this is the actual
    implementation of wrapping a non-wspy collector.
20. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
    CSVs into per-test-case/per-trial datasets by correlating run manifests with PTS results, composite.xml,
    and log timestamps. See the detailed report at
    [phoronix_hook_investigation.md](file:///home/mev/source/wspy/doc/phoronix_hook_investigation.md) for
    design and prototypes. **Capture instrumentation landed ahead of the full item** (2026-07-19,
    `doc/phoronix_hook_investigation.md`'s "Implementation Update" section): `scripts/pts_hooks/{pre,
    post}_test_run.sh` (PTS `result_notifier` hook scripts, sub-millisecond TSV checkpoints to a fixed
    staging path — verified against the real installed module that a hook subprocess's environment is
    *replaced*, not inherited, which rules out passing the wspy run directory down directly), and
    `scripts/setup_phoronix_hooks.sh` (one-time host registration helper, `setup_perf.sh`-style
    check-then-prompt). **Registered on this host and rewired (2026-07-19,** same doc's "Follow-up"
    section**):** relocation of the staging log into a `<pass-name>.pts_hooks.log` artifact (recorded in
    `manifest.json`'s `passes[]`) now lives in `wspy-run`'s own `run_pass()`, per-pass rather than once at
    the end of a whole invocation — the first cut of this lived only in `workload/phoronix/run_test.sh`,
    which meant Phoronix runs launched via the web launcher's preset path or `wspy-queue` (both go through
    `wspy-run` directly, not that script) silently lost their hook capture; moving it into `wspy-run`
    itself covers every front end that funnels through it. `run_test.sh` no longer does any relocation of
    its own. The web launcher's *custom checklist* run path (`web/joblib.py`'s `execute_custom_run()`,
    plus `execute_profile_run()`'s supplementary plot-data passes) calls `wspy` directly rather than
    through `wspy-run`, so it needed its own Python-side equivalent
    (`_archive_stale_pts_hooks_log()`/`_capture_pts_hooks_log()`, `web/joblib.py`) — now wired in too, same
    day, same per-pass artifact shape (`"pts_hooks_log"` in `manifest.json`). Every real launch path in
    the codebase now captures hook data the same way. **Real-host testing (2026-07-19, same doc's
    "Real-Host Findings" section) found registration had never actually worked at all**: two compounding
    bugs, one ours (`setup_phoronix_hooks.sh` wrote a hyphenated `modules-data/result-notifier/` directory;
    PTS's own module lookup resolves the underscored `result_notifier`, matching the module's literal PHP
    class name, so registration silently no-opped — fixed), one upstream (PTS's bundled
    `result_notifier.php` unconditionally dereferences a null `test_result_buffer` and calls a
    nonexistent `pts_test_result::get_result()`, fatally crashing `phoronix-test-suite` itself as soon as
    *any* real hook script is configured — filed and fixed upstream at
    [phoronix-test-suite/phoronix-test-suite#924](https://github.com/phoronix-test-suite/phoronix-test-suite/pull/924)
    / [#925](https://github.com/phoronix-test-suite/phoronix-test-suite/issues/925), verified live). Until
    that upstream fix ships in a release, registering the hooks on an unpatched PTS install turns "no
    telemetry" into "the benchmark run crashes with zero results" — a locally-patched
    `result_notifier.php` (applied and verified on this project's dev host) is the stopgap. **Still open**:
    teaching `wspy-phoronix-segment.py` to prefer `pts_hooks.log` over the composite.xml/log-timestamp
    correlation it uses today.

**Tier 7 — testing:**

21. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
22. Contributor guide for adding a collector/metric/schema bump safely.

## 4.4 priorities
Goal: optional/heavier pieces that shouldn't block the rest, in priority order:
1. Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions,
   resumable/selective re-execution) — full version of the lightweight config-file execution
   already in `wspy-run` (4.0); don't build both at once.
2. Optional deep trace analysis (Perfetto-compatible export of tree+topdown+interval timelines) —
   advanced companion path for difficult workloads, needs 4.3's lower-overhead tracing backend to
   feed it.
3. Temporal drift detection (cluster movement across versions/configs/machines) — needs 4.3's
   clustering plus enough history to detect movement; treat as an investigation trigger, not a
   standalone feature.
4. Optional dashboard backend (e.g. Grafana) for exploratory slicing — explicitly optional/coexists
   with static-first publishing; doesn't block 4.0-4.3.
5. Optional live TUI (run progress, interval metrics, throttling/skew warnings) — a terminal-side
   surface, unrelated to and not superseded by 4.1's web interface work; nice-to-have, CLI-first model
   stays primary.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision. (Several
earlier open questions here — native multi-pass execution, ARM64 support, publication automation,
core/thread affinity — have been resolved by shipped work; see "What shipped in 4.1" and "Shipped
since 4.1" above rather than a stale "resolved" note here.)

- **Should `wspy-run`'s builtin profiles be refactored to be declaratively defined (as
  configurations+options) instead of today's hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays in
  `load_builtin_profile()`?** Opened by the preset/configuration/option deep-dive above.
  Recommendation: not yet — let the web UI's preset/configuration/option model (shipped in 4.1)
  stabilize against real feedback first, then decide whether `wspy-run` itself should be rebuilt on the
  same vocabulary. Premature to commit to a CLI/`wspy-run` restructure before the vocabulary has been
  used for anything real; there's real leeway to make this change later if it produces a cleaner
  architecture, but no reason to rush it ahead of the UI work that motivated it.
- **Is cross-machine comparability a hard requirement for the first round?** Still open.
  Recommendation: no. Provenance fields are captured (4.0); defer comparability *scoring* to 4.3 —
  scoring needs enough historical runs across machines to be meaningful, which doesn't exist yet.
- **Should the website stay static-only, or add an interactive backend?** Still open. Recommendation:
  static-first through 4.3, keep an optional Grafana-style backend as a 4.4 nice-to-have. Non-goal:
  don't let the interactive-backend question block 4.3's static-site work.
- **Minimum metadata set for a run to be "publishable":** every field the original recommendation
  named is captured (timestamps, command line, host/CPU/kernel, provenance, schema version, output
  file list, `wspy-validate` pass/fail) — one open caveat remains: GPU data (busy/clocks/power/temp/
  memory activity) is CSV-only today, `struct manifest_info` (`manifest.h`) has no GPU fields at all,
  so it's absent from the manifest/run-index host block; see 4.2's GPU-fusion tier. "Benchmark
  name/suite" is intentionally out of `wspy`'s own scope — it's `wspy-run`/`workload/*`'s job, not the
  manifest's.

## External brainstorming references
- ReBench — reproducible experiment configuration, resumable execution, explicit benchmark
  parameter tracking: https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) — static-site publication for benchmark trends with an interactive
  frontend model: https://asv.readthedocs.io/en/stable/
- Grafana OSS — optional dashboard-based slicing/templating if the interactive-backend path is
  taken: https://grafana.com/oss/grafana/
- Perfetto — timeline/trace analysis and SQL-based trace queries, relevant to the optional deep
  trace analysis pipeline (4.4): https://perfetto.dev/docs/

Note: OpenBenchmarking.org returned HTTP 403 in the environment used for this research; not
reviewed.
