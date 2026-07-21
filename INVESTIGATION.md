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

**Hierarchical topdown schema (full L1→L2→L3):** `print_topdown()`'s already-computed per-vendor L2
breakdown (ucode/fastpath, frontend latency/bandwidth, backend cpu/memory, speculation branch/pipeline)
now reaches CSV, not just human text, as 9 new trailing columns (`contention_pct` +
`<parent>_<child>_pct`), all on the same contention-adjusted `slots_no_contention` denominator L1
already uses (a real AMD-only consistency fix — the human-text L2 lines previously divided by raw
`slots`). `TOPDOWN_FORMULA_VERSION` (`wspy.h`) is recorded in the manifest/run-index
(`topdown_formula_version`, `MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.6.0 → 1.7.0), `null`
when a run collects no topdown counters. `wspy-plot`'s `topdown-detail` template (`plot.c`) charts the 9
new columns on their own, since they aren't claimed by the pre-existing `topdown` template (which only
matches the 4 L1 columns) and would otherwise all land in the generic fallback plot. **L3 tie-in**
(fast-follow to the above): `--topdown-backend`'s own `l1_bound`/`l2_bound`/`l3_bound`/`dram_bound`/
`store_bound` detail (`print_topdown_be()`, a genuinely separate perf counter group with its own
independent `cpu-cycles` reading) now also reaches the same `slots_no_contention` denominator, via 5 new
`*_slots_pct` trailing columns and a small cross-group sharing mechanism (`topdown.c`'s
`shared_slots_no_contention`, published by `print_topdown()` and read by `print_topdown_be()` — safe only
because `setup_counter_groups()`'s check order guarantees the former always runs first for the same row;
see `CLAUDE.md`'s `topdown.c` entry for the exact mechanism). The original 5 cpu-cycles-normalized
columns are untouched for backward compatibility; the new columns are explicitly documented as *not*
guaranteed to sum to `backend_memory_pct` (independent measurement chains, same caveat as the existing L1
sanity check). `wspy-plot`'s new `memory-bound-detail` template covers the 5 new columns, mirroring
`topdown-detail`. Also fixed, found via inspection while touching this code: two previously-unguarded
unsigned-subtraction underflow risks in `print_topdown_be()`'s own `l2_bound`/`l3_bound` computation
(same bug class as the AMD L2-split fix below), via the same `safe_sub()` helper. Intel/ARM only — AMD
has no `COUNTER_TOPDOWN_BE` raw events at all, so `print_topdown_be()` is never called there, unchanged
from before this item.

**Zen-family preset packs:** `wspy-run`'s `zen-portable` (`quick`+`ibs-basic`) and `zen4plus-deep`
(`deep-cpu`+`ibs-memory-deep`) builtin profiles, the first defined purely as a composition of other
builtin profiles (`load_profiles()`, the same machinery that resolves a user-supplied comma list) rather
than hand-written flag strings. `zen-portable` avoids `--power` (AMD Family 19h+ only) and IBS
`l3missonly` filtering (Zen5-only) so it runs warning-free across the whole Zen family; `zen4plus-deep`
assumes Family 19h+ hardware where both are real, with `l3missonly` degrading gracefully (not failing)
on Zen4. Verified end-to-end on real Zen5 hardware.

**PMU-capability-aware comparability warnings:** `wspy-summary`'s repeatability verdict gains a fourth,
independently-combinable reason, `mixed-pmu` — a bucket's contributing runs are compared by
`(cpu_vendor,counters_requested,counters_measured)` signature (`summary.c`'s `struct bucket` `pmu_*`
fields, sourced from three columns the query already reads off `store.c`'s `runs` table, no new schema),
and any deviation from the first-seen signature flags the bucket. Deliberately exact-match rather than a
numeric threshold like `noisy`'s `--max-cv`: different `cpu_vendor` means a same-named CSV column was
likely computed from genuinely different raw hardware events; different `counters_requested` means the
contributing runs weren't even asking for the same counters (e.g. `--topdown` vs `--topdown2`); different
`counters_measured` (with `counters_requested` equal) means one run's counter setup degraded while
another's didn't — the cross-run aggregation blind spot `wspy-validate`'s own per-manifest coverage
warning can't see. Verified end-to-end on real hardware: two real runs of the same workload (one
`--topdown`, one `--topdown2`) correctly produced `WARN:thin,noisy,mixed-pmu` via `wspy-store`/
`wspy-summary`.

**Per-core energy (`power_core`) support:** `--power --per-core` now opens a real `power_core`/
`energy-core` event per representative CPU (`power_core`'s own sysfs `cpumask` — one representative
logical CPU per physical core, e.g. the 16 even-numbered CPUs out of 32 on a real Zen5/SMT2 host) and
adds new `core_joules`/`core_watts` trailing columns to `--per-core`'s row shape, alongside (not
replacing) the existing systemwide `pkg_joules`/`pkg_watts`. Every per-core-eligible CPU gets a
structurally identical group (same column set every row needs); a CPU that isn't one of `power_core`'s
representative CPUs gets a placeholder counter marked with a new sentinel
(`POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE`, `power.h`) that `setup_counters()` skips before even attempting
`perf_event_open()` — genuinely never-attempted, not "requested but failed", so it doesn't skew
`counters_requested`/`counters_measured` or `preflight.c`'s budget estimate. `--power` alone (no
`--per-core`) is completely unaffected. Confirmed on real Zen5 hardware (root): representative CPUs
showed real nonzero values correlating with actual scheduling activity, sibling CPUs read exactly
`0.000`, `pkg_joules`/`pkg_watts` stayed unchanged across every row, and coverage counts confirmed
exactly 16 representative attempts (not 32). Also confirmed, a genuine finding: summed per-core energy
across representative CPUs was roughly 16× smaller than package energy for the same window —
core-domain energy is a real, meaningfully smaller subset of package energy (excludes uncore/IO/memory-
controller/L3/idle-package power), not a units bug; see `CLAUDE.md`'s `power.c` entry.

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

**ROCm SMI + sysfs GPU fusion layer:** `--gpu-metrics` now merges `amd_sysfs.c` and `amd_smi.c` into one
fused column set instead of requiring a separate `--gpu-smi` for VRAM — sysfs supplies temp/activity/
power/freq (the actively-used path; `amd_smi.c` is documented "legacy"), SMI fills in temp/activity only
when sysfs's reading failed, and SMI remains the sole VRAM source. New `gpu_temp_source`/
`gpu_activity_source` columns record which backend actually supplied each value — the "per-metric
validity flags" this item's original scope called for; power/freq/VRAM each have exactly one possible
source, so they keep this codebase's usual zero-means-unmeasured convention instead of a redundant flag.
The precedence logic itself (`gpu_fusion.c`'s `gpu_fusion_combine()`) is a pure, unit-tested function
(`test_gpu_fusion.c`) separated from the hardware-dependent glue that reads the real backends, mirroring
`power.c`/`ibs.c`'s own testability split. Also collapsed 4 previously hand-duplicated GPU-metrics print
sites (CSV header, per-core CSV, aggregate CSV, human output, across `wspy.c` and `topdown.c`) into one
shared `print_gpu_metrics()`, closing off the exact column-ordering bug class the `--gpu-smi --interval`
fix above already ran into once. `--gpu-smi`'s own raw/legacy columns are unchanged. Verified live on
real AMD GPU hardware: SMI's `gpu_metrics_info` call failed independently of sysfs, and the fused row
still correctly reported `sysfs`/`sysfs` sources plus a real VRAM reading from SMI's separate (successful)
VRAM call. Extending the manifest/index/profile pipeline to GPU runs (the fusion tier's other item)
remains open — see "4.2 — remaining work" below.

**GPU telemetry provenance in the manifest/run-index:** `struct manifest_gpu_info` (`manifest.h`) adds
an `options.gpu` object to both documents (`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.7.0 →
1.8.0) — which `--gpu-*` flag(s) were requested, the resolved AMD/NVIDIA device index, and whether each
backend (`amd_sysfs`/`amd_smi`/`nvidia`) actually produced valid data on this run's last read.
Deliberately provenance-only, not a duplicate of the measured busy/temp/activity/power/freq/VRAM values
themselves (those stay CSV-only, same as every other metric this codebase collects — no manifest field
duplicates measured data, not even `--power`'s `pkg_joules`/`pkg_watts`), the same role
`counters_requested`/`counters_measured` already play for perf counters. Device-index fields are gated
on the `requested` flags rather than the index's sign, so a zero-initialized struct (no GPU flag given
at all) reports `null` rather than looking like "device 0". Closes the "Minimum metadata set for
publishable" open question's GPU caveat (see "Open questions" below) — on provenance terms, not by
capturing the measured values. Verified live on real AMD GPU hardware: the same `amd_smi_metrics`-
fails/`amd_sysfs_metrics`+`amd_smi_memory`-succeed combination the fusion layer above confirmed live
round-trips correctly into both JSON documents. This closes out 4.2's GPU fusion work entirely (both
the fusion layer above and this item).

**System-wide disk I/O stats:** a new `SYSTEM_DISK` bit (`system.c`) reports per-block-device read/
write bytes and time-in-I/O deltas, scraped from `/sys/block/<dev>/stat` (devices enumerated via
`/proc/partitions`, filtered to whole disks) — the same per-device breakdown `SYSTEM_NETWORK` already
gives for `/proc/net/dev`, as three new `disk <dev> read,disk <dev> write,disk <dev> time,` CSV/header
columns per device rather than `archive/wspy2.0/diskstats.c`'s old separate `disk-<dev>.csv`-per-device
approach. Default-on in `system_mask` like `SYSTEM_NETWORK`/`SYSTEM_FREQ`/`SYSTEM_TEMP` (only printed
when `--system`/`-s` is given, no separate CLI flag). `wspy-plot` gained two matching fallback plots
(`disk-io` for the byte counters, `disk-time` for the time-in-I/O column — kept separate since bytes and
milliseconds don't share a useful scale, `plot.c`'s `add_disk_fallback_match()`), and the web launcher's
custom-plot column autofit (`web/joblib.py`) recognizes `disk <dev> ...` columns via the same `"system"`
sentinel `net <iface>` already resolves to. Verified live: real `dd`-driven writes to the root
filesystem showed `disk nvme0n1 write`/`disk nvme0n1 time` tracking actual bytes written and I/O time
tick-for-tick, while a tmpfs-backed write correctly showed zero disk activity. Device enumeration
excludes `loop`/`ram`/`zram` names unconditionally (`is_virtual_disk_device()`) — found via the same
live testing: a real dev host's 35 snap-package loop devices pushed a realistic `--system --power
--counters=topdown --interval` CSV to 137 columns, past `plot.c`'s `MAX_CSV_FIELDS` (128) cap, which
silently truncated header parsing and dropped the `topdown-detail` plot with no error; filtering
brought the same CSV to 35 columns and restored correct plotting. Loop devices' own `/sys/block/loopN/
stat` also never reflects real backing-file I/O (always `read=0/write=0/time=0`), so this is the
correct default independent of the column-budget concern.

**System-wide memory pressure stats:** a new `SYSTEM_MEM` bit (`system.c`) reports 6 fixed `/proc/
meminfo` fields — `MemFree`/`Cached`/`Dirty`/`Writeback`/`SwapFree`/`Committed_AS` — as
`mem_free_mb,mem_cached_mb,mem_dirty_mb,mem_writeback_mb,swap_free_mb,committed_as_mb,` CSV/header
columns (kB converted to MB at print time, matching `freq_mhz`/`cpu_temp_c`'s own convention). Distinct
from `--tree-vmsize`'s per-process RSS/swap snapshot — this is host-wide, useful for spotting a
workload driving the whole machine into reclaim/swap rather than just its own footprint. Unlike net/
disk, these are absolute point-in-time gauges, not deltas, so there's no `last_*`/`prev_*` tracking.
`archive/wspy2.0/memstats.c` built this once against a fixed 18-label table before the 2.0→3.0 rewrite
dropped it; this keeps the same `/proc/meminfo` source but narrows to the 6 fields this item calls out
rather than the old 18-field table or its own separate `meminfo.csv` file. Default-on in `system_mask`,
only printed with `--system`/`-s`. `wspy-plot` gained a real `memory-pressure` template (not a fallback
bucket like `network-io`/`disk-io`, since these 6 columns are fixed names sharing one MB scale), and the
web launcher's custom-plot column autofit lists the 6 names directly in `SYSTEM_COLUMN_NAMES` rather
than prefix-matching, since there's no per-device/per-interface variation. Verified live: a Python
process touching a 300MB buffer moved `mem_free_mb` measurably across `--interval` ticks on a 62GB host.

**`proctree` JSON export + interactive viewer + run-to-run diff:** `proctree --json <tree-file>` emits
one JSON document (per-`comm` summary + full process tree, every field unconditional rather than gated
by `-M`/`-N`/`-P`/`-U`/`-X`/etc.'s text-mode toggles) instead of the text tree/summary — the interchange
format both the new web viewer and `--diff` mode consume, versioned via `PROCTREE_JSON_SCHEMA_VERSION`
(see `doc/ARTIFACT_CONTRACT.md`'s "Tree JSON export"). `proctree --diff [--json] <a.json> <b.json>`
matches subtrees structurally (ancestor-`comm`-path, disambiguated by sibling occurrence order, since
pids never correspond across two separate runs), reporting `added`/`removed`/`changed`/`same` per node
plus a `comm`-keyed `summary_diff` overview; exits 1 if any difference was found, 0 if the trees matched
exactly. `web/server.py` gained an on-demand `GET /api/tree-json/<suite>/<benchmark>/<run_id>` (shells
out to `proctree --json`, no artifact written to disk) feeding a new client-side-rendered
`/tree-viewer/<suite>/<benchmark>/<run_id>` page (`web/static/proctree_viewer.js`: collapsible tree,
search/filter by `comm`/pid, auto-detected column toggles for whichever `--tree-*` annotations this run
actually collected), linked from every report that has a `process.tree.txt`. `GET /tree-diff?r=...&r=...`
reuses the homepage's/`/history`'s existing run-selection checkboxes (a second "Tree diff selected"
button alongside "Compare selected") to drive the same viewer against `GET /api/tree-diff-json`'s merged
diff tree, rendering per-node added/removed/changed/same badges. Drops this item from 4.2 Tier 1's
remaining-work list below (Graphviz export for an already-filtered small subtree remains a possible
optional secondary output, not implemented — the interactive viewer is the main way to view a whole
run's tree).

**`wspy-core-report`: per-core imbalance/hot-core/core-class diagnostics:** a new standalone binary
(`core_report.c`) that reports cross-core min/max/mean/stddev/coefficient-of-variation for every metric
column in an existing `--per-core --csv` file, naming the "hot" (max) and "cold" (min) core by index —
a post-hoc report over an already-collected artifact (matching `wspy-validate`/`wspy-plot`'s own
pattern), not a live collection-time feature. When this host's cores aren't all the same type
(`cpu_info.c`'s per-core `vendor` field — ARM big.LITTLE, Intel Atom+Core hybrid, AMD Zen5/Zen5c, all
now differentiated per-core, see "AMD Zen5/Zen5c core detection" below), an additional breakdown groups
the same stats by core class instead of lumping every core together. Must be run on the same host that
collected the CSV (or one with identical topology) — core classes are re-detected fresh via
`inventory_cpu()`, there's no per-core class column in the CSV itself.
The class-grouping logic (`gather_core_values()`/`distinct_classes_present()`) takes a plain
class-per-core-index array rather than reading `cpu_info` directly, so it's exercised in
`test_core_report.c` against a synthetic heterogeneous-host assignment without needing real or fake
hardware topology. `--csv` output mirrors the human report's fields
(`metric,scope,scope_value,n,min,min_core,max,max_core,mean,stddev,cv_percent`); `--metric <name>`
filters to specific columns. Process/thread migration diagnostics (did a process's threads move between
cores during the run) was split out of this item into its own 4.4 backlog entry during design — it
needs new instrumentation nothing today provides, not just new analysis of data already collected.

**cgroup identity + limits + `cpu.stat` throttling in the manifest/run-index:** a new module
(`cgroup.c`/`cgroup.h`) adds a top-level `"cgroup"` object to both documents
(`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.8.0 → 1.9.0) — cgroup v2 identity (the unified-
hierarchy path from `/proc/self/cgroup`'s `"0::"` line), resource limits (`cpu.max`'s quota/period,
`cpu.weight`, `memory.max`/`memory.high`), and CPU-throttling stats (`cpu.stat`'s `nr_periods`/
`nr_throttled`/`throttled_usec`) — needed for fair comparison of runs in containerized environments,
where a cpu.max quota or an ongoing throttling episode can explain a degraded result that has nothing
to do with the workload itself. cgroup v2 (unified hierarchy) only; a pure cgroup v1 host degrades the
whole thing to unavailable, and every limit field degrades independently (a real, confirmed-live case:
a desktop terminal-emulator's leaf cgroup had `memory.max`/`memory.high` but no `cpu.max`/`cpu.weight`/
throttling fields at all, since the cpu controller wasn't enabled on it). Identity/limits are read once;
`cpu.stat`'s cumulative counters are read twice — once near workload launch (right before the
`--passes`/single-pass fork, so it applies uniformly to both paths), once at manifest-write time — and
the *delta* is what's reported (throttling during this run specifically, not since the cgroup's own
creation), mirroring `read_counters()`'s before/after idiom for perf counters rather than
`provenance.c`'s one-shot facts. `cgroup_state`/`cgroup_throttle_baseline` are module-owned run-lifetime
state (`cgroup.h`, mirroring `affinity.h`'s `requested_affinity`/`affinity_active` precedent) rather
than `wspy.c`-local globals. `manifest_cgroup_info` (`manifest.h`) is a deliberately leaner,
manifest-facing projection of `cgroup.c`'s own structs, matching `manifest_gpu_info`'s own precedent of
not reusing the collecting module's internal struct directly. Tested against fake `/proc/self/cgroup`+
`/sys/fs/cgroup` fixtures (`test_cgroup.c`, mirroring `ibs.c`/`power.c`/`affinity.c`'s own testable-`_at()`
convention), including a regression fixture for the real no-cpu-controller case found during
development.

**AMD Zen5/Zen5c core detection + `wspy-core-report` web UI hook:** `cpu_info.c` previously classified
every family-0x1a AMD core as `CORE_AMD_ZEN5` uniformly, unable to tell full Zen5 cores apart from the
physically compact Zen5c cores on hybrid parts (e.g. Ryzen AI 300 "Strix Point") — cpuid family/model
alone can't distinguish them. `resolve_amd_zen5_dense_cores()` closes this by clustering on per-core
`cpufreq` max instead (any family-0x1a core whose max frequency reads below the highest seen among its
siblings is reclassified `CORE_AMD_ZEN5C`), mirroring the heuristic `scripts/map_cpu_hierarchy.py`
already used to label the same host's cores; degrades to leaving every core `CORE_AMD_ZEN5` when
frequency data isn't readable, the usual measured-vs-unavailable idiom. Two consumers needed fixing to
avoid silently mishandling the new class: `topdown.c`'s slots-per-cycle formula (Zen5c shares Zen5's
8-wide core design, so it's folded into the same branch rather than falling through to an
uninitialized `slots`) and `wspy.c`'s `core_is_per_core_eligible()` (without it, Zen5c cores would
silently collect zero per-core counters). Verified live: `./cpu_info`'s Zen5/Zen5c split matches
`map_cpu_hierarchy.py` exactly, CPU-for-CPU, on a Ryzen AI 9 HX 370 (4 Zen5 + 8 Zen5c cores). This also
means `wspy-core-report`'s existing per-core-class breakdown (above) now actually fires a real
Zen5-vs-Zen5c split, with no changes needed there beyond adding the class's display name.

`wspy-core-report` itself had no web launcher hook until now — a new "Per-core class comparison"
section on the Validate tab (`web/server.py`) runs it against a discovered or pasted `--per-core` CSV
(`discover_percore_csv_paths()`, gated on a real `core` header column so a chip never points at a file
that would just fail), with `--metric`/`--csv` options exposed. Report pages (both the fixed-config and
`wspy-run`/checklist shapes) also gained a "Compare cores" link next to any `--per-core` CSV artifact
(`core_report_link()`) that lands pre-filled on that Validate tab section via a new `core_report_csv`
query param and `render_index()`'s `active_tab` — closing the loop from a `--per-core` run straight
through to a core-class comparison without dropping to the CLI.

**Feature normalization prerequisites:** `wspy-store` now derives a fixed, coverage-aware feature
vocabulary from `metric_values`/`runs` into a new `run_features` table (`store.c`'s
`extract_run_features()`, `STORE_SCHEMA_VERSION` 3→4) — v1 covers topdown L1 (`retire_pct`/
`frontend_pct`/`backend_pct`/`speculate_pct`), cache/TLB/branch miss rates, `fault_rate`/
`ctxswitch_rate` (rusage-derived), `phase_stability` (needs `--interval`), and `parallelism_proxy`
(cross-core IPC coefficient of variation, needs `--per-core`) — each independently `measured`/
`unavailable` rather than a silent zero when its source columns weren't collected. Runs automatically
after metrics ingestion (`--no-feature-extract` opts out). Per-process I/O-rate features are
deliberately deferred: `--tree-io`'s `rchar`/`wchar` live in the *tree* output file, which nothing
ingests into the store today. See `doc/INVESTIGATION_ARCHIVE.md`'s "Concrete design: feature
normalization prerequisites" for the full derivation-rule rationale. This was one of two
characterization-track items originally scoped for 4.2 Tier 1 — both have since shipped, see the
"Archetype scorecard" entry below. `FEATURE_SET_VERSION` `1.0` → `1.1` (added `smt_contention_pct` and
`active_core_count`) alongside that item, grounded in real prior workload-clustering work — see its
own entry for detail.

**Archetype scorecard:** `wspy-archetype` (`archetype.c`) classifies a run along four axes scored from
`run_features` — `resource_dominance` (the headline axis: `compute-bound`/`frontend-bound`/
`memory-bound`/`speculation-bound`, ranked from topdown L1 percentages, with a top-2 alternative and
a margin-based confidence level) plus three simpler supporting tags (`parallelism_shape`,
`control_flow_style`, `runtime_stability`, each `unknown` when their source feature wasn't collected).
No taxonomy/threshold/confidence-formula spec existed anywhere in this repo before this item — every
rule is a from-scratch v1 design, confirmed with the user as 4 independent axes (not a single
composite cross-product label) specifically because `resource_dominance` is the one axis with a
natural ranked percentage to define "top-2 alternatives" against. Real prior art grounded the design:
a 2024 clustering analysis (~240 Phoronix tests + 23 SPEC CPU2017 benchmarks, k-means into 30
clusters, see `mvermeulen.org/perf/2024/06/08/clustering/`) used exactly
`retire`/`frontend`/`backend`/`speculation` as its core clustering metrics, directly validating the
`resource_dominance` approach, and separately used `on_cpu` (cores actively used) as a clustering
dimension distinct from load balance — motivating the new `active_core_count` run_feature (see above)
that `parallelism_proxy` alone didn't capture. Two CLI modes mirror `summary.c`'s bulk/`--trace`
duality: default scores every run matching `--command`/`--hostname` filters (one row per run, CSV or
human table; deliberately excludes runs with zero `run_features` rows at all, e.g.
`--no-feature-extract`, rather than showing them as all-`unknown`); `--run <hostname>:<run_id>` prints
one detailed `key=value` scorecard. Designed for extensibility: a new simple threshold-based axis
(e.g. a `compute_style` axis from `--float`'s existing `float` CSV column, once that has real
cross-workload validation) is one rule-table addition plus one `classify_simple_axis()` call site, no
changes needed elsewhere. See `CLAUDE.md`'s `archetype.c` entry for the full design.

**Compare-view curation (Phase 1):** `GET /compare` now carries an optional annotation layer —
`compare.json` (`COMPARE_SCHEMA_VERSION`, `web/server.py`), the first cross-*run* state file in this
codebase (`curation.json` is strictly per-run; `run_index.jsonl`/`store.db` are flat per-run logs with
no relationship between specific runs), stored at `<output_root>/compares/<id>.json` where `id` is a
hash of the sorted, deduped run-key set (order-independent, exact-match — a different run set gets a
different id and starts uncurated, no fuzzy reattachment). Scoped to Phase 1 only: one `overview_note`
for the comparison as a whole plus one commentary note per filename row, reusing today's exact
filename-based row identity — no cross-run alignment of differently-named files yet (deferred; see
below). A separate `GET`/`POST /compare/curate?r=...&r=...` edit page mirrors the studio/report split
rather than an inline-edit toggle. Covered by `web/test_compare_curation.py` (id determinism/exact-
match, load/save round-trip, run-resolution dedup/floor, save/clear-a-note paths) — same "not wired
into `make test`" convention as `web/test_joblib.py`/`web/test_trace_links.py`. See `CLAUDE.md`'s
"Compare-view curation" entry for the full design.

**Deferred out of Phase 1, not dropped:** manually aligning two differently-named files from
different runs as "the same measurement" (e.g. two runs that used different profiles/passes and so
named conceptually-equivalent output differently) — the row-identity model above is filename-only, so
this would need a real new alignment concept (a group/label spanning a per-run file mapping) rather
than an extension of the current commentary layer. Worth revisiting once real multi-profile
comparisons actually need it, not before.

**Doc/version consistency check:** `tests/doc_version_check.sh` (wired into `run_tests.sh`, once, not
per GPU-build axis — static text/build-list check, not build-variant-dependent) — and this wasn't a
hypothetical exercise: running it for the first time found the exact class of drift the backlog item
described, live in the repo. `doc/ARTIFACT_CONTRACT.md`'s manifest/run-index JSON examples *and* its
own separate "Current versions as of this writing" prose summary had each independently drifted to a
stale `1.5.0` against the real `1.9.0` `MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` — two
different places quoting the same fact, gone stale independently, exactly the failure mode a single
source of truth would have prevented — and `README.md` had no section at all for `wspy-core-report`/
`wspy-archetype` despite both being built by `make all`. All three fixed in the same change as adding
the script. Grep-based (every check in `tests/` is), not a parser: for each `*_SCHEMA_VERSION`-style
constant the doc actually quotes a version for, every occurrence — scoped to that constant's own
`## ` section via `awk` range-matching, so a same-shaped value elsewhere can't produce a false match
— must equal the real header `#define`; a constant named in the doc but never given a version number
to check (`STORE_SCHEMA_VERSION`, `TOPDOWN_FORMULA_VERSION`, `CURATION_SCHEMA_VERSION`/
`COMPARE_SCHEMA_VERSION`) is a `WARN`, not a `FAIL` — nothing to compare against, and mandating every
constant be quoted isn't this check's job. Separately checks every `Makefile`-built binary is
mentioned somewhere in `README.md`, and the reverse (a section for something that's neither a
Makefile binary nor a real executable in the repo root). See `CLAUDE.md`'s `tests/doc_version_check.sh`
entry for the full design.

**Release-prep checklist/script:** `scripts/release_prep.sh` — captures the v4.0/v4.1/v4.1.1 release
process as a repeatable script: pre-flight checks, a merged-PR/release-label audit, version bump,
stale-version-reference grep, the full test matrix, a release-notes draft, and doc bookkeeping
reminders, ending with print-only tag/push/publish commands it never executes itself (matching how
every push/PR gets handled in this project's own workflow — asked first, never auto-executed). Also
not hypothetical: the label audit found real, live drift the first time it ran — PR #124 was missing
its `v4.2` label (every other merged PR since `v4.1.1` already had it, applied incrementally as each
merged), now fixed. Two real bugs found and fixed while building this, worth recording since they're
non-obvious: (1) `gh pr list --search "merged:>=<date>"` is imprecise at same-day tag/PR boundaries
(`v4.1.1`'s own tagged commit's author-date collided with its own PR's merge time and got
double-counted as "since v4.1.1") — fixed by using `git log <tag>..HEAD --merges` for exact PR
ancestry instead of a GitHub date search; (2) `gh pr edit --add-label` fails outright on this repo
with a "Projects (classic) is being deprecated" GraphQL error before the label is ever applied — fixed
by using `gh api repos/{owner}/{repo}/issues/<n>/labels` instead. See `CLAUDE.md`'s
`scripts/release_prep.sh` entry for the full phase-by-phase design.

**Size `wspy-run`'s `--tree` pass timeout from an actual run-time estimate:** `estimate_tree_pass_
timeouts()` (`wspy-run`) sizes the timeout generically for any pass whose flags use `--tree` — not
hardcoded to `tree-heavy` by name, so `gpu-compute` (which also uses `--tree`, previously with no
timeout at all) now gets one too. Reuses `web/joblib.py`'s already-validated Phoronix runtime-
estimation logic (`estimate_phoronix_workload_seconds()`, moved there from `server.py`'s "Estimated
runtime display" Check button — see `web/joblib.py`'s own entry) via a new small CLI wrapper,
`scripts/estimate_tree_timeout.py`, rather than reimplementing the same `phoronix-test-suite info`
text parsing a second time in bash. Design settled through direct discussion, correcting two
assumptions from the original backlog line: the timeout's real purpose is a ptrace-hang backstop, not
primarily a data-volume cap — losing a key ptrace event for a traced process can leave `wspy` hung
waiting to clean up — and real Phoronix runs legitimately exceed the historical 3600s constant, so
the floor stays at exactly that constant (this can only raise the cap for a workload confirmed to
legitimately need longer, never lower it) with a generous 6-hour ceiling as a true hang backstop, not
a normal-operation limit. `phoronix-test-suite info <test>`'s own per-test estimate is treated as a
floor, not a target, and multiplied up more aggressively for `batch-run` specifically
(`BATCH_RUN_MULTIPLIER=5.0` vs. `RUN_MULTIPLIER=2.0`) — confirmed live against a real installed test
profile (`blender-1.2.1/test-definition.xml`) that a full `batch-run` sweep runs every configured
option combination (5 blend files × 2 compute backends = 10 full renders for that one test), something
`info`'s single-test estimate doesn't account for. Falls back to the exact historical `3600` whenever
no estimate can be derived (`python3` missing, non-Phoronix workload, `phoronix-test-suite` not
installed/reachable) — never blocks a run over a missing/failed estimate. Deliberately scoped to
`--tree` passes only for now (the item's original motivation); see the "Phoronix per-test
option-combination count" item below for a related future connection (a real combination count would
replace this item's own `batch-run` multiplier guess with a grounded number) and `wspy-ledger`'s own
scope for where that's planned to live.

**Profile cookbook + interpretation playbook** (`doc/PROFILE_COOKBOOK.md`, see its own `CLAUDE.md`
entry): a reading guide for `summary.c`'s `verdict` column, `archetype.c`'s `confidence`, `phase.c`'s
`phase` output, and the two real comparability mechanisms (`mixed-pmu`, environment `--group-by`) —
what each signal means and what to do when it fires, not a restatement of the artifact format
(`doc/ARTIFACT_CONTRACT.md` already owns that). Every numeric example is real captured output from a
small synthetic 4-run dataset built specifically to trigger a genuine `WARN:noisy,mixed-pmu` bucket
and a real low-confidence `wspy-archetype` classification, rather than invented figures; the phase-
boundaries example is the one exception, explicitly labeled illustrative since a live phase
transition needs real perf access this development sandbox didn't have. Also directly resolves the
backlog line's ambiguous "cluster" wording: states plainly that statistical clustering is **not**
shipped yet (still its own distinct 4.3 item, "Clustering + nearest-neighbor + cluster profile
cards"), rather than describing a feature that doesn't exist — kept separate from the real, but
unrelated, ARM PMU hardware "cluster" topology output (`cpu_info.c`), which is topology discovery,
not an interpretive signal.

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

→ Informed 4.2's "Zen-family preset packs" and "PMU-capability-aware comparability warnings" (both
shipped). Also informs 4.3's "AMD IBS sampling-mode support" (moved to the front of 4.3, see that
section — icache/TLB/dcache/L2/L3/branch rate estimates decoded from real per-sample tag data, not just
counting-mode sample counts) and that same phase's "IBS-derived memory-path bottleneck decomposition,"
which depends on it existing first.

### Topdown deep-dive
Advancements worth adopting, in priority order for `wspy` specifically:
1. ~~Multiplex-aware confidence~~ — shipped (see "What shipped in 4.0").
2. ~~Decomposition consistency/sanity checks~~ — shipped alongside #1.
3. ~~Hierarchical (L1→L2→L3) parent/child schema with explicit raw-vs-contention-adjusted
   denominators~~ — shipped, including L3 (folding `--topdown-backend`'s own detail in), see
   "Shipped since 4.1"'s "Hierarchical topdown schema".
4. ~~SMT/contention-aware normalization — publish both denominators, document which one drives
   classification~~ — shipped alongside #3 (`contention_pct` CSV column; every other percentage
   documented as a fraction of `slots_no_contention`).
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

→ Items 3-8 map to 4.2's "Hierarchical topdown schema" (shipped) and 4.3's "Phase-aware topdown,"
"Composite attribution," and "Core-class-aware topdown" (moved to 4.3, blocked on hardware access).

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
has). (Both original Tier 1 characterization-track items and the launcher/infra follow-ups tier have
now shipped or moved elsewhere -- see "Shipped since 4.1" above and 4.3's infra tier for the
`wspy-run`/`--passes` collapse -- so this is now a single remaining tier.)

**Docs/testing/release process:**

1. Reproducibility bundle export (tarball: manifest + raw + derived per batch).

**Dropped, not deferred:** "Deeper Phoronix Test Suite awareness in the web UI" — its "read a Phoronix
benchmark article and inventory its benchmarks" sub-item conflicts with Phoronix's site use policy
(scraping/parsing their articles), so that half is off the table outright; the rest of the item's
motivation — tracking which Phoronix tests have been run — is now covered by `wspy-ledger`'s existing
workload-coverage tracking, which lowers the value of building Phoronix-specific web UI on top of the
general launcher enough that the item isn't worth carrying forward.

## 4.3 priorities
Goal: use the normalized store built in 4.1 for regression detection, clustering, phase-aware
topdown/IBS attribution, static-site publishing, and a lower-overhead tracing backend.

**Tier 1 — AMD IBS sampling-mode support (moved to the front of 4.3, 2026-07-20; see below):**

1. AMD IBS *sampling*-mode support: mmap'ing the perf ring buffer and requesting `PERF_SAMPLE_RAW`
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
   parsing, and the rate-aggregation/report layer built on top. Feeds this same phase's own
   "IBS-derived memory-path bottleneck decomposition" item (Tier 3 below). Moved here (was the last
   item in 4.2 Tier 1) rather than left at the tail of 4.2: it's a large, genuinely new capability on
   its own, not a small extension squeezed in after the rest of 4.2 wrapped up, and it has no
   dependency on anything else in 4.2's own remaining work (characterization prerequisites,
   launcher/infra, docs/testing/release) — only on already-shipped
   4.0/4.2 IBS capability-discovery work (`ibs.c`), so it's equally startable as 4.3's own first item.

**Tier 2 — needs 4.1's normalized store/history:**

2. Baselines and regression/anomaly detection.
3. Machine/environment comparability scoring — depends on provenance capture (shipped, `provenance.c`)
   existing across enough runs to score against. Broader than 4.2's (shipped) "PMU-capability-aware
   comparability warnings": that item is a narrow, immediate per-bucket exact-match check on
   `(cpu_vendor,counters_requested,counters_measured)`; this item is the deferred, scored version across
   the fuller provenance surface (BIOS, microcode, governor, memory, virtualization, etc.).
4. Distribution-first reporting (quantiles, clustering prep).
5. Clustering + nearest-neighbor + cluster profile cards, coverage-aware distance (common-subspace
   only when data coverage differs).

**Tier 3 — topdown/attribution, needs 4.2's hierarchical schema + phase detection (both shipped) +
this phase's own IBS sampling mode (Tier 1 above):**

6. Phase-aware topdown (warmup/steady/degraded segmentation, drift signal).
7. Composite attribution (topdown + cache/TLB/IBS signals) — the "no blocking-syscall activity" vs.
   "heavy blocking-syscall activity" split from the critical-path work (shipped, see "Shipped since
   4.1") is a direct input here, alongside topdown/cache/TLB/IBS.
8. IBS-derived memory-path bottleneck decomposition (combine with topdown/cache) — needs this phase's
   own IBS sampling-mode support first (Tier 1 above); today's counting-mode IBS has no per-sample tag
   data to decompose.
9. Core-class-aware topdown (hybrid Intel Atom+Core; weighted aggregate) — **blocked on hardware
   access**: moved out of 4.2 (2026-07-19) since real Intel hybrid (Alder Lake/Raptor Lake) or ARM
   big.LITTLE hardware isn't reachable in the environment doing this work, and every line of
   core-type-specific raw-event/PMU-type code would ship unverified. Investigation before deferring
   turned up more missing infrastructure than the one-line backlog description implied, worth keeping
   so a future pass doesn't have to re-derive it: `wspy.c`'s `--per-core` counter setup (`main()`,
   ~line 1327) explicitly excludes `CORE_INTEL_ATOM` from the per-core loop today — on a real hybrid
   system, `--per-core` silently collects zero counters from Atom (E-core) cores, so there's no
   existing per-core Atom topdown data to weight-aggregate over yet. In the default (non-`--per-core`)
   aggregate mode, a process-wide perf counter sums across whatever cores the OS scheduled the thread
   on — on a hybrid CPU that's an uncontrolled Atom/Core mix with no visibility into how much of each,
   which is the concrete case the "don't mix Atom+Core topdown into one headline number" caveat
   (Topdown deep-dive item 6, above) is about. `--affinity=coretype=<id>` already exists (`affinity.c`)
   but is ARM-only (clusters by the ARM-specific `MIDR_EL1` register); x86 hybrid detection for that
   same grouping is a separate, already-documented gap in that file. A reporting-only v1 (detect
   `cpu_info->is_hybrid` + topdown counters requested, warn when the collection mode can't account for
   the mix, no new raw-event code) was scoped and ready to implement when this was deferred — revisit
   that scope first if picked back up before hardware becomes available, since it needs no hardware to
   validate. **Update (2026-07-20):** AMD Zen5/Zen5c per-core classification shipped (see "Shipped
   since 4.1" above, real hardware was available for that vendor) and `--per-core` already collects
   Zen5c counters (unlike the Atom exclusion above), so the "no existing per-core data to weight-
   aggregate over" blocker is gone specifically for AMD — but the weighted-aggregate composite-topdown
   analysis itself is still unimplemented for every vendor including AMD, and `--affinity=coretype=<id>`
   is still ARM-only (x86 hybrid detection for that grouping remains the separate gap `affinity.c`
   already documents). This item stays open; AMD is just no longer blocked on hardware access the way
   Intel/ARM hybrid still are.

**Tier 4 — publishing/reporting expansion, needs 4.1's report studio:**

10. Static-site publishing pipeline (per-benchmark + suite + cross-suite pages from templates). Distinct
   from 4.1's per-run curation studio, not a replacement for it: the studio is where one report gets
   curated by a person; this is what turns *many* already-curated (or un-curated, template-driven)
   reports into a browsable site. Likely consumes the same export formats (WordPress/HTML/Markdown,
   4.1) rather than inventing a fourth.
11. Characterization badges + similarity panels in reports — a new block type in 4.1's curation studio
    once 4.2's archetype scorecard exists to draw a badge from, not a separate report surface.
12. Interactive tree/timeline drill-down, GPU phase overlays — the interactive counterpart to 4.1's
    static inclusion-depth mechanism (none/summary/excerpt/full) for the tree/interval blocks
    specifically; that mechanism stays the right default for a published, non-interactive report even
    once this exists.

**Tier 5 — report-layer additions on data already collected in 4.0:**

13. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
    process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
14. System (`--system`) → per-interface network attribution and local-vs-system-pressure
    attribution, plus steal-time capture (user/system/iowait are already captured and printed —
    `system.c`'s existing `/proc/stat` parsing — this item is the missing steal column and the
    analysis layer on top of what's already there, not the raw mix itself).
15. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
    `comm`-pattern role tagging).

**Tier 6 — GPU deeper profiling:**

16. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
    heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
17. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) —
    depends on 4.2's GPU fusion layer providing consistent per-metric data first.
18. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`,
    extended once GPU runs feed the same index.
19. Fold into general environment-comparability scoring (power cap, memory clock, thermal state,
    driver version) — no separate "GPU comparability score" needed; one scoring mechanism, not two.

**Tier 7 — infra:**

20. Low-overhead tracing alternative to `ptrace` (`ftrace` tracepoints or minimal eBPF) for
    `--tree`/`--tree-open` — `ptrace` context-switches on every syscall entry/exit, which skews the
    very counters being measured for I/O-heavy or fork-heavy workloads. Also the eventual fix for the
    observer-effect caveat noted under "Critical-path / synchronization-latency: what's left" above.
21. Collector-plugin implementation (perf stat / trace-cmd / GPU tools as collectors behind the
    `collector` field, normalization path) — the schema seam shipped in 4.0; this is the actual
    implementation of wrapping a non-wspy collector.
22. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
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
23. Collapse `wspy-run`'s builtin profiles onto native `--passes` bin-packing (moved from 4.2, 2026-07-21
    — see below for why). Deprioritized to 4.3 rather than dropped: low value relative to everything
    else on the 4.2/4.3 boards, no dependents, safe to leave alone indefinitely. Investigation before
    deferring found the item is already mostly done, worth recording so a future pass doesn't have to
    re-derive it: `deep-cpu`/`deep-gpu` (`wspy-run`'s builtin profiles) already collapsed their
    pure-counter middle pass onto `--passes=software,branch,ipc,topdown2,cache2,cache3,memory,float,
    topdown-frontend,topdown-optlb` back in 4.1. Their remaining separate passes (`systemtime`/
    `amdtopdown`/`gpu_busy`/`gpu_metrics`) all use `--interval 1`, which is hard-fatal'd against
    `--passes` (`wspy.c`, "no defined multi-pass merge semantics for periodic ticks") — a real
    architectural constraint, not a missed collapse; giving `--passes` an interval-compatible merge
    story would be a separate, larger redesign, not this item. `tree-heavy`/`gpu-compute` (`--tree`) and
    `ibs-basic`/`ibs-memory-deep` (IBS) are excluded from `--passes` the same way; `quick` is already one
    pass; `zen-portable`/`zen4plus-deep` just compose other profiles and inherit whatever those do. The
    only actual remaining candidate is **`deep-cpu-intel`**, which still hand-authors 4 separate `wspy`
    invocations (`software_branch`/`topdown`/`ipc_l2`/`backend`), none of which touch any
    `--passes`-incompatible flag — collapsing it to one `--rusage --passes=software,branch,ipc,topdown2,
    cache2,topdown-backend` pass is the entire remaining scope of this item. Real consequence worth
    flagging when this is picked up: that changes on-disk output shape from 4 files (each with its own
    manifest) to 1, which anything downstream assuming those 4 specific filenames (external scripts,
    `workload/cpu2017`'s driver if it references Intel-specific pass names, `tests/capability_matrix.sh`)
    would need checked against.
24. Detect and resume interrupted `wspy-run` profiles (raised 2026-07-21: a real host crash mid-batch,
    twice, with no way to tell from a report that the run never finished, and no way to pick up where
    it left off short of redoing already-completed passes). Two phases of very different size, second
    depends on the first:
    - **Phase A — surface incompleteness in reporting.** `generate_manifest()` (`wspy-run:772`) writes
      the run-level `manifest.json` (the one with per-pass `passes[]` status) exactly once, only after
      the entire pass loop (`for i in "${!PASS_NAMES[@]}"; do run_pass ...; done`) finishes — a crash of
      the whole host (or the `wspy-run` process itself) mid-loop kills it before that write, so the
      run directory ends up with whichever passes *did* finish cleanly (each pass's own per-pass output
      + `<pass-name>.manifest.json`, written by `wspy` itself as that pass completes) but no top-level
      `manifest.json` at all. That's a clean, already-computable signal needing zero new instrumentation:
      a unified-layout (`--suite`/`--benchmark`) run directory with no bare `manifest.json` is
      unambiguously "never finished" — distinct from a run that finished all its passes but whose child
      workload itself failed (which *does* get a `manifest.json`, just with a failing `exit_status`,
      already covered by `wspy-validate`'s PASS/WARN/FAIL and shouldn't be conflated with this). Surface
      this on `/report` (a clear "incomplete — N of M passes ran" banner, counting existing per-pass
      manifests against `PASS_NAMES`' expected count) and on `/history`'s search/filter (a new status
      value alongside its existing `run_status_from_passes()`/`run_status_from_exit_status()` derivation
      — see `CLAUDE.md`'s "historical run browser" entry).
    - **Phase B — resume, skipping already-completed passes.** A new `wspy-run --resume <existing-run-
      dir>` mode reusing that directory's existing `RUNROOT`/`RUN_ID` (not generating a fresh one, or
      the resumed passes would land somewhere new instead of alongside the ones already there). For each
      `PASS_NAMES` entry, skip re-running it only if *both* (a) that pass's own `<name>.manifest.json`
      already exists with a clean exit, and (b) its recorded configuration exactly matches what this
      invocation would run now — exact-match, not fuzzy, same "don't guess at approximate equivalence"
      idiom `compare_id_for_keys()`/`summary.c`'s `mixed-pmu` check already use elsewhere in this
      project, since silently trusting a stale pass run under a since-changed profile/flag set would be
      worse than just re-running it. The match check needs one new small piece of provenance: since
      `run_pass()` already threads `--config-option` per pass (`wspy-run:470-472`, existing plumbing, no
      wspy-side changes needed), `wspy-run` can record a hash of that pass's own resolved flag string as
      `--config-option pass_flags_hash=<hash>` and compare it against what it would compute for the same
      pass now. Deliberately does **not** attempt to resume a pass that was itself interrupted
      mid-execution — that pass's partial output is simply discarded and the whole pass re-run from
      scratch, since resuming a partially-written `--interval` CSV or a half-finished `--tree` capture
      mid-stream is a much harder, riskier problem than this item is trying to solve; only *entire,
      already-cleanly-finished* passes are ever skipped.
    - **Scope boundaries, both cross-referenced by name per this doc's own convention:** distinct from
      `wspy-queue`'s job lifecycle (`pending`/`running`/`done`/`failed`) — that's scheduling/retry of
      *whole new jobs*, not resuming partway through one already-multi-pass `wspy-run` invocation's own
      internal passes. Also distinct from 4.4's "Config-first experiment definition system" item, whose
      own "resumable/selective re-execution" is part of a much heavier full YAML/JSON suite/benchmark/
      repetition system — this item is the lightweight version, scoped specifically to `wspy-run`'s
      existing profile/pass model, and shouldn't be folded into or block on that bigger item.
25. Phoronix per-test option-combination count, surfaced ahead of running (raised 2026-07-21, while
    scoping 4.2's `--tree` pass timeout-sizing item: a real, recurring pain point is discovering
    *after* a long `batch-run` sweep that a test's full option matrix takes far longer than expected,
    then hand-building a shorter `-subset` profile only in hindsight — this item is knowing that
    ahead of time instead). Confirmed live against a real installed test profile
    (`~/.phoronix-test-suite/test-profiles/system/blender-1.2.1/test-definition.xml`) that
    `<TestSettings>` names the exact shape needed: one `<Option>` block per configurable dimension,
    each with a `<Menu>` of `<Entry>` choices — blender's own profile has a 5-entry "Blend File" option
    and a 2-entry "Compute" option, meaning a full `batch-run` sweep of just this one test is 5×2=10
    separate full renders, not 1. Parsing `<TestSettings>/<Option>/<Menu>/<Entry>` (counting `<Entry>`
    elements per `<Option>`, multiplying across every `<Option>` in the file) is cheap and purely
    static — no need to actually run anything — and follows the same "deliberately not a real XML
    parser" convention `ledger.c`'s existing `scan_phoronix_dependencies()`/`extract_external_deps()`
    already established for reading this same `test-definition.xml` file for a different field
    (`<ExternalDependencies>`). Natural home is `wspy-ledger` (per its own suggestion) alongside that
    existing Phoronix-profile-scanning logic — surfaced as a new annotation/warning column (e.g.
    "10 option combinations") next to a workload's existing done/skipped/unsupported/
    needs-tool-support status, flagged prominently above some threshold worth a human's attention
    before committing to a full `batch-run`. Also a natural future input to the (now shipped, see
    "Shipped since 4.1" above) `--tree` pass timeout-sizing item's own `BATCH_RUN_MULTIPLIER`: a
    `batch-run`'s real total time is much closer to (single-test estimate) × (this item's own
    combination count) than to the single-test estimate alone, which would replace that item's own
    blind 5.0 multiplier guess with a grounded number — noted here as a future connection, not a
    reason to block either item on the other or expand either one's current scope.

**Tier 8 — testing:**

26. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead
    guardrails — needs deterministic micro-workloads and 4.1's normalized store plus 4.2's
    stats/confidence infrastructure.
26. Contributor guide for adding a collector/metric/schema bump safely.

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
6. Process/thread migration diagnostics (did a process's threads actually move between cores during
   the run) — split out of 4.2's "Per-core imbalance/hot-core diagnostics" item, since it needs new
   instrumentation (periodic `/proc/<pid>/stat` `processor`-field sampling, or scheduler tracepoints)
   rather than just new analysis of data `--per-core` already collects. Natural pairing with 4.3's
   lower-overhead tracing backend if that lands first, but not a hard dependency.
7. Job-browsing view in the web UI — pushed out of 4.2 (2026-07-20). A queued job (`wspy-queue add`,
   or the Run tab's "Queue instead of running it now" checkbox) is visible today only via
   `wspy-queue list`/`show`, not from the web UI itself. Bundle in sharing structured configuration
   provenance with the job format (`web/joblib.py`'s job schema and `manifest.h`'s
   `configuration_provenance` are designed to be close in shape but aren't wired together yet).

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision. (Several
earlier open questions here — native multi-pass execution, ARM64 support, publication automation,
core/thread affinity, minimum metadata set for publishable — have been resolved by shipped work; see
"What shipped in 4.1" and "Shipped since 4.1" above rather than a stale "resolved" note here.)

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
