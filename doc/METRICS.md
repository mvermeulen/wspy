# Metrics Reference

This is the master index of every metric wspy's tools can produce: what it's called, where that name
comes from, and exactly how it's computed. It exists to answer two questions:

1. **For a person or an AI agent reading a `wspy`/`wspy-summary`/`wspy-archetype` output**: what does
   this column actually mean, and (where we've bothered to say) is a given value high, low, or fine.
2. **For us, deciding what belongs in the SQLite store (`store.c`)**: this is the working list we'll
   prune/refine as we decide which raw metrics deserve a normalized `run_features` entry (see
   [Database status](#database-status-legend) below), which are fine left as long-format
   `metric_values` rows, and which aren't worth persisting at all.

This document is *not* a tutorial on perf/topdown methodology and it is not a substitute for reading
`CLAUDE.md`'s Architecture table or the source. It only records: canonical name, one-line derivation,
where in the code that derivation lives, whether/how it currently reaches the database, and — only where
it's genuinely meaningful, not for every row — a rule-of-thumb for what counts as high/low. Guidance
given here is uncalibrated (rule-of-thumb from the code's own hardcoded thresholds, or plain domain
reasoning), not derived from any statistical baseline yet; expect this to be refined once we've run
calibration passes across real workloads.

## Naming convention

Where a metric already has a name in `wspy`'s CSV header or human-readable output, that name is used
verbatim here (`ipc`, not "instructions per cycle") — including inconsistent casing/spacing across
columns (`branch miss` has a space, `retire_pct` doesn't; that's the existing codebase, not a typo in
this doc). Metrics that are *derived* rather than raw pass-through (e.g. `ipc`, or `run_features` like
`parallelism_proxy`) are noted as such — see `CLAUDE.md`'s note on why we store `ipc` and not raw
`instructions`+`cpu-cycles`: the elapsed-normalized ratio is comparable across runs, the raw operand
counts mostly aren't.

For live counter output, `topdown.c:print_metrics()` is the dispatcher that walks the run's counter
groups and calls the matching `print_*()` formatter per group (by `cgroup->mask`) — it owns no metric
math of its own; every section below names the actual formatter function that does.

## Database status legend

- **[raw]** — a real `wspy` CSV column. `store.c:ingest_csv_metrics()` ingests *any* CSV column
  generically (header-driven typing) into the long/tall `metric_values` table, so every `[raw]` metric
  already reaches the store today, keyed by its CSV header text as `metric_name`.
- **[feature]** — additionally promoted into the normalized `run_features` table
  (`store.c:extract_run_features()`), i.e. already a first-class per-run summary value used by
  `wspy-archetype`/`wspy-summary`. Two flavors: a straight `AVG()` of one `[raw]` metric
  (`SIMPLE_METRIC_FEATURES[]`), or a genuinely derived statistic (coefficient of variation, fraction of
  ticks, rate-per-second) computed from one or more `[raw]` metrics.
- **[environment]** — lives in the manifest, promoted by `store.c:enrich_from_manifest()` into the
  normalized `run_environment` table (one row per run, not long-format) — not in `metric_values` at all.
- **[manifest-only]** — captured in the JSON manifest today but *not yet* ingested into any SQLite table.
  Candidates for a future `store.c` schema addition.
- **[human-only]** — computed and printed in `PRINT_NORMAL` (human-readable) mode but never emitted as a
  CSV column, so it isn't captured by anything downstream today. Candidates for promoting to a real CSV
  column if we want them in the store.
- **[categorical]** — a classification label, not a number; not part of `metric_values`' numeric
  convention, produced by `phase.c`/`archetype.c` from other metrics.

---

## Timing & process resource usage

Source: `topdown.c:print_usage()` (columns emitted regardless of counter flags), fed by a `struct rusage`
from `wait4(child_pid,...)` (normal mode) or `getrusage(RUSAGE_CHILDREN,...)` (`--tree` mode), both in
`wspy.c`.

- **elapsed** — `[raw]` wall-clock seconds, `finish_time - start_time` (both `CLOCK_REALTIME`). Not itself
  "high/low"-judged; it's the normalizer most other rate metrics divide by.
- **utime** — `[raw]` `ru_utime.tv_sec + ru_utime.tv_usec/1e6`, seconds of user-mode CPU time.
- **stime** — `[raw]` `ru_stime.tv_sec + ru_stime.tv_usec/1e6`, seconds of kernel-mode CPU time.
- **on_cpu** — `[human-only]` `(utime+stime) / elapsed / num_cores_available`, i.e. fraction of the
  available cores actually kept busy end-to-end. `num_cores_available` is this process's own outer
  affinity mask, not narrowed by `--affinity`. Guidance: close to `1.0 * (cores actually used)` for a
  CPU-bound single/multi-threaded run; well below 1 core's worth suggests the workload spent real wall
  time blocked (I/O, sleeping, waiting on another process) rather than computing.
- **nvcsw** — `[raw]` `ru_nvcsw`, voluntary context switches (the process blocked on something).
- **nivcsw** — `[raw]` `ru_nivcsw`, involuntary context switches (preempted by the scheduler).
- **inblock** — `[raw]` `ru_inblock`, block I/O input operations.
- **oublock** — `[raw]` `ru_oublock` (human-mode label reads "onblock"), block I/O output operations.
- **maxrss** — `[raw]` `ru_maxrss` (KB, per Linux convention).
- **minflt** — `[raw]` `ru_minflt`, minor page faults (satisfied without disk I/O).
- **majflt** — `[raw]` `ru_majflt`, major page faults (required disk I/O) — accumulates into the
  `fault_rate` feature below.
- **nswap** — `[raw]` `ru_nswap`, swap-out count.

## Derived rusage rate features (already in the store)

Source: `store.c:extract_run_features()`, computed once per run from the `[raw]` rusage metrics above
(`SUM()` across all rows, since these rusage fields are emitted once per run regardless of `--interval`).

- **fault_rate** — `[feature]` `(minflt+majflt) / elapsed_seconds`, page faults per second.
- **ctxswitch_rate** — `[feature]` `(nvcsw+nivcsw) / elapsed_seconds`, context switches per second.

## IPC & topdown (L1 breakdown)

Source: `topdown.c:print_ipc()` / `print_topdown()`. All topdown values are read from `read_counters()`
already multiplex-scaled: each raw counter read carries a `time_running/time_enabled` ratio for the
window since the last read, and if it was only partially scheduled on the PMU that window, its raw delta
is scaled up by `enabled_delta/running_delta` before any print function ever sees `.value`.

- **ipc** — `[feature]` (as `ipc_mean`, straight `AVG()`) `instructions / cpu-cycles`, both
  multiplex-scaled values from the same counter group. This is why we store `ipc` and not raw
  `instructions`/`cpu-cycles` separately — the ratio is comparable across differently-scaled or
  differently-multiplexed runs, the raw operand counts mostly aren't. Guidance: the code itself flags
  `> 3.0` "high" and `< 0.7` "low" in human output — treat these as rough eyeballing thresholds for a
  general-purpose x86/ARM core, not a calibrated verdict; a memory-bound or heavily-speculative workload
  can legitimately sit well under 1.0, and a tight vectorized loop can legitimately clear 3–4.
- **GHz** (annotation next to raw `cpu-cycles`, not a CSV column) — `[human-only]`
  `cpu_cycles / elapsed / 1e9 / num_cores_available / (aflag ? num_cores_available : 1)`.
- **retire** — `[feature]` (`retire_pct`) `retiring_slots / slots_no_contention * 100`. Fraction of
  pipeline slots that produced useful retired work. Vendor sourcing: Intel `core.topdown-retiring`; AMD
  `ex_ret_ops`; ARM `op_retired`.
- **frontend** — `[feature]` (`frontend_pct`) `frontend_slots / slots_no_contention * 100`. Fraction of
  slots lost to the frontend not supplying enough work (fetch/decode). Intel `core.topdown-fe-bound`; AMD
  `de_no_dispatch_per_slot.no_ops_from_frontend`; ARM `stall_slot_frontend`.
- **backend** — `[feature]` (`backend_pct`) `backend_slots / slots_no_contention * 100`. Fraction of slots
  lost to the backend being unable to accept work (execution ports, load/store, memory). Intel
  `core.topdown-be-bound`; AMD `de_no_dispatch_per_slot.backend_stalls`; ARM `stall_slot_backend`.
- **speculate** — `[feature]` (`speculate_pct`) `speculation_slots / slots_no_contention * 100`. Fraction
  of slots spent on work later thrown away (branch mispredicts, machine clears). Intel
  `core.topdown-bad-spec`; AMD `safe_sub(de_src_op_disp.all, retiring)`; ARM
  `safe_sub(op_spec, op_retired)`.
- **confidence** — `min()` of the `time_running/time_enabled` ratio across every counter feeding
  retire/frontend/backend/speculate — how much of the measurement window each contributing counter was
  actually scheduled on the PMU. Guidance: below `0.90` (90%), the run's own human output labels this
  "low-confidence" — heavy multiplexing means the topdown split above is a scaled estimate, not a direct
  count; treat retire/frontend/backend/speculate with more skepticism the lower this is.
- **sanity** — `(retire+frontend+backend+speculate) * 100 / slots_no_contention`, ideally ≈100%. This is
  a *measurement-quality* check, not a workload characteristic: it should sum to 100% by topdown's own
  definition (the four categories are meant to be exhaustive and non-overlapping). Guidance: human output
  flags a drift of more than `±5` percentage points from 100 (x86 only, not checked on ARM) — treat a
  flagged run's retire/frontend/backend/speculate split with caution; it means the four independently-
  read hardware counters didn't add up, not that the workload is unusual.
- **contention_pct** — AMD only; `smt_contention_slots / slots * 100` (note: divided by raw `slots`, not
  `slots_no_contention`, unlike every other `_pct` column here) — slots lost specifically to SMT sibling
  contention rather than to the workload's own frontend/backend/speculation.
- **retire_ucode_pct**, **retire_fastpath_pct** — `[feature]` L2 split of `retire`: `retire_ucode` = slots
  retiring via microcode-assisted ops (Intel `core.topdown-heavy-ops`; AMD `ex_ret_ucode_ops`),
  `retire_fastpath` = `safe_sub(retire, retire_ucode)`, both as `% of slots_no_contention`. High
  `retire_ucode_pct` can indicate reliance on complex/microcoded instructions rather than simple
  fast-path ones. Promoted to `run_features` (issue #232) — same shape as `float_pct`, no `archetype.c`
  consumer yet.
- **frontend_latency_pct**, **frontend_bandwidth_pct** — L2 split of `frontend`: latency-bound (fetch
  stalls, e.g. icache/iTLB misses; Intel `core.topdown-fetch-lat`) vs. bandwidth-bound (frontend can't
  decode/issue fast enough even without a stall).
- **backend_cpu_pct**, **backend_memory_pct** — L2 split of `backend`: `backend_memory` = slots lost
  waiting on the memory subsystem (Intel `core.topdown-mem-bound`; AMD ratio-scaled from
  `ex_no_retire.load_not_complete/not_complete`), `backend_cpu` = the rest (execution port/resource
  contention). A workload dominated by `backend_memory_pct` is memory-latency/bandwidth-bound; dominated
  by `backend_cpu_pct` is core-execution-resource-bound. `backend_memory_pct` alone is `[feature]`
  (same name) — the anchor `wspy-archetype`'s `memory_attribution_locus` axis gates on, since it isolates
  the memory-specific portion of a backend stall rather than `backend_pct`'s coarser whole-backend read.
- **spec_branch_pct**, **spec_pipeline_pct** — L2 split of `speculate`: mispredicted-branch-driven vs.
  other pipeline-clear-driven speculation waste.

`safe_sub(a,b) = a>=b ? a-b : 0` guards every subtraction of independently-read counters (no guaranteed
parent≥child ordering) against unsigned wraparound — if you see an implausible exact `0.00` in an
`*_pct` column that "should" be small-but-nonzero, this clamp is a likely explanation.

**Reading `counters.txt` (human `PRINT_NORMAL` mode) for this group specifically:** the on-screen label
differs from the CSV/feature name in two places — the CSV column is `speculate` but the text row is
labeled `speculation`, and every top-level line (`retiring`/`frontend`/`backend`/`speculation`) prints
**two** percentages, e.g. `retiring 29916616347812 # 27.6% (47.0%)`: the first is `value/slots*100` (share
of *all* pipeline slots, contention included), the parenthetical second is `value/slots_no_contention*100`
— that second one is what the CSV column and `retire_pct`/etc. features actually store. The `--
ucode`/`-- fastpath`/`-- latency`/`-- bandwidth`/`-- cpu`/`-- memory`/`-- branch mispredict`/`-- pipeline
restart` sub-lines each print only one percentage, also against `slots_no_contention`, matching their
`*_pct` CSV columns directly. One more wrinkle: the `smt-contention` line's own trailing `( 0.0%)` is a
**hardcoded literal**, not a second computed value (`topdown.c`'s format string only has one `%4.1f`
argument for that line) — don't read it as data.

## Topdown backend deep-dive (`--topdown-backend`)

Source: `topdown.c:print_topdown_be()`. Intel/ARM only (AMD unsupported, no columns emitted). Its own
independent `cpu-cycles` counter group — genuinely a separate measurement, not guaranteed to sum exactly
to `backend_memory_pct` above.

- **l1_bound**, **l2_bound**, **l3_bound**, **dram_bound**, **store_bound** — `[raw]`, each as % of *this
  group's own* `cpu-cycles`. Intel: `l1_bound=safe_sub(exe_activity.bound_on_loads, memory_activity.stalls_l1d_miss)`,
  `l2_bound=safe_sub(l1_miss, memory_activity.stalls_l2_miss)`,
  `l3_bound=safe_sub(l2_miss, memory_activity.stalls_l3_miss)`, `dram_bound=l3_miss` (raw),
  `store_bound=exe_activity.bound_on_stores`. ARM only populates `l3_miss`→`dram_bound`
  (`stall_backend_mem`); l1/l2 stay 0. Guidance: this is *where in the cache hierarchy* backend memory
  stalls concentrate — `dram_bound` dominant means the working set doesn't fit any cache level; `l1_bound`
  dominant on an otherwise backend-bound run can mean poor load scheduling more than genuine capacity
  misses.
- **l1_bound_slots_pct**, **l2_bound_slots_pct**, **l3_bound_slots_pct**, **dram_bound_slots_pct**,
  **store_bound_slots_pct** — `[raw]`, same numerators expressed instead as `% of slots_no_contention`
  (the shared denominator from `print_topdown()`), for direct comparison against `backend_memory_pct`.

## Topdown frontend/op deep-dive (AMD only)

Source: `topdown.c:print_topdown_fe()` (`--topdown-frontend`) and `print_topdown_op()`
(`--topdown-opcache`).

- **icache** — `[feature]` (`icache_miss_pct`) **CSV column** = `ic_tag_hit_miss.instruction_cache_miss /
  ic_tag_hit_miss.instruction_cache_accesses * 100` (AMD instruction-cache *miss rate*, distinct from the
  cross-vendor generic `L1-icache miss` column elsewhere). **Careful reading `counters.txt`**: the human
  text has *two* separate rows sharing this name space — a row literally labeled `icache` showing the raw
  access *count* (`icache_access`, annotated "`X.XXX icache per 1000 inst`") and a separate row labeled
  `icache miss` showing the actual miss-rate percentage. The CSV column's value corresponds to the `icache
  miss` text row, **not** the text row also named `icache` — don't be misled by the name match.
- **itlb1** — `[raw]` `(l1_tlb_miss_l2_hit + l1_tlb_miss_l2_miss) * 1000 / instructions` — L1 iTLB misses
  per 1000 instructions. Text row: `l1 iTLB miss`.
- **itlb2** — `[feature]` (`itlb_miss_per1k`) `l1_tlb_miss_l2_miss * 1000 / instructions` — L2 iTLB
  misses (i.e. misses that also missed the L2 TLB) per 1000 instructions. Text row: `l2 iTLB miss`.
- **tlbflush** — `[raw]` `ls_tlb_flush.all * 1000 / instructions` — TLB flushes per 1000 instructions.
  Text row: `tlb flush`.
- **opcache** — `[raw]` **CSV column** = `op_cache_hit_miss.op_cache_miss /
  op_cache_hit_miss.all_op_cache_accesses * 100` (AMD micro-op cache *miss rate*). Same text/CSV name
  collision as `icache` above: the human text's `opcache` row is the raw access count
  ("per 1000 inst"-annotated), the actual miss-rate percentage is on the separate `opcache miss` text row.
- **dtlb1** — `[raw]` `ls_l1_d_tlb_miss.all * 1000 / instructions` — L1 dTLB misses per 1000 instructions.
  Text row: `l1 dTLB miss`.
- **dtlb2** — `[feature]` (`dtlb_miss_per1k`) `ls_l1_d_tlb_miss.all_l2_miss * 1000 / instructions` — L2
  dTLB misses per 1000 instructions. Text row: `l2 dTLB miss`.

## Branch prediction

Source: `topdown.c:print_branch()`.

- **branch miss** — `[feature]` (`branch_mispredict_pct`) `branch_misses / branches * 100`. Vendor
  sourcing: Intel `br_misp_retired.all_branches`/`br_inst_retired.all_branches`; AMD
  `branch-misses`/`branch-instructions`; ARM `br_mis_pred_retired`/`br_retired`. Guidance:
  `wspy-archetype` itself treats `<= 2.0%` as "straight-line" control flow and anything higher as
  "branch-heavy" (`archetype.c`) — a workable rule of thumb, not a hard architectural boundary.
- **branches per 1000 inst**, **conditional per 1000 inst**, **indirect per 1000 inst** — `[human-only]`
  branch-instruction density normalized by instruction count (not by time), useful for judging how
  control-flow-heavy code is independent of IPC.
- AMD extras: **near_return**, **near_return_mispredicted**, **indirect_branch_mispredicted** — `[raw]`
  raw counts; human mode also derives near-return and indirect mispredict rates as percentages.
- ARM extras: **br_immed_retired**, **br_return_retired**, **br_pred**, **br_mis_pred** — `[raw]` raw
  counts + per-1000-inst density in human mode.

## Cache (generic, cross-vendor)

Source: `topdown.c:print_cache()` (shared helper), driving four callers over generic `PERF_TYPE_HW_CACHE`
events (kernel-resolved per PMU, no vendor table needed).

- **L1-dcache miss** — `[feature]` (`dcache_miss_pct`, from `print_dcache()`) `l1d-read-miss / l1d-read * 100`.
- **L1-icache miss** — `[raw]` (from `print_icache()`) `l1i-read-miss / l1i-read * 100`.
- **opcache miss** — `[raw]` (from `print_opcache()`, distinct from the AMD-only `opcache` column above)
  `op_cache_hit_miss.op_cache_miss / op_cache_hit_miss.all_op_cache_accesses * 100`.
- Human mode also reports each of these as `access-count / instructions * 1000` ("per 1000 inst").

## L2/L3 cache (vendor-specific)

Source: `topdown.c:print_l2cache()` / `print_l3cache()`.

- **l2miss** — `[feature]` (`l2_miss_pct`) `l2_miss / l2_access * 100`. Intel: `l2_request.miss /
  l2_request.all`. ARM: `l2d_cache_lmiss_rd / l2d_cache`. AMD (composite, includes prefetch accounting):
  `l2_access = l2_request_g1.all_no_prefetch + l2_pf_hit_l2 + l2_pf_hit_l3 + l2_pf_miss_l3`;
  `l2_miss = l2_cache_req_stat.ic_dc_miss_in_l2 + l2_pf_hit_l3 + l2_pf_miss_l3`. AMD human text prints
  this overall ratio as the annotation on the **`l2 miss from l1`** row (which itself is only the
  `l1_miss_l2_miss`/`ic_dc_miss_in_l2` term, i.e. the *demand-miss* component of the numerator) — the code
  has an explicit comment warning that a prior `wspy-analyze` run misread that pairing as "this row's own
  hit/miss rate," inverting hit vs. miss; the percentage is the *whole-group* `l2miss`, combining
  demand-miss and prefetch-miss sources, not just the row it's printed next to. The `l2 hit from l1`/`l2
  hit from l2 pf`/`l3 hit from l2 pf`/`l3 miss from l2 pf` rows carry no percentage annotation at all
  (raw counts only).
- **l3miss** — `[feature]` (`l3_miss_pct`) AMD only, requires `/sys/devices/amd_l3/type` (silently
  unavailable otherwise): `l3_lookup_state.l3_miss / l3_lookup_state.all_coherent_accesses_to_l3 * 100`.

## Memory bandwidth

Source: `topdown.c:print_memory()`. CSV column: `bandwidth` (MB/s), assumes a 64-byte cache line.

- **bandwidth** — `[raw]` ARM: `mem_access * 64 / 1024 / 1024 / elapsed`. AMD:
  `(ls_data_cache_refills.local_all + .remote_all + ls_hwpref_data_cache_refills.local_all + .remote_all)
  * 64 / 1024 / 1024 / elapsed`. AMD human mode additionally splits **local bandwidth**/**remote
  bandwidth** `[human-only]` by NUMA locality (requires a `/sys/devices/system/node/node1` to detect
  "remote" at all — single-node hosts never populate this split).

## Floating point (AMD only)

Source: `topdown.c:print_float()`. CSV column: `float`.

- **float** — `[feature]` (`float_pct`, `SIMPLE_METRIC_FEATURES`, issue #227).
  `(fp_ret_fops_AVX512 + fp_ret_fops_AVX256 + fp_ret_fops_AVX128 + fp_ret_fops_MMX + fp_ret_fops_scalar) /
  instructions * 100`. Human mode also breaks out each SIMD width as "per 1000 inst".

## TLB (generic, cross-vendor)

Source: `topdown.c:print_cache()` helper again, via `print_itlb()`/`print_dtlb()`.

- **iTLB miss** — `[feature]` (`itlb_generic_miss_pct`) `iTLB-load-misses / iTLB-loads * 100`. Distinct
  from `itlb2`/`itlb_miss_per1k` above (AMD-only, `--topdown-optlb`, a per-1000-inst rate) — this is the
  cross-vendor, percentage-based pair `--tlb` alone collects.
- **dTLB miss** — `[feature]` (`dtlb_generic_miss_pct`) `dTLB-load-misses / dTLB-loads * 100`. Same
  distinction from `dtlb2`/`dtlb_miss_per1k` as `iTLB miss` above.

## ARM-only raw dumps (no ratio columns)

Source: `topdown.c:print_arm_dcache_mem()` / `print_arm_icache_tlb()` / `print_arm_mem_align_tlb()`. Each
column is a raw event count plus a `per 1000 inst` annotation in human mode, no headline ratio: `l1d_cache_refill`,
`l1d_tlb_refill`, `l2d_cache_refill`, `l2d_tlb_refill`, `l1i_cache_refill`, `l1i_tlb_refill`,
`l2i_tlb_refill`, `dtlb_walk`, `itlb_walk`, `ld_align_lat`, `st_align_lat`. All `[raw]`.

## Software counters

Source: `topdown.c:print_software()`, `PERF_TYPE_SOFTWARE` events (`software_counter_group()`). All
`[raw]`, all raw counts; every counter except `cpu-clock`/`task-clock` is additionally annotated
`value/task_time` ("/sec") in human mode.

- **cpu-clock**, **task-clock** — scheduler-observed clock time, annotated in seconds (`/1e9`) in human
  mode.
- **page faults**, **major page faults**, **minor page faults** — `PERF_COUNT_SW_PAGE_FAULTS[_MAJ/_MIN]`.
  Redundant with rusage's `minflt`/`majflt` above but sourced from `perf` rather than `wait4`/`getrusage`.
- **context switches** — `PERF_COUNT_SW_CONTEXT_SWITCHES`. Redundant with rusage's `nvcsw+nivcsw` but
  doesn't distinguish voluntary/involuntary.
- **cpu migrations** — `PERF_COUNT_SW_CPU_MIGRATIONS`, times the scheduler moved this task to a different
  core. High values on a `--affinity`-pinned run suggest the pin didn't take effect as expected.
- **alignment faults**, **emulation faults** — architecture-trap counters, near-always 0 on modern x86/ARM
  hardware for normal userspace code; a nonzero value is itself the signal.

## System-wide

Source: `system.c:print_system()`, gated per-field by `system_mask` bits, all `[raw]`.

- **load** — `/proc/loadavg` field 1 (1-minute load average), direct read.
- **runnable** — `/proc/loadavg` field 4 (currently-runnable process count), direct read.
- **cpu** — `(usertime+systemtime delta) / elapsed / num_procs * 100`, from `/proc/stat`'s `cpu ` line
  fields 1&3. Note: field 2 (`nice` time) is read but *not* folded into this percentage — a workload
  running at a non-zero nice value won't be reflected here.
- **idle** — `idletime delta / elapsed / num_procs * 100` (`/proc/stat` field 4).
- **iowait** — `iowaittime delta / elapsed / num_procs * 100` (`/proc/stat` field 5). High values mean the
  system was blocked on storage/network I/O rather than genuinely idle or computing.
- **irq** — `(irq delta + softirq delta) / elapsed / num_procs * 100` (`/proc/stat` fields 6+7, combined).
- **freq** — average of every `/sys/.../cpufreq/scaling_cur_freq` across all CPUs (kHz→MHz); `0.0` on a
  VM/host with no `cpufreq` exposed.
- **cpu_temp** — average of matching hwmon `temp*_input` sensors (`k10temp`/`coretemp`/`cpu_thermal`
  drivers, preferring a `Tctl`/`Tdie`/`Package id N` labeled sensor); `0.0` if none found.
- **net &lt;iface&gt;** — combined RX+TX byte delta per interface (`/proc/net/dev` fields 1+9 summed, not
  reported separately).
- **disk &lt;dev&gt; read/write/time** — from `/sys/block/<dev>/stat`: `read`/`write` = sector-delta × 512
  bytes (sectors are always 512 bytes regardless of the device's actual logical block size); `time` = the
  kernel's own "I/O time" field delta (ms), not a sum of read+write time. Loop/ram/zram devices excluded.
- **mem_free_mb**, **mem_cached_mb**, **mem_dirty_mb**, **mem_writeback_mb**, **swap_free_mb**,
  **committed_as_mb** — point-in-time (not delta) reads from `/proc/meminfo`, KB→MB.
- **gpu_busy** — `amd_sysfs_gpu_busy_percent()` if AMD sysfs available, else a best-effort
  `/sys/class/devfreq/*/load` scan for an iGPU, else `0`. Only collected when `SYSTEM_GPU` is set (GPU
  flags OR this in regardless of `-s`/`--system`).

## Power (RAPL, `--power`)

Source: `power.c` (PMU/scale discovery) + `topdown.c:print_power()`/`print_power_core()`. Needs root or
`CAP_PERFMON`. Incompatible with `--passes` (fatals).

- **pkg_joules** — `[raw]` multiplex-scaled raw-LSB delta × the PMU's own `.scale` sysfs value (not
  hardcoded — read per-host). Caveat: the very first `--interval` tick's value includes wspy's own 2-
  second pre-launch sleep window, since the counter starts accumulating before the workload actually
  starts.
- **pkg_watts** — `[raw]` `pkg_joules / (that read's actual scheduled window in seconds)` — correct even
  under `--interval`/multiplexing, unlike a naive `joules/elapsed`.
- **core_joules**, **core_watts** — `[raw]` same shape, from the `power_core` PMU, needs `--power
  --per-core` together; `0.0` if the read's target CPU wasn't the PMU's "representative" CPU for that
  core.

## AMD IBS (`--ibs-basic` / `--ibs-memory-deep`)

Source: `ibs.c` + `topdown.c:print_ibs()`. System-wide only. Format-field offsets are looked up via sysfs
(`ibs_pmu_format()`), never hardcoded, so unsupported hardware degrades to "unfiltered" instead of
failing.

- **ibs_fetch** — `[raw]` raw sampled-fetch-event count.
- **ibs_op** — `[raw]` raw sampled-macro-op count; under `--ibs-memory-deep`, filtered to L3-miss-only
  and/or a load-latency threshold if the running kernel/CPU exposes those format fields.
- **ibs_op_unfiltered** — `[raw]`, memory-deep only: a second, unfiltered IBS-op event, used only as the
  denominator below.
- **ibs_op_accepted_ratio** — `[raw]` `ibs_op / ibs_op_unfiltered` — fraction of sampled ops the
  l3missonly/ldlat filters actually accepted; shows how much filtering skews the effective sampling
  period, not a workload characteristic per se.
- **ibs_l3missonly**, **ibs_ldlat_threshold**, **ibs_fetchlat_threshold** — `[raw]` the filter
  configuration actually applied (echoes back what was requested vs. what the hardware supported).

### AMD IBS sampling mode (`--ibs-sample`)

A different capture mode from `--ibs-basic`/`--ibs-memory-deep` above: instead of one aggregate hardware
counter, `ibs_sample.c` mmaps the `ibs_fetch`/`ibs_op` PMUs' perf ring buffers (`PERF_SAMPLE_RAW`) and
decodes each individual IBS sample record at end-of-run (`ibs_sample_drain()`), building rate estimates
from the decoded field values. Source: `ibs_sample.c:print_ibs_sample()`. CSV header:
`ibs_sample_fetch_count,ibs_sample_ic_miss_rate,ibs_sample_l1tlb_miss_rate,ibs_sample_l2tlb_miss_rate,ibs_sample_op_count,ibs_sample_dc_miss_rate,ibs_sample_dc_l1tlb_miss_rate,ibs_sample_dc_l2tlb_miss_rate,ibs_sample_brn_misp_rate,ibs_sample_lost,ibs_sample_dram_rate,ibs_sample_remote_node_rate`.
Important caveat shared by every column here: these are computed once from one ring-buffer drain at
end-of-run, not a rolling window — under `--interval`, every periodic tick row reports `0` for all of
them and only the final tail row is populated.

- **ibs_sample_fetch_count** — `[raw]` count of decoded IBS-fetch samples actually drained (`fs->fetch_count`).
- **ibs_sample_ic_miss_rate** — `[raw]` `fetch_ic_miss_count / fetch_count`.
- **ibs_sample_l1tlb_miss_rate** — `[raw]` `fetch_l1tlb_miss_count / fetch_count`.
- **ibs_sample_l2tlb_miss_rate** — `[raw]` `fetch_l2tlb_miss_count / fetch_count`.
- **ibs_sample_op_count** — `[raw]` count of decoded IBS-op samples actually drained (`os->op_count`).
- **ibs_sample_dc_miss_rate** — `[feature]` (`ibs_dc_miss_pct`) `op_dc_miss_count / op_count` (data-cache
  miss rate among sampled ops).
- **ibs_sample_dc_l1tlb_miss_rate** — `[feature]` (`ibs_dc_l1tlb_miss_pct`) `op_dc_l1tlb_miss_count / op_count`.
- **ibs_sample_dc_l2tlb_miss_rate** — `[feature]` (`ibs_dc_l2tlb_miss_pct`) `op_dc_l2tlb_miss_count / op_count`.
- **ibs_sample_brn_misp_rate** — `[raw]` `op_brn_misp_count / op_brn_ret_count` — note the denominator is
  *branch-retiring ops only*, not `op_count` like every other rate here; don't divide by the wrong base
  when recomputing this from raw fields.
- **ibs_sample_lost** — `[raw]` `fs->samples_lost + os->samples_lost` — count of samples the kernel
  ring buffer dropped (overrun) before they could be drained, combined across both the fetch and op
  streams. This is a *measurement-quality* signal, not a workload characteristic: human output
  explicitly warns that when nonzero, every rate above is "a lower bound" — treat a run with nonzero
  `ibs_sample_lost` as under-sampled, not as evidence the true miss rates are actually lower.
- **ibs_sample_dram_rate** — `[feature]` (`ibs_dram_pct`) `op_dram_count / op_count` — fraction of sampled
  ops whose data source was DRAM (as opposed to some cache level).
- **ibs_sample_remote_node_rate** — `[feature]` (`ibs_remote_node_pct`) `op_remote_node_count / op_count` —
  fraction of sampled ops whose data source was a remote NUMA node.
- **ibs_sample_data_src_breakdown** — `[human-only]` (`print_ibs_sample_data_src_breakdown()`) a full
  named percentage histogram of every sampled op's sourcing location (`op_data_src_count[]`), keyed by
  one of two AMD-documented category-name tables
  depending on whether the `zen4_ibs_extensions` PMU capability is enabled on this host (`default` vs.
  `zen4_ibs_extensions` scheme — different hardware generations encode different category sets at the
  same index, so the scheme actually active is printed alongside the breakdown). Deliberately
  **never emitted as CSV** (`ibs_sample.h`'s own design note: cross-scheme category names aren't stable
  enough to be a schema'd column) — read only from human-mode output. `dram_rate`/`remote_node_rate`
  above are the two categories from this same histogram considered stable enough to promote to real
  columns.

## GPU

Three independent backends, gated by `SYSTEM_GPU`/build flags — see `CLAUDE.md`'s GPU support section for
which flag maps to which backend. The CSV/human row for the fused `--gpu-metrics` columns below is
emitted by `topdown.c:print_gpu_metrics()`, which just formats whatever `gpu_fusion_combine()` (below)
already resolved — no arithmetic of its own.

- **gpu_busy_percent** (`amd_sysfs.c`) — `[raw]` direct integer read of
  `/sys/class/drm/card<N>/device/gpu_busy_percent`.
- **temp_gfx**, **gfx_activity**, **gfx_power**, **gfxclk_freq** (`amd_sysfs.c`, `gpu_metrics` blob) —
  `[raw]` version-dispatched (`format_revision` 1/2/3) parse; units/scale differ by revision
  (temp/power are raw on v1, `/100.0`/`/1000.0` on v2/v3; `gfxclk_freq` is 0 on v1, which lacks it).
- **gpu_temp**, **gpu_activity**, **gpu_power**, **gpu_freq**, **gpu_vram_used**, **gpu_vram_total**,
  **gpu_temp_source**, **gpu_activity_source** (`gpu_fusion.c`, `--gpu-metrics`) — `[raw]` "first
  available source wins" reconciliation: sysfs preferred for temp/activity/power/freq (all four sourced
  together when sysfs provides them); AMD SMI (`amdsmi_get_gpu_metrics_info()`) fills temp/activity only
  if sysfs didn't; power/freq have no SMI fallback; vram is SMI-only (`amdsmi_get_gpu_vram_usage()`), no
  sysfs source. `*_source` columns record which backend actually supplied temp/activity that row.
- **nv_gpu_busy** (`nvidia_nvml.c`) — `[raw]` `nvmlDeviceGetUtilizationRates()`'s `gpu` field (%).
- **nv_vram_used_mb**, **nv_vram_total_mb** (`nvidia_nvml.c`) — `[raw]` `nvmlDeviceGetMemoryInfo()`'s
  `used`/`total` bytes `/ (1024*1024)`.

## Counter coverage

Source: `coverage.c`. Not a workload metric — a measurement-quality metric, describes how much of the
*requested* counter set the hardware actually delivered.

- **counters_measured**, **counters_requested** — `[raw]` running tallies: `coverage_requested`
  increments on every attempted `perf_event_open()`, `coverage_measured` only when it succeeded. Human
  mode reports `"N/M measured"` plus a line per unavailable counter (group, label, errno). Guidance: a
  measured/requested ratio well under 1.0 means some of the metrics above are missing or zero for reasons
  that have nothing to do with the workload (PMU budget exhaustion, permissions, unsupported hardware) —
  check this before reading a suspiciously-flat or -zero topdown/cache column as a real workload trait.

## Derived run features (store-only, not raw CSV columns)

Source: `store.c:extract_run_features()`. These are computed once per run directly from already-ingested
`metric_values` rows — they're the store's own answer to "what's a good numeric summary/fingerprint of
this whole run," and are what `wspy-archetype`'s clustering/nearest-neighbor features draw on.

- **phase_stability** — `[feature]` fraction of distinct `--interval` ticks `phase.c` classified as
  `steady` vs. any other phase (`warmup`/`degraded`), counting distinct tick ordinals (not raw
  long-format rows, which would double-count a tick once per counter column). Guidance:
  `wspy-archetype` itself thresholds this — `<= 0.4` "erratic", `<= 0.8` "phased", else "steady"
  (`archetype.c`).
- **parallelism_proxy** — `[feature]` cross-core coefficient of variation of mean per-core `ipc`
  (`stddev/|mean|`, sample stddev with n-1 denominator, needs ≥2 cores with per-core data). Answers "is
  work balanced across the cores that were active." Guidance: `wspy-archetype` thresholds `<= 0.15`
  "balanced-parallel", else "imbalanced".
- **active_core_count** — `[feature]` count of cores whose mean `ipc` exceeded
  `ACTIVE_CORE_IPC_THRESHOLD` (meaningful even for a single active core, unlike `parallelism_proxy`).
- **ipc_p10**, **ipc_p90**, **ipc_iqr** — `[feature]` distribution-shape features over one run's own
  `--interval` tick-level `ipc` values (10th/90th percentile, interquartile range); requires at least
  `QUANTILE_MIN_TICKS` qualifying ticks or all three report unavailable. (`extract_quantile_features_for_metric()`
  is written to be reusable for other metrics beyond `ipc` — currently only wired up for `ipc`.)
- **retire_pct**, **frontend_pct**, **backend_pct**, **speculate_pct**, **dcache_miss_pct**,
  **icache_miss_pct**, **l2_miss_pct**, **l3_miss_pct**, **branch_mispredict_pct**, **itlb_miss_per1k**,
  **dtlb_miss_per1k**, **smt_contention_pct**, **ibs_dc_miss_pct**, **ibs_dram_pct**,
  **itlb_generic_miss_pct**, **dtlb_generic_miss_pct** — `[feature]` straight `AVG()` of the
  correspondingly-named `[raw]` metric across the whole run (see `SIMPLE_METRIC_FEATURES[]` in `store.c`
  for the exact metric_name each maps to — several feature names differ from their source CSV column,
  e.g. `icache_miss_pct` averages the AMD `icache` column, not the generic `L1-icache miss` column;
  `itlb_miss_per1k`/`dtlb_miss_per1k` and `itlb_generic_miss_pct`/`dtlb_generic_miss_pct` are two
  genuinely different TLB signals, AMD-only-per-1000-inst vs. cross-vendor-percentage respectively, not
  a naming variant of the same one).

## Per-core imbalance diagnostics (`wspy-core-report`)

Source: `core_report.c`. Not a fixed set of named metrics — a **generic statistical summary applied to
every numeric column** of an existing `--per-core --csv` file (column identity decides what's a metric,
same convention as `store.c`/`plot.c`; dimension columns `core`/`time`/`phase` and per-run-constant
bookkeeping columns like coverage/IBS-filter-config are excluded). Must be run on the same host (or one
with identical topology) that collected the CSV — core classes are re-detected fresh from *this* host at
report time, not read from the CSV. Applies to any `[raw]`/`[feature]` metric above that was collected
`--per-core` (most often `ipc`, but works identically for e.g. `backend_memory_pct` or `l2miss`). Not yet
promoted into the SQLite store (a post-hoc CLI report over a CSV file, like `wspy-plot`). Printed by
`print_stats_human()`/`print_stats_csv()`.

- **n** — count of distinct cores this metric had a value for.
- **mean**, **min**, **max** — cross-core mean/min/max of that metric's per-core value (a core's own
  values are first collapsed to their mean if the CSV has more than one row per core, e.g. `--interval`).
- **stddev** — cross-core sample standard deviation.
- **cv_percent** — `stddev / mean * 100`, the coefficient of variation — the actual "imbalance" figure:
  how much this metric varies core-to-core relative to its own average. Same statistical shape as
  `parallelism_proxy` above, but per-metric and computed post-hoc from a CSV rather than baked into
  `run_features` at ingest time.
- **hot core** / **cold core** — the core index holding the max / min value for this metric, so a high
  `cv_percent` can be traced back to *which* core is the outlier.
- When cores aren't all the same type (ARM big.LITTLE, Intel Atom+Core hybrid, AMD Zen5/Zen5c), the same
  n/mean/min/max/stddev/cv_percent/hot/cold breakdown is additionally reported once per core class,
  instead of lumping every core together — useful for telling "expected asymmetry from a hybrid part"
  apart from genuine scheduling/imbalance.

## Classification (categorical, not numeric)

Not database metrics in the `metric_values`/`run_features` sense — labels derived *from* the metrics
above, for human/agent consumption.

- **phase** (`warmup` / `steady` / `degraded`) — `[categorical]`, `phase.c`. Per-`--interval`-tick
  hysteresis state machine over `ipc`: buffers a warmup window, transitions to `steady` once IPC's
  coefficient of variation drops below a threshold (baseline = window mean); drops to `degraded` when
  `ipc/baseline` falls below `1 - PHASE_DEGRADED_DROP` for `PHASE_PERSIST_SAMPLES` consecutive ticks
  (debounced); recovers back to `steady` on the mirrored recovery threshold, re-baselining to the
  recovery-point IPC rather than trusting the pre-degradation baseline. Only active with `--interval` +
  `COUNTER_IPC` + not `--per-core`.
- **resource_dominance** (`wspy-archetype`) — `[categorical]` ranked top-2 pick among
  `retire_pct`/`frontend_pct`/`backend_pct`/`speculate_pct` → compute-bound / frontend-bound /
  memory-bound / speculation-bound, with a confidence label (`high`/`medium`/`low`) from the margin
  between the top two.
- **parallelism_shape** (`wspy-archetype`) — `[categorical]` threshold on `parallelism_proxy` (see above).
- **control_flow_style** (`wspy-archetype`) — `[categorical]` threshold on `branch_mispredict_pct` (see
  above).
- **runtime_stability** (`wspy-archetype`) — `[categorical]` threshold on `phase_stability` (see above).
- **memory_attribution** (`wspy-archetype`) — `[categorical]` cross-references `backend_pct` (topdown)
  against every independently-measured cache/TLB/IBS signal the run collected (`dcache_miss_pct`,
  `l2_miss_pct`, `l3_miss_pct`, `itlb_miss_per1k`/`dtlb_miss_per1k`, `itlb_generic_miss_pct`/
  `dtlb_generic_miss_pct`, `ibs_dc_miss_pct`, `ibs_dram_pct`, `smt_contention_pct`), further modified by
  `blocking_wait_pct`/`sched_rundelay_pct` when the run used `--tree` with the relevant sub-flags.
  Checked in priority order: `blocked` (`blocking_wait_pct` at/above 20% — heavy futex/io-wait, the CPU
  was waiting on the kernel, not stalled) beats `oversubscribed` (`sched_rundelay_pct` at/above 20% with
  low blocking-wait — runnable but not given the CPU, an affinity/placement problem) beats
  `not-memory-bound` (backend_pct below a 20% floor, nothing to explain) beats `corroborated` (backend
  significant and at least one cache/TLB/IBS signal is also notably elevated — `memory_attribution_reasons`
  lists which) beats `uncorroborated` (backend significant but every signal that *was* collected looks
  unremarkable — a genuine attribution gap worth a closer look, not a measurement failure), or `unknown`
  (backend_pct itself wasn't measured, or none of the corroborating signals were even collected this
  run). `blocked`/`oversubscribed` are checked *before* cache/TLB/IBS corroboration specifically because
  a "memory-bound" topdown read on a run that wasn't actually stalled makes asking whether other
  counters "corroborate" it moot regardless of what they show. Deliberately doesn't itself rank *which*
  cache level a stall concentrates in — that's a different, stall-cycle-attribution question only
  `--topdown-backend`'s own dedicated group (`l1_bound_slots_pct`/etc. above) can honestly answer, and
  that group is Intel/ARM-only besides; `memory_attribution_locus` below is the request-outcome-based
  answer for the AMD/IBS side of that same question instead.
- **memory_attribution_locus** (`wspy-archetype`) — `[categorical]` which hop in the memory hierarchy a
  `corroborated` `memory_attribution` read concentrates in: `l1` / `l2-l3` / `dram` / `remote-numa`, plus
  a `tlb-cofire` tag in `memory_attribution_locus_reasons` when `ibs_dc_l1tlb_miss_pct`/
  `ibs_dc_l2tlb_miss_pct` (or, in the cache-counter fallback tier, `dtlb_generic_miss_pct`) are also
  elevated — address translation is an independent hop from the data-miss chain, so it's a co-firing tag,
  not a competing label. `unknown` (with no reasons) unless `memory_attribution` is exactly
  `"corroborated"` and `backend_memory_pct` (not the coarser `backend_pct`) clears the same 20% floor
  `memory_attribution` itself uses — ranking hops on an uncorroborated/blocked/oversubscribed/
  not-memory-bound read, or on a run whose backend stall wasn't actually memory-specific, would claim
  precision this signal set doesn't have. Two precision tiers, always recorded first in
  `memory_attribution_locus_reasons` (`tier=ibs-sample` / `tier=cache-counter`) so cache-counter-derived
  precision is never mistaken for IBS-grade:
  1. **IBS tier** (preferred, needs `ibs_dc_miss_pct`): `ibs_remote_node_pct` at/above 10% wins first (a
     cross-socket hop dominates regardless of where else the access resolved) → `remote-numa`; else
     `ibs_dram_pct` at/above 10% → `dram`; else, if `ibs_dram_pct` was itself measured, the residual
     `ibs_dc_miss_pct − ibs_dram_pct` at/above 10% → `l2-l3` (missed L1D, resolved before DRAM); else a
     plain `ibs_dc_miss_pct` at/above 10% → `l1`. Falls through to tier 2 if IBS data exists but nothing
     in it clears its threshold — `memory_attribution`'s own corroboration may have come from a signal
     (e.g. `smt_contention_pct`) with no hierarchy-position information at all.
  2. **Cache-counter tier** (any vendor, no IBS needed): same shape over `l3_miss_pct` (→ `dram`) /
     `l2_miss_pct` (→ `l2-l3`) / `dcache_miss_pct` (→ `l1`), each at/above the same 10% cutoff
     (`CACHE_MISS_ELEVATED_PCT`) `memory_attribution`'s own signal table already uses for these features —
     not a new threshold. Coarser than the IBS tier: a miss *rate* per cache level, not per-sample
     hierarchy-position tag data.

  See `doc/INVESTIGATION_ARCHIVE.md`'s "IBS-derived memory-path bottleneck decomposition" write-up
  for the full design rationale.
- **memory_attribution_locus_reasons** (`wspy-archetype`) — `[human-only]`/`[categorical]` (not a
  `run_features` row, only ever printed alongside `memory_attribution_locus` in `wspy-archetype`'s own
  CSV/human/`--run` output) — comma-joined trace of which precision tier and signal(s) produced the
  `memory_attribution_locus` label, in the same `name=value` style as `memory_attribution_reasons`.
- **distance** (`wspy-archetype --nearest`/`--kmeans`) — `[human-only]`/`[categorical]` (not a
  `run_features` row, only ever printed by `--nearest`'s neighbor ranking or `--kmeans`'s
  distance-to-centroid ordering) — a coverage-aware, z-score-standardized RMS distance between two runs'
  feature vectors, computed only over the `run_features` dimensions both runs actually measured (a pair
  sharing 6 features isn't penalized for sharing less than a pair sharing 18 — see `compared_features`
  below). Lower = more similar; not bounded to a fixed range, so only meaningful relative to other
  distances printed alongside it in the same ranking, not as an absolute score.
- **compared_features** (`wspy-archetype --nearest`) — `[human-only]`/`[categorical]` count of
  `run_features` dimensions both runs in a `--nearest` row actually measured (`coverage='measured'` on
  both sides) — the coverage-transparency companion to `distance` above, so a low distance computed over
  only 2 shared features can be told apart from one computed over 15.
- **blocking_wait_pct** (`wspy-archetype` input, `store.c`) — `[feature]` `(futex_wait_seconds +
  io_wait_seconds) / elapsed_seconds * 100`, scanned directly from `--tree`'s raw `futex`/`io_wait` lines
  (needs `--tree-futex`/`--tree-io-wait`; `unavailable` without `--tree` at all, the common case).
- **sched_rundelay_pct** (`wspy-archetype` input, `store.c`) — `[feature]` `rundelay_seconds /
  elapsed_seconds * 100`, scanned from `--tree`'s raw `schedstat` lines (needs `--tree-schedstat`).
- **env_score** (`wspy-summary`) — `[human-only]`/`[categorical]` (not a `run_features` row, printed as
  a column on every `wspy-summary`/`--check-regression` bucket row) — unweighted fraction of 8 tracked
  `run_environment` fields (`virt_role`, `hypervisor_vendor`, `microcode_version`, `bios_vendor`/
  `bios_version`/`bios_date`, `cpu_governor`, `memory_total_kb`) that agreed across a bucket's
  contributing runs; `memory_total_kb` counts as agreeing within 5% (routine firmware/DIMM jitter),
  every other field is exact match, and a field only counts once mutually comparable across every
  contributing run (`hypervisor_vendor` self-excludes on bare-metal runs). A bucket with zero
  comparable fields gets an explicit no-data sentinel rather than a fabricated 0%/100% — absence of
  provenance data is not evidence of a mismatch. `--min-env-score` (default 0.8) is the threshold below
  which the bucket's **mixed-env** verdict reason fires, the scored extension of 4.2's exact-match
  `mixed-pmu` check onto `run_environment`'s fuller provenance surface.
- **drift_pct** (`wspy-summary --phase-topdown`) — `[human-only]`/`[categorical]` (not a `run_features`
  row, printed per topdown column in `--phase-topdown`'s output) — the largest phase-to-phase swing in
  that column's mean across the run's observed `warmup`/`steady`/`degraded` phases; blank (never
  fabricated) for a column with data in only one observed phase. The trailing note names the single
  largest drifter across every topdown column in the run.

Source: `provenance.c`, one-shot per-run capture, JSON in the manifest, promoted into SQLite by
`store.c:enrich_from_manifest()`. Every field degrades independently (`available=0` + a reason string)
rather than failing the run. All `[environment]`.

- **virt_role** — "guest"/"host", CPUID leaf 1 ECX bit 31 (x86_64) or `/proc/cpuinfo`'s `hypervisor` flag.
- **hypervisor_vendor** — CPUID leaf `0x40000000` vendor string (x86_64 guest only).
- **microcode_version** — `/proc/cpuinfo`'s `microcode` line, verbatim.
- **bios_vendor**, **bios_version**, **bios_date** — `/sys/class/dmi/id/bios_*`, verbatim.
- **cpu_governor**, **cpu_scaling_driver** — `/sys/devices/system/cpu/cpu0/cpufreq/scaling_{governor,driver}`.
- **cpu_governor_uniform** — bool, false if any online CPU's governor differs from cpu0's — worth
  checking before trusting `freq`/topdown ratios as steady-state if false.
- **memory_total_kb** — `sysinfo().totalram * mem_unit / 1024`.
- **compiler_version**, **libc_version** — build-time `__VERSION__` / `gnu_get_libc_version()`.

## Cgroup (manifest-only today — not yet in the store)

Source: `cgroup.c`, JSON in the manifest via `manifest.c`. **No `store.c` table ingests these yet** — a
concrete candidate for a future normalized table, parallel to `run_environment`. All `[manifest-only]`.

- **path** — cgroup v2 unified-hierarchy path (`/proc/self/cgroup`'s `0::<path>` line).
- **quota_us**, **period_us** (from `cpu.max`) — CPU bandwidth limit; quota `-1` means unlimited.
- **cpu_weight** (from `cpu.weight`) — relative CPU scheduling weight.
- **memory_max_bytes** (from `memory.max`) — hard memory limit; `-1` means unlimited.
- **memory_high_bytes** (from `memory.high`) — soft memory throttling threshold; `-1` means unlimited.
- **nr_periods**, **nr_throttled**, **throttled_usec** (from `cpu.stat`) — reported as a **delta** across
  the run (`end - start` baseline), i.e. this run's own throttling impact, not the cgroup's lifetime
  cumulative counters. A nonzero `nr_throttled`/`throttled_usec` delta means the run was CPU-throttled by
  its cgroup limit during the measurement window — worth checking before attributing a low `ipc`/high
  `iowait` to the workload itself.

## Known gaps / candidates for this list to grow into

- `on_cpu` and topdown's `GHz` annotation are useful but currently `[human-only]` — never reach the
  store. Promoting either to a real CSV column would need a `PRINT_CSV_HEADER`/`PRINT_CSV` case added to
  their respective `print_*` functions (see `CLAUDE.md`'s CSV-column pitfalls before doing so).
  `on_cpu` in particular seems like an obvious `run_features` candidate once it has a CSV home.
  Per-1000-inst density annotations (branches, cache accesses, etc.) are the same story.
- `float` (AMD FP-op density) is now `float_pct` in `SIMPLE_METRIC_FEATURES` (issue #227). The
  reference-matrix corpus (SPEC CPU2026 by-machine pages) showed a clean intrate-vs-fprate split with no
  overlap, but only on one machine at n=1 per test — a `SIMPLE_AXES` `vectorization_density` archetype
  axis with real thresholds is still waiting on more machines/compilers/repeat runs before committing to
  boundaries, per `archetype.c`'s own extension note.
- Cgroup fields have no store table at all yet (see above) — `nr_throttled`/`throttled_usec` in
  particular seems worth a `run_features` entry (e.g. `throttled_pct = throttled_usec / (elapsed*1e6)`)
  since it directly explains anomalous runtime_stability/backend_pct results.
- IBS and GPU metrics are `[raw]` (reach `metric_values` on any host that emits them) but have zero
  `run_features` today — low priority unless/until a GPU- or IBS-focused archetype axis is wanted.
