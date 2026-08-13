# wspy

wspy - a workload spy

wspy is an instrumentation wrapper: it launches a child command, lets it run, and reports
runtime + hardware performance-counter + system metrics gathered while it ran, plus a set of
tools around it for turning many such runs into a queryable, publishable dataset (normalized
storage, summary statistics, shared plots, a job queue, and a web launcher/report browser).
It's the author's internal testbed for workload-characterization experiments, published to
make it easy to pull onto different machines; listed as public in case it's otherwise useful.
Run `wspy --version` for the exact version of a given checkout.

## Building

```
make                          # builds wspy, cpu_info, proctree, wspy-validate, wspy-ledger,
                               # wspy-store, wspy-summary, wspy-plot, wspy-core-report,
                               # wspy-archetype (no GPU support)
make AMDGPU=1                 # also builds amd_smi, amd_sysfs (needs ROCm; auto-detects /opt/rocm or /usr)
make AMDGPU=1 ROCM_DIR=<path> # point at a non-default ROCm install
make NVIDIA=1                 # also builds nvidia_nvml for --gpu-nvidia (dlopen()s the driver's
                               # libnvidia-ml.so.1 at run time -- no CUDA toolkit needed to build)
make AMDGPU=1 NVIDIA=1        # both GPU axes at once (e.g. an AMD iGPU + NVIDIA dGPU laptop)
make test                     # build and run the unit tests
./run_tests.sh                # build + run unit tests + integration smoke tests
./tests/arm_topdown_microbench.sh # ARM-only topdown-equivalent sanity check (skips elsewhere)
make clean                    # remove object files
make clobber                  # also remove built binaries
```

`wspy-store`/`wspy-summary`/`wspy-archetype` link against the system SQLite library — install
`libsqlite3-dev` (or your distro's equivalent) before building. `wspy-plot` shells out to a `gnuplot`
binary at run time, not a build-time dependency. `wspy-run` is a bash script; `wspy-sweep`,
`wspy-queue`, `wspy-bundle`, `wspy-analyze`, `wspy-symbolize`, `wspy-publish`, `wspy-testpoint`,
`wspy-phoronix-import`, and the web launcher (`web/server.py`) are plain Python 3 scripts — stdlib
only, nothing to build or install (`wspy-analyze` additionally needs a running Ollama daemon at use
time, not build time, to do anything; `wspy-symbolize` needs `addr2line` on `PATH` (binutils,
near-universally already installed) at use time to resolve anything, though it degrades to an
"unresolved" report rather than failing outright if it's missing; `wspy-phoronix-import`'s `--result`
source needs `phoronix-test-suite` installed; `wspy-publish`/`wspy-testpoint` need network access to
the configured git remote to clone the report-root the first time, `wspy-publish` additionally to the
configured WordPress site).

Performance counters and `--tree` (which uses `ptrace`) generally need root, or
`CAP_SYS_PTRACE` plus `perf_event_paranoid <= 1`. `scripts/setup_perf.sh` checks and, if you
confirm, adjusts the `nmi_watchdog` and `perf_event_paranoid` sysctls for the current session.
`--power` needs root or `CAP_PERFMON` specifically (RAPL/`energy-pkg` access is stricter than
`perf_event_paranoid` alone covers) — the same script also checks/grants that on the `wspy`
binary (`sudo setcap cap_perfmon+ep`); re-run it after rebuilding, since the grant is tied to
that exact binary file. `./cpu_info` and `wspy --capabilities`/`--preflight` need no privileges
at all.

## Usage

```
wspy [options] -- <command> [args...]
```

Everything after `--` (or the first non-option argument) is treated as the child command to
launch and instrument. Run `wspy --help` (or `-h`) for the full, current option list; invalid or
missing arguments instead print a short usage line pointing at `--help`.

Some of the more commonly used options:

* `--version` - print the wspy version and exit
* `--capabilities` - probe available counters for this host/kernel and exit (no workload needed)
* `--preflight [<counter flags>]` - check whether the given counter flags will fit in the
  available hardware PMU counter slots without multiplexing, and exit; suggests which flags to
  drop (or, if it's the cause, stopping the NMI watchdog) when they won't. No root needed, and no
  workload command either -- this is also run automatically (silently, unless something doesn't
  fit) before every real run.

* Output
  * `-o <file>` - send output to a file instead of stdout
  * `--csv` - CSV output instead of human-readable
  * `--interval <sec>` - print a snapshot every `<sec>` seconds while the child runs
  * `--no-phase-detect` - disable automatic warmup/steady/degraded phase detection on
    `--interval` samples (a `phase` CSV column + boundary summary are on by default)
  * `--verbose` / `-v` - verbose diagnostics (repeat for more detail)
* Run artifacts (reproducibility metadata, independent of each other)
  * `--manifest <file>` - write a JSON run manifest (command line, timestamps, exit status,
    host/CPU info, counter coverage, provenance, cgroup v2 identity/limits/throttling deltas,
    output files produced)
  * `--run-index <file>` - append a compact JSON Lines record for this run to a shared file,
    so tooling can query "all runs" by scanning one file
  * **`<file>`'s parent directory must already exist for both flags** - wspy does not create it.
    A missing directory is a `warning: unable to open manifest file: ...`/`unable to open run
    index file: ...` on stderr, not a hard failure: the workload still runs, counters are still
    measured, wspy still exits 0, and only that one artifact is silently dropped. This surfaces
    two steps downstream as `wspy-store` reporting `0 record(s)` ingested or `wspy-archetype
    --run <host:id>` reporting "no run found," with no mention of the real cause — worth checking
    first if either of those happens unexpectedly in a scripted pipeline.
  * `--exit-with-child` - exit with the launched command's own exit status (or 128+signal),
    instead of wspy's default of always exiting 0 regardless of the child
  * `--preset-name <name>`, `--config-name <name>`, `--config-option <k>=<v>` - metadata-only;
    record which named preset/configuration/option a front end (`wspy-run`, the web launcher)
    chose to run, so a manifest/run-index record can say "this was `deep-cpu`" instead of a
    report having to re-derive it from raw flags. `wspy` itself never reads these back.
* Multi-pass counter execution
  * `--passes=<groups>` - re-run the workload once per automatically-sized pass (bin-packed to
    fit the hardware PMU counter budget) and merge the result into one CSV/manifest row, instead
    of requiring N separate `wspy` invocations to sweep more counter groups than fit at once.
    Aggregate-only: incompatible with `--interval`/`--per-core`/`--tree`/AMD IBS/GPU flags.
  * `--multiplex` - with `--passes`, use one pass covering every requested group instead of
    bin-packing, trading precision (heavier kernel multiplexing) for a single re-execution
* Process info
  * `--rusage` / `-r` - report `getrusage(2)` info (on by default)
  * `--per-core` / `-a` - report performance counters per-core instead of system-wide
  * `--per-core-freq` - with `--per-core`, add each core's live cpufreq reading (`core_freq_mhz`)
* Core/thread affinity
  * `--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|cpuset=<c0,c1,...>` - pin the
    workload to a subset of cores/threads (SMT off, one L3-domain or core-type, or an explicit
    list) before it launches; recorded into the manifest/run-index either way
  * `--list-affinity` - print discovered SMT/L3-domain/core-type topology and exit (no root needed,
    no workload command either); also folded into `--capabilities`
* Process tree
  * `--tree <file>` - trace the child (and its descendants) via `ptrace`, recording
    fork/exec/exit events with timestamps to `<file>`
  * `--tree-cmdline` - also record each process's full command line
  * `--tree-vmsize` - also record peak RSS plus anon/file/shmem RSS composition and swap
  * `--tree-futex`, `--tree-io`, `--tree-io-wait`, `--tree-schedstat`, `--tree-connect`,
    `--tree-nanosleep`, `--tree-wait`, `--tree-poll` - per-process synchronization-latency
    instrumentation (blocking futex/I/O/connect/wait/poll/sleep time, `/proc/<pid>/io` byte
    counters, and `/proc/<pid>/schedstat` run-queue delay) — together they explain a degraded
    interval phase as blocked in the kernel, runnable but not scheduled, or a genuine on-CPU stall
  * `proctree <file>` - the companion tool that reads a `--tree` file back and reconstructs
    the process hierarchy; `proctree --json <file>` emits the same tree as one JSON document, and
    `proctree --diff [--json] <a.json> <b.json>` diffs two runs' trees structurally
    (added/removed/changed/same per node) — both also drive the web launcher's interactive tree
    viewer and tree-diff pages
  * `--target=comm=<name>[,cmdline=<substr>]` (requires `--tree`) - once a `--tree` run's hot-process
    table shows which comm dominates the run's time, attach a dedicated counter group (the same
    counters this run's other flags requested) to just the matching process(es) instead of only
    ever reading the whole-subtree aggregate — every process whose comm/cmdline matches gets its
    own attach, discovered live as it execs. Surfaces as a `target_counters` array on both the
    per-process and per-comm-rollup entries in `proctree --json`'s output
  * `--symbol-sample` / `--symbol-sample-event=<event>` (requires `--target`) - attach a
    `PERF_SAMPLE_IP` sampling counter (default event `cycles`; `event` ∈ `cycles`/`instructions`/
    `cache-misses`/`branch-misses`) to each `--target`-matched process and record its raw sampled
    instruction pointers as `target_samples`/`target_maps` in `proctree --json`'s output — resolve
    them to routine/symbol names with `wspy-symbolize` (below)
* System-wide metrics
  * `--system` / `-s` - report load average, CPU time (`/proc/stat`), network (`/proc/net/dev`),
    per-block-device disk I/O (`/sys/block/<dev>/stat`), and memory pressure (`/proc/meminfo`)
    counters, plus GPU metrics if a `--gpu-*` option is also given
* Performance counters (`--ipc` and `--software` are on by default; `--no-ipc`/`--no-software`
  to turn either off)
  * `--counters=<list>` - **the recommended way to select counter groups**: a comma-separated
    list of group names, e.g. `--counters=topdown,cache2`. Additive, same as every flag below.
    Valid names: `ipc`, `topdown`, `topdown2`, `topdown-frontend`, `topdown-backend`,
    `topdown-optlb`, `branch`, `dcache`, `icache`, `cache1` (currently a no-op), `cache2`,
    `cache3`, `tlb`, `memory`, `opcache`, `float`, `software`
  * Individual `--<name>` flags (and their short forms below) still work identically for each
    group name above, but are deprecated in favor of `--counters=<name>` -- each now warns once
    used; silence with `--no-deprecation-warnings` or `WSPY_NO_DEPRECATION_WARNINGS=1`
    * `--ipc` / `-i` - instructions-per-cycle
    * `--topdown` / `-t`, `--topdown2`, `--topdown-frontend`, `--topdown-backend`, `--topdown-optlb`
      - Intel/AMD topdown methodology counters at various levels; on ARM, `--topdown`
        reports a topdown-equivalent decomposition from ARM PMU raw events
    * `--branch` / `-b`, `--dcache`, `--icache`, `--cache1`, `--cache2` / `-c`, `--cache3`,
      `--tlb`, `--memory`, `--opcache`, `--float` - individual hardware counter groups
    * `--software` - software counters (page faults, context switches, ...)
  * `--power` - CPU package energy/power (`pkg_joules`/`pkg_watts`) via the `power`/`energy-pkg`
    perf PMU (RAPL-equivalent); with `--per-core`, also per-core energy (`core_joules`/
    `core_watts`) on the representative logical CPUs the `power_core` PMU exposes. Needs root or
    `CAP_PERFMON` specifically (see `scripts/setup_perf.sh` above)

ARM notes:

* On ARM hosts with multiple PMU device types (for example, big.LITTLE clusters exposing
  different `armv8_pmuv3_*` devices), `wspy --capabilities` prints an ARM PMU topology report
  with cluster-to-CPU mapping.
* Per-core mode (`--per-core`) binds each core's raw events to that core's PMU type, so mixed-PMU
  systems can collect ARM PMU groups without cross-cluster type mismatches.
* Process-wide (`--topdown` without `--per-core`) ARM raw counters can be sensitive to task
  migration across PMU clusters; for cluster-specific runs, use `--per-core` or pin affinity.
* AMD IBS (Instruction-Based Sampling; AMD-only, system-wide)
  * `--ibs-basic` - unfiltered `ibs_fetch`/`ibs_op` sample counts
  * `--ibs-memory-deep` - `ibs_op` with L3-miss-only + load-latency filtering for memory-path
    analysis (skews the effective sampling period — see the run's own output for the
    skew/quality annotations)
  * `--ibs-maxcnt <n>`, `--ibs-ldlat <n>`, `--ibs-fetchlat <n>` - override the built-in
    default sampling/filter thresholds
  * `--ibs-sample` - sampling-mode IBS: decodes each individual sample's tagged register data
    (dc/ic/TLB miss, branch-mispredict, DRAM-fill, and remote-NUMA-fill rates) instead of just a
    running count, via a mmap'd perf ring buffer. Rates are computed once at end-of-run, not
    per-`--interval` tick — see `ibs_sample.h`. The full named memory-data-source breakdown
    (Local L3/CCX, DRAM, remote-NUMA CCX, long-latency DIMM, MMIO, ...) prints in the
    human-readable output only, since the category meanings differ between pre-Zen4 and Zen4+
    hardware — only the two scheme-independent rates above reach CSV
* AMD GPU metrics (only available when built with `AMDGPU=1`)
  * `--gpu-smi` - GPU info via ROCm's `amd_smi` library
  * `--gpu-busy` - instantaneous GPU busy percent, read from sysfs
  * `--gpu-metrics` - detailed GPU metrics (temperature, activity, power, clock) fused from sysfs
    (the primary source) and ROCm SMI (fills in temperature/activity if sysfs's reading failed,
    and is the sole VRAM source); `gpu_temp_source`/`gpu_activity_source` columns record which
    backend actually supplied each value
  * `--gpu-device=<idx>` - select a specific AMD GPU device by index for the above, on
    multi-GPU hosts (default: lowest-numbered card / SMI device 0); see `--capabilities`
    for the enumerated device list
* NVIDIA GPU metrics (only available when built with `NVIDIA=1`)
  * `--gpu-nvidia` - busy percent + VRAM used/total via NVML (`nv_`-prefixed CSV columns, so they
    coexist with AMD's `gpu_*` columns when both are active in the same run)
  * `--gpu-nvidia-device=<idx>` - select a specific NVIDIA device by index on multi-GPU hosts
    (default: device 0); see `--capabilities` for the enumerated device list

Examples:

```
sudo ./wspy -- sleep 1                      # default IPC counters around `sleep 1`
sudo ./wspy --csv --counters=topdown -- myapp arg1   # CSV output with topdown counters
sudo ./wspy --tree tree.out -- myapp        # record the process tree while myapp runs
./proctree tree.out                         # display the tree recorded above
sudo ./wspy --system --gpu-busy -- myapp    # system + GPU metrics (needs AMDGPU=1 build)
sudo ./wspy --manifest run.json --run-index index.jsonl -- myapp
                                             # write reproducibility metadata alongside output
```

## wspy-run: profile-driven launcher

`wspy-run` is a wrapper script that runs a sequence of predefined `wspy` passes ("a profile")
against one workload command, so you don't have to hand-write the same multi-invocation `wspy`
command lines every time (see `workload/*/run_test.sh` for what that used to look like).

```
./wspy-run --list                                        # show builtin profiles and their passes
./wspy-run -o results/coremark quick -- phoronix-test-suite batch-run coremark
./wspy-run -o results/503.bwaves --run-index results/index.jsonl deep-cpu -- \
    runcpu --config mev-aocc.cfg --action=validate --tune base 503.bwaves_r
./wspy-run --dry-run -c my-passes.conf -- sleep 1        # preview a custom pass list

# Unified output layout: one directory per run (<outdir>/<suite>/<benchmark>/<run-id>/),
# with a run-level manifest.json, summary.txt, and reserved plots/ dir alongside each pass's
# own output. Composing two builtin profiles with a comma runs both in the same run directory.
./wspy-run --suite phoronix --benchmark coremark -o results --run-index results/phoronix/index.jsonl \
    deep-cpu,tree-heavy -- phoronix-test-suite batch-run coremark
```

Builtin profiles: `quick` (one fast IPC+system pass), `deep-cpu` (the multi-pass AMD counter sweep
used for topdown characterization), `deep-cpu-intel` (the shorter Intel-only equivalent — Intel
lacks `deep-cpu`'s AMD-specific opcache/frontend/L3 groups), `deep-gpu` (`deep-cpu` plus GPU
busy/metrics passes), `tree-heavy` (a single `--tree` pass with full command-line capture, capped
at a run-time-estimated timeout, 3600s floor), `ibs-basic`/`ibs-memory-deep`/`ibs-sample`
(single-pass AMD IBS collection — counting mode for the first two, sampling mode with a named
cache/DRAM breakdown for the third, see the IBS flags above), `gpu-compute` (tree tracing + system
+ power + both GPU backends + topdown on one shared `--interval` timeline, for GPU-bound workloads a
separate-pass-per-category profile can't correlate tick-for-tick), and the Zen-family preset packs
`zen-portable` (`quick`+`ibs-basic`, warning-free across the whole Zen family) and `zen4plus-deep`
(`deep-cpu`+`ibs-sample`+`tree-heavy`, assumes Family 19h+). Builtin profiles are data, not code —
`quick`/`deep-cpu`/etc. are `profiles/*.conf` files (the exact `<pass-name> <wspy-flags...>` grammar
below), and `zen-portable`/`zen4plus-deep` are `profiles/*.spec` composites; adding a profile means
adding a file, not editing this script. `-c <file>` loads a custom pass list instead
(`<pass-name> <wspy-flags...>` per line), and
a comma-separated profile list (e.g. `deep-cpu,tree-heavy`) composes more than one builtin
profile's passes into a single invocation — see `./wspy-run --help` for the full option list and
config-file format. Each pass writes to `<outdir>/<prefix><pass-name>.<csv|txt>` by default, or
into the unified per-run directory layout above when `--suite`/`--benchmark` are given (see
`doc/ARTIFACT_CONTRACT.md`'s "Unified output layout" section for the full directory contents);
`--manifest-dir`/`--run-index` pass those `wspy` flags through per pass either way. `--affinity
<spec>` and `--config-option <k>=<v>` (repeatable) both pass through unchanged to every pass's own
`wspy` invocation — the latter is metadata only (no effect on what a pass does), useful for tagging
a run with context `wspy-summary --group-by-option` can later query on.

`--run-id <id>` names the run *directory* (`<outdir>/<suite>/<benchmark>/<run-id>/`) — it is **not**
the same identifier as any individual pass's own `run_id` inside its `--run-index` record; each
`wspy` process invoked by a pass computes its own independently. If you need to look a specific
pass up later (e.g. `wspy-archetype --run <hostname>:<run_id>`), read that pass's actual `run_id`
back off the `--run-index` file after the run, rather than assuming it matches whatever you passed
to `--run-id` here — see `doc/ARTIFACT_CONTRACT.md`'s "Unified output layout" section for the full
distinction.

## wspy-sweep: comparison matrix mode

`wspy-sweep` cross-products one wspy-controllable axis (`--affinity`, covering SMT on/off,
L3-domain placement, and core-type comparisons — see `wspy --list-affinity` for real ids on your
host) against one or more workload commands, running `wspy-run` once per resulting cell and
tagging each with `--config-option` so the results are directly comparable afterward via
`wspy-summary --group-by-option`. Deliberately doesn't try to sweep compiler/kernel/governor —
those go in as a uniform `--tag`/spec `tags` value instead (recorded, never automated); see
`INVESTIGATION.md`'s "Comparison matrix mode deep-dive" for why.

```
# Quick form: one workload, one axis
./wspy-sweep --affinity all,nosmt --profile deep-cpu -- phoronix-test-suite batch-run coremark

# Declarative spec: multiple workloads, uniform context tags
./wspy-sweep --spec sweep.json
```

```json
{
  "suite": "sweep-smt-coremark",
  "workloads": [{"name": "coremark", "command": ["phoronix-test-suite", "batch-run", "coremark"]}],
  "axes": {"affinity": ["all", "nosmt"]},
  "profile": "deep-cpu",
  "tags": {"compiler": "gcc13", "kernel": "6.12.0"}
}
```

`--dry-run` prints every cell's command line without running any of them. After the sweep,
best-effort ingests into the normalized store and prints (doesn't run) the `wspy-summary
--group-by-option` command that shows the comparison.

## wspy-validate: pre-publish quality checks

`wspy-validate` runs basic sanity checks against one or more `--manifest`-produced manifest files,
so a bad run (permission-denied counters, a truncated CSV, a nonzero exit) is caught before it's
published rather than after. Each check is independent, so one failure doesn't hide the others:
manifest schema version recognized, every listed output file present and non-empty, CSV column
counts match the header on every row, workload exit status, counter coverage (partial coverage is
a warning, not a failure — that's `coverage.c`'s designed-in graceful degradation), and a sanity
range on numeric CSV columns (looser for `%`-suffixed multi-core aggregate columns).

```
./wspy-validate run.manifest.json                 # human report, [PASS]/[WARN]/[FAIL] per check
./wspy-validate --strict *.manifest.json           # any WARN also fails the exit status
./wspy-validate -q *.manifest.json                 # only print manifests with a FAIL
```

Exits 0 only if every manifest had zero failures (`--strict` also fails on any warning). See
`./wspy-validate --help` for the full option list.

## wspy-ledger: coverage ledger

`wspy-ledger` generates a "coverage ledger" for a suite of workloads (e.g. a SPEC CPU2017 or
Phoronix benchmark list) directly from a shared `--run-index` file, replacing the kind of
hand-maintained "what's still missing" tracking that `workload/phoronix/phoronix.tests.txt`
does today. Give it a workload list (one name per line) and one or more run-index files, and it
reports each workload's status:

* `done` - at least one matching run in the index exited cleanly
* `skipped` - no matching run found at all
* `needs-tool-support` - matching run(s) exist but none succeeded, or the workload is annotated
  as such in the workload list
* `unsupported` - annotated as such in the workload list (e.g. a workload known not to build,
  or that needs a GPU-enabled wspy build unavailable here)

A workload's name is matched as a substring against each run-index record's command line.

`--phoronix-option-combos` reports each matching workload's full Phoronix option-matrix size
upfront (the product of every `<TestSettings>/<Option>`'s `<Menu>` entry count in its
`test-definition.xml`) as an `option_combinations` column/bracketed suffix, so a large option
space is visible before a long batch-run sweep discovers it partway through —
`combo_has_freeform`/`combo_ambiguous` flag a lower-bound or disagreeing count rather than
silently guessing. Scans `--phoronix-profiles-dir` (default `$HOME/.phoronix-test-suite/
test-profiles`), the same directory `--unavailable-deps` reads; both may be given together.

```
./wspy-ledger --run-index results/index.jsonl workloads.txt
./wspy-ledger --run-index results/index.jsonl --csv workloads.txt   # machine-readable
./wspy-ledger --run-index results/index.jsonl --strict workloads.txt  # nonzero exit if
                                                                        # anything's outstanding
./wspy-ledger --run-index results/index.jsonl --phoronix-option-combos workloads.txt
                                                                        # option-matrix size per workload
```

`workloads.txt` is one workload name per line; append a tab-separated `unsupported` or
`needs-tool-support` plus a free-text note to override inference for a specific workload. See
`./wspy-ledger --help` for the full option list.

## wspy-store: normalized SQLite store

`wspy-store` ingests one or more `--run-index` (JSONL) files into a SQLite database: a `runs`
catalog table plus a long/tall `metric_values` table parsed from each run's CSV output (covers
aggregate, `--interval`, and `--per-core` shapes uniformly, since column identity comes from the
CSV header, not which flags produced it). This is what makes "all my runs" queryable instead of a
pile of separate output files. Ingestion is idempotent — re-running against the same or a grown
run-index file upserts rather than duplicating rows — and best-effort enriches each run from its
manifest (host fields) and CSV (metric values) when those files are still readable.

```
./wspy-store --db results/store.db --run-index results/index.jsonl
./wspy-store --db results/store.db --run-index results/index.jsonl --strict   # nonzero exit on
                                                                                # any malformed/
                                                                                # collided record
```

**`--csv` on the original `wspy` run is required for anything downstream that reads
`metric_values`/`run_features`** (`wspy-archetype`, `wspy-summary`) — not just a nice-to-have output
format choice. A pass run without `--csv` still gets its manifest enriched, but `wspy-store`'s
summary line reports `0 metric-set(s) ingested, 1 skipped` (easy to miss in a scripted pipeline)
and every `run_features` row lands `coverage=unavailable`, so `wspy-archetype` then correctly but
silently reports `resource_dominance=unknown` even though the counters were genuinely measured and
printed in the run's human-readable output.

See `./wspy-store --help` for the full option list.

## wspy-summary: regenerable summary tables

`wspy-summary` queries a `wspy-store` database directly and computes min/max/mean/median/stddev/CV,
a 95% confidence interval of the mean, a repeatability verdict (`PASS`, or `WARN:` plus any
combination of `thin` — too few runs, `noisy` — too much spread, `mixed-pmu` — contributing runs
differ in CPU vendor or requested/measured counter coverage, `mixed-env` — see `env_score` below),
and z-score outlier flags per `(group,metric)` bucket — grouped by workload command (default),
hostname, CPU vendor, `affinity_mode`/`preset_name`/`config_name`, or `cpu_governor`/`virt_role` —
so a summary table can always be regenerated from indexed data with no manual copy/paste.
`--group-by-option <name>` composes a *second* grouping axis from an arbitrary `--config-option`
key (e.g. a `wspy-sweep` cell's axis tag), for "this workload, broken out by X" comparisons.
`--strict` fails if any bucket is too thin (`--min-runs`), too noisy (`--max-cv`, default 5%), or
nothing matched.

`--run-id <hostname>:<run_id>` (repeatable) narrows to an *exact* run set rather than a text/exact-
column match — needed because two runs can share byte-identical `command`/`hostname` (a redo
superseding an earlier bad run) and still need to land in separate buckets, which `--command`/
`--hostname` alone can never express. `wspy-testpoint aggregate` (below) is the intended caller —
it resolves a curated `stats-pool` run set first, then passes it through as `--run-id` flags.

`env_score` is the fraction of 8 tracked machine-environment fields (`virt_role`, `hypervisor_vendor`,
`microcode_version`, `bios_vendor`/`bios_version`/`bios_date`, `cpu_governor`, `memory_total_kb` —
`memory_total_kb` within 5% counts as agreeing, to tolerate routine firmware/DIMM-population jitter)
that agreed across a bucket's contributing runs, unweighted (no per-field point values — the same
reasoning that keeps `--ibs-sample`'s cross-scheme memory-source categories out of its own CSV
schema), or empty when none of them were ever mutually comparable — never treated as a mismatch,
absence of provenance data is not evidence of one. `--min-env-score <fraction>` (default 0.8) is the
threshold below which a bucket's verdict carries `mixed-env`.

`--check-regression <hostname>:<run_id>` is a standalone mode (mutually exclusive with `--trace`):
compares that run's own per-metric values against a *rolling* baseline — every strictly-earlier run
sharing the same `--group-by`/`--group-by-option` bucket the target run itself would belong to (no
separate matching-key concept, no new store schema). A metric outside the baseline's 95% CI is
reported as `above`/`below` (direction-neutral — this tool has no per-metric notion of which
direction is a regression vs. an improvement, so it reports the deviation and lets a human judge
it), alongside `within`/`no-baseline`/`thin` (fewer than `--min-runs` baseline runs). `--strict`
fails if any metric was flagged `above`/`below`.

`--phase-topdown <hostname>:<run_id>` is a standalone mode (mutually exclusive with `--trace`/
`--check-regression`): for a run collected with `--interval` + IPC counters + phase detection
(`phase.c`'s per-tick warmup/steady/degraded classification), breaks that run's own topdown output
down by phase — each topdown column's mean/n per phase, plus a `drift_pct` (the largest phase-to-
phase swing) and a trailing note naming the single largest drifter. A run collected without phase
data (aggregate, `--per-core`, or no `--interval`) degrades to an explicit notice rather than an
empty table.

```
./wspy-summary --db results/store.db                                # human table
./wspy-summary --db results/store.db --csv --metric ipc --metric retire
./wspy-summary --db results/store.db --group-by command --group-by-option affinity  # e.g. after
                                                                       # a wspy-sweep affinity sweep
./wspy-summary --db results/store.db --show-runs                    # append contributing
                                                                       # hostname:run_id per bucket
./wspy-summary --db results/store.db --trace myhost:1731000000-1234 # resolve one run to its
                                                                       # manifest/CSV/tree/plots
./wspy-summary --db results/store.db --check-regression myhost:1731000000-1234  # compare against
                                                                       # every earlier matching run
./wspy-summary --db results/store.db --phase-topdown myhost:1731000000-1234     # topdown broken
                                                                       # down by warmup/steady/degraded
```

See `./wspy-summary --help` for the full option list.

## wspy-plot: shared plotting templates

`wspy-plot` scans wspy CSV output for a `time` column (i.e. produced with `--interval`) and
renders matching built-in plot templates (topdown, cache, IBS, network I/O, a generic fallback
for anything unclaimed, ...) via `gnuplot`, one PNG per matching template per CSV. `--plot
NAME=col1,col2,...` defines a custom grouping when the built-in templates don't fit what you want
charted together.

```
./wspy-plot --rundir results/phoronix/coremark/<run-id>       # writes into <rundir>/plots by default
./wspy-plot --csv results/amdtopdown.csv --out-dir plots
./wspy-plot --list-templates
```

See `./wspy-plot --help` for the full option list.

## wspy-core-report: per-core imbalance diagnostics

`wspy-core-report` post-processes an already-collected `--per-core --csv` file: for every metric
column it reports cross-core min/max/mean/stddev/coefficient-of-variation, naming the "hot" (max)
and "cold" (min) core. On a heterogeneous host (ARM big.LITTLE, Intel Atom+Core, AMD Zen5/Zen5c) it
also breaks the same stats down by core class. Must be run on the same host that collected the CSV
(or one with identical topology) — core classes are re-detected fresh, not read from the CSV.
`--weight-by <metric>` weights each core's contribution to mean/stddev/cv by that same core's own
value of another metric column (e.g. `ipc`) instead of counting every core equally regardless of how
much work it did — on a hybrid host (or any run with wildly imbalanced core activity), a barely-active
core otherwise pulls the combined number just as hard as a fully-active one; min/max/hot/cold stay the
raw per-core values either way.

```
./wspy-core-report results/percore.csv
./wspy-core-report --csv results/percore.csv --metric ipc
./wspy-core-report results/percore.csv --weight-by ipc   # activity-weighted aggregate
```

See `./wspy-core-report --help` for the full option list.

## wspy-archetype: archetype scorecard

`wspy-archetype` classifies runs recorded in a normalized store (`wspy-store --db <path>`) along
five workload axes derived from `run_features`: `resource_dominance` (the headline axis —
compute-bound/frontend-bound/memory-bound/speculation-bound, ranked from topdown L1 percentages,
with a top-2 alternative and a confidence level), `parallelism_shape`/`control_flow_style`/
`runtime_stability` as simpler supporting tags (each `unknown` when its source feature wasn't
collected), and `memory_attribution` — cross-references topdown's own `backend_pct` against every
independently-measured cache/TLB/IBS signal the run collected, reporting `corroborated`/
`uncorroborated`/`not-memory-bound`/`unknown` (with `memory_attribution_reasons` naming which
signal(s) fired or were checked) rather than trusting a "memory-bound" topdown read on its own. On a
run collected with `--tree --tree-io-wait --tree-schedstat` (or `--tree-futex`), two more outcomes take
priority over cache/TLB/IBS corroboration: `blocked` (heavy futex/io-wait — the CPU was waiting on the
kernel, not stalled) and `oversubscribed` (heavy scheduler run-delay — runnable but not given the CPU),
since a "memory-bound" topdown read on either of those isn't a genuine hardware stall to begin with.
On AMD hosts, a `"corroborated"` `memory_attribution` also gets a `memory_attribution_locus` —
*which* cache level the stall concentrates in (`l1`/`l2-l3`/`dram`/`remote-numa`, plus a
`tlb-cofire` tag), decomposed from `--ibs-sample`'s per-sample hit-outcome tags where available
and falling back to plain cache-miss-rate signals otherwise (`memory_attribution_locus_reasons`
names which tier/signal decided it) — the AMD-only equivalent of what Intel/ARM's
`--topdown-backend` L1/L2/L3/DRAM stall-cycle chain already answers directly.

`--nearest` ranks other runs by similarity to one target run, using a coverage-aware
distance over whichever `run_features` both runs actually have `measured` (z-score-standardized,
root-*mean*-square over the shared feature set, so a pair sharing fewer features isn't penalized
just for sharing less). `--kmeans <n>` partitions every matching run into `n` clusters over that
same distance and prints a profile card per cluster (member list plus the features whose centroid
sits furthest from the population mean); a cluster's centroid averages each dimension only over
the members that actually measured it, since members can have different `run_features` coverage.

`--run-guest <json-file>` scores the same axes as `--run`, but from a flat, caller-supplied
`{"feature_name": value, ...}` JSON object instead of a database lookup — no `--db`/store needed
at all. Meant for scoring a run this host's own store has never seen (e.g. `wspy-testpoint`
scoring a machine recovered only from its already-published WordPress pages); an unrecognized key
is ignored, and a partial feature set still scores whatever axes it can.

```
./wspy-archetype --db store.db                          # score every run, one row per run
./wspy-archetype --db store.db --run somehost:2026...    # detailed single-run scorecard
./wspy-archetype --db store.db --nearest somehost:2026... --k 5   # 5 most-similar runs
./wspy-archetype --db store.db --kmeans 4                # 4 cluster profile cards
./wspy-archetype --run-guest features.json               # score a JSON feature object, no store
```

See `./wspy-archetype --help` for the full option list.

## wspy-queue: job queue processor

`wspy-queue` processes a directory of **job** files — portable, spec-only JSON describing a
workload/preset/checklist to run, captured before any output exists — through a
`pending`/`running`/`done`/`failed` lifecycle (Maildir-style directories, no daemon needed). Jobs
are added either from this tool or from the web launcher's "Queue this instead of running it now"
checkbox, and processed serially (a `wspy` run has exclusive use of the machine's PMU counters).
A job file can be copied verbatim to another machine with wspy checked out and processed there.

```
./wspy-queue add --profile deep-cpu -- phoronix-test-suite batch-run coremark
./wspy-queue run                       # drain all pending jobs
./wspy-queue list
./wspy-queue requeue <job-id>          # a failed job stays failed until requeued
```

See `./wspy-queue --help` for the full subcommand/option list.

## wspy-bundle: reproducibility bundle export

`wspy-bundle` packages one run directory's manifest(s), raw per-pass output, and derived artifacts
(plots, summary, curation, AI narrative) into a single checksummed `.tar.gz`, so a run can be archived
or handed off without access to the machine's live output-root or `store.db`. The same web launcher
report page has a "Download reproducibility bundle" link that produces the identical archive.

```
./wspy-bundle --output-root web/runs --suite demo --benchmark coremark --run-id <run-id>
./wspy-bundle --rundir /path/to/a/run/directory --dry-run    # list contents without writing
```

See `./wspy-bundle --help` for the full option list.

## wspy-publish: WordPress + report-root publishing

`wspy-publish` is the connectivity/publishing layer for `doc/REPORT_HIERARCHY.md`'s report
hierarchy: authenticates to a WordPress site via an Application Password (`web/wp_client.py`,
stdlib `urllib` only) and to a local clone of the report-root git repo. `configure` interactively
writes credentials to `~/.config/wspy/publish.json` (mode 600, via `getpass` so the Application
Password never touches shell history); `test-connection` proves both connections — WordPress auth
+ required-capability check, a lookup pass over the hierarchy's existing top-level pages, one
throwaway draft page, and a clone-or-verify plus one local (never auto-pushed) commit against the
report-root repo; `upload-media` uploads a file's raw bytes to the WordPress media library
(`--alt-text`/`--title`/`--caption`, `--set-featured-on <page-id>` to attach it to a page) —
attachments have no draft state, so the upload itself is the only step, live immediately.

Two page-publishing subcommands, for two different shapes of page: `publish-page` is the
draft-first flow for a single, standalone WP page (by `--page-id`, or `--slug`/`--parent-id` to
find-or-create) — create/update it as a draft, verify the response has an `id`/`link`, and only
flip it to `status=publish` with an explicit `--publish` flag, so a pipeline that dies mid-run
leaves an inspectable draft rather than a half-written live page; `--from-rundir <dir>` generates
its content straight from a run directory's curated blocks (`curation.json`) via
`web/server.py`'s own `render_export_wordpress()` — the same renderer the web UI's export tab
uses — pre-uploading every `depth=full` image block to the WP media library first and
substituting the resulting URLs in place of the local server's own `/files/...` links
(`--base-url` still needed for non-image "full file" reference links, since only images get
uploaded here). `publish-path` is the hierarchy-aware counterpart: given a full top-down path of
`--level SLUG TITLE` pairs (`doc/REPORT_HIERARCHY.md`'s nested-Pages levels — suite/test/
test-point/machine/run), it walks and auto-creates any missing parent stub along the way and
publishes real content only on the leaf; this is what every suite/machine/test-point publishing
script (`scripts/publish_cpu2026_benchmarks.py`, `scripts/publish_phoronix_pages.py`,
`scripts/publish_machine_page.py`, `scripts/publish_reference_matrix.py`) builds on.
`publish-path` refuses to silently clobber a page a human has hand-edited in wp-admin since wspy
last wrote it — a content fingerprint recorded at publish time
(`~/.config/wspy/publish_state.json`) is checked before any overwrite, raising a drift error
instead; `--force` bypasses the check. (`publish-page` stays a deliberately simple primitive with
no drift check of its own — for a single standalone page, not part of the hierarchy, the caller is
assumed to know what they're overwriting.)

```
./wspy-publish configure
./wspy-publish test-connection
./wspy-publish publish-page --slug my-report --title "My Report" --parent-id 17 --content-file report.html
./wspy-publish publish-page --page-id 123 --publish
./wspy-publish upload-media --file chart.png --alt-text "topdown breakdown" --set-featured-on 123
./wspy-publish publish-page --slug my-report --title "My Report" --parent-id 17 \
    --from-rundir web/runs/demo/coremark/some-run-id --base-url http://127.0.0.1:8765/files/x
./wspy-publish publish-path --level cpu2026 CPU2026 --level 706.stockfish_r 706.stockfish_r \
    --content-file benchmark.html --publish
./wspy-publish publish-path --level cpu2026 CPU2026 --level 706.stockfish_r 706.stockfish_r \
    --content-file benchmark.html --publish --force   # overwrite despite a detected hand-edit
```

See `./wspy-publish --help`/`./wspy-publish publish-page --help`/`./wspy-publish publish-path --help`
for the full option lists.

## wspy-testpoint: run selection, aggregation, and README rendering for a test point

`wspy-testpoint select-runs` resolves, for one `(suite, benchmark, machine)` combination, which role
each linked run plays — a test point's run history is rarely interchangeable repeats: some are
genuine repeated trials, some are redos superseding an earlier bad run, and some dive into more
detail with a different collection scope (extra process-tree/IBS passes) meant to supplement the
primary numbers rather than be pooled into the same statistics. Each run defaults to `stats-pool`
(matching command/pass-set, no failure), `excluded` (a failed run), or `supplementary` (a differing
pass-set), plus one run defaults to `primary`; a human's `--set RUN_ID=ROLE`/`--primary RUN_ID`
override is remembered (`human_set`/`primary_human_set`) so a later re-run's defaults never silently
overwrite it. Persists `runs.json` under `doc/REPORT_HIERARCHY.md`'s report-root
(`<suite>/<test>/<test-point>/<machine>/`), committing locally only (never pushes), same
clone-or-verify git plumbing `wspy-publish` uses (`web/report_root.py`):

```
./wspy-testpoint select-runs --suite phoronix --benchmark compress-7zip-default --machine amd-395
./wspy-testpoint select-runs --suite phoronix --benchmark compress-7zip-default --machine amd-395 \
    --set 20260801T170831.394-965bd352=excluded --primary 20260731T090012.101-abc12345
./wspy-testpoint select-runs --suite cpu2026 --benchmark 706.stockfish_r --machine amd-395 --dry-run
```

`--hostname` (default: this machine's own hostname) is the actual value runs are filtered by;
`--machine` is only the human-assigned report-root path segment (`doc/REPORT_HIERARCHY.md`'s
`<vendor>-<short-model>` convention) — no mapping between the two is assumed. Every subcommand
below shares `--suite`/`--benchmark`/`--machine`/`--report-root` plus `--phoronix-dest-root`/
`--cpu2026-dest-root` (each suite's own materialized-test-point root, needed to resolve which
report-root path this test point maps to; defaults match `web/server.py`'s own).

`wspy-testpoint aggregate` turns a resolved `stats-pool` run set into real statistics, via
`wspy-summary --run-id <hostname>:<run_id>` (one per run, not `--command`/`--hostname` — those can't
tell a redo apart from what it's redoing when they share identical command text, the single most
common reason role-assignment exists in the first place). Requires the runs already ingested into the
target `wspy-store` database — never auto-ingests, since a run only has a run-index record to ingest
at all if it was launched with `--run-index`; a `stats-pool` run not yet in the store is warned about
by name (with the `wspy-store` command to fix it) but doesn't block aggregating the rest:

```
./wspy-testpoint aggregate --suite phoronix --benchmark compress-7zip-default --machine amd-395
./wspy-testpoint aggregate --suite phoronix --benchmark compress-7zip-default --machine amd-395 \
    --db web/runs/store.db --csv --metric ipc
```

`wspy-testpoint render` turns that same aggregate into an actual curated `README.md`, reusing the 4.1
curation studio's block model (`web/server.py`) rather than inventing a second one: each counter-group
section (topdown, cache, branch, ...) is written to a small generated file under `sections/`, referenced
by an ordinary artifact block in a `curation.json` right alongside `runs.json` — a block whose section
file already existed from a prior render keeps its title/depth/commentary/position exactly as a human
left them, only the underlying file's content refreshes. A non-`PASS` verdict gets its own callout
section; `supplementary`-role runs are listed by name/reason (not their specific artifacts yet). A
"Workload characterization" section runs `wspy-archetype --run <hostname>:<run_id>` once per stats-pool
run and reports whether its "headline axis," `resource_dominance` (compute-bound/frontend-bound/
memory-bound/speculation-bound), agrees across the whole pool or diverges — real signal a workload's
characterization may be unstable across its own history, not just measurement noise, and not something
a single run's own scorecard could ever show:

```
./wspy-testpoint render --suite phoronix --benchmark compress-7zip-default --machine amd-395
./wspy-testpoint render --suite phoronix --benchmark compress-7zip-default --machine amd-395 --dry-run
```

`wspy-testpoint characterize` is a read-only counterpart to `render`'s own "Workload
characterization" section: prints the same stats-pool + WordPress-recovered-peer archetype
scorecards as one JSON object, with no report-root write/commit at all — used by the web
Reference tab's live pages, which import `wspy-testpoint` but can't be imported back by it:

```
./wspy-testpoint characterize --suite phoronix --benchmark compress-7zip-default --machine amd-395
```

This is the full run-selection/aggregation/rendering pipeline behind
`doc/INVESTIGATION_ARCHIVE.md`'s "wspy-testpoint: run selection, aggregation, and curated README
rendering for a test point" write-up — pulling specific artifacts from `supplementary` runs (today
only listed by name/reason) remains the one open follow-up. See
`./wspy-testpoint select-runs --help`/`./wspy-testpoint aggregate --help`/
`./wspy-testpoint render --help`/`./wspy-testpoint characterize --help` for the full option lists.

## wspy-symbolize: address-to-symbol resolution for --symbol-sample profiling

`wspy-symbolize` resolves the raw (instruction-pointer, hit-count) samples a `--target
... --symbol-sample` run captures into a sorted per-symbol hit-count table — the address-to-symbol
resolution half of symbol-level profiling (`topdown.c`'s `--symbol-sample`/`--symbol-sample-event`
flags capture the raw data; this tool is a separate post-hoc step, not linked into `wspy` itself, so
resolving addresses via `addr2line` never blocks the traced workload). Reads a raw `--tree` output
file (via `proctree --json`, same as the web launcher's own tree viewer), symbolizes one process
(`--pid`) or every process sharing a command name (`--comm`, merged by symbol name *after*
per-process resolution — raw addresses are never merged across processes, since ASLR generally gives
each its own load addresses):

```
./wspy --tree process.tree.txt --target=comm=myworker --symbol-sample --symbol-sample-event=cycles -- ./myworkload
./wspy-symbolize --tree-file process.tree.txt --comm myworker
./wspy-symbolize --tree-file process.tree.txt --pid 12345 --json > symbols.json
```

No call-graph (a flat self-hit table, like `perf report --no-children`); an address with no
containing `/proc/<pid>/maps` region (kernel-space samples, or JIT'd/anonymous-mapped code) reports
as `<unresolved: no backing map>` rather than being guessed at, and a resolved binary/library that
`addr2line` can't name (stripped, or a genuinely unknown offset) reports as `<unresolved: ?? symbol>`
grouped by file. See `INVESTIGATION.md`'s "Symbol-level profiling deep-dive" for the full design.
See `./wspy-symbolize --help` for the full option list.

## wspy-phoronix-import: openbenchmarking.org-seeded single-test-point suites

`wspy-phoronix-import` decomposes an already-published Phoronix result or suite into one minimal
single-test-point suite per (test, option-combination), materialized under
`workload/phoronix/<test>/<options>/` and registered with `wspy-ledger --add` — growing a
pre-profiled workload library cheaply instead of hand-authoring one `wspy-run` invocation per
benchmark. Three source methods:

```
./wspy-phoronix-import --result https://openbenchmarking.org/result/2607160-PTS-7700X3D886
./wspy-phoronix-import --file ~/Downloads/result-suite.xml --dry-run
./wspy-phoronix-import --installed-suite pts/compression-1.1.4
./wspy-phoronix-import --list-installed        # installed suites under ~/.phoronix-test-suite/test-suites
./wspy-phoronix-import --list-materialized     # already-materialized test points under workload/phoronix/
```

Re-running against the same source is additive: an already-materialized `<test>/<options>/`
directory is left untouched and reported as `exists` rather than overwritten. Materializing itself
doesn't copy anything into `~/.phoronix-test-suite/test-suites/local/`, run anything, or install
anything; the INSTALLED column (from `phoronix-test-suite info`) just flags which points still need
`phoronix-test-suite install` run by hand. The web launcher's Phoronix tab drives the identical
logic and additionally shows an inventory of already-materialized test points with a "Use in Run
tab" button per point — it copies that point's suite into `test-suites/local/` (so the command
actually works) and prefills the Run tab's workload/suite/benchmark fields; a run launched that way
gets symlinked back under `workload/phoronix/<test>/<options>/runs/<run-id>/` for easy browsing,
while the real files stay under the normal `--output-root` (so the report page, `/compare`, and
bundle export need no special-casing). See `./wspy-phoronix-import --help` for the full option
list.

## wspy-analyze: local LLM (Ollama) narrative analysis

`wspy-analyze` turns a run directory's already-computed, already-validated numbers (raw counter
output, `wspy-validate` PASS/WARN/FAIL results, which counter groups are present) into prose via a
locally running [Ollama](https://ollama.com) model — narration over classification: every bottleneck
category/verdict fed into the prompt was computed by deterministic code before this tool ever runs,
never re-derived by the model. Writes the rendered prompt (`aiprompt.txt`) and each queried model's
response (`aianalysis.<model-slug>.txt`) into the run directory itself, alongside its other artifacts
(so `wspy-bundle` and the report page pick them up automatically, labeled "AI-generated"). Needs a
running Ollama daemon; `--dry-run` renders and prints the prompt without calling it.

```
./wspy-analyze --rundir results/phoronix/coremark/<run-id> --model gpt-oss:20b
./wspy-analyze --rundir results/.../<run-id> --all-models          # query every installed model
./wspy-analyze --rundir results/.../<run-id-a> --compare-rundir results/.../<run-id-b>
                                                                      # what changed between two runs
./wspy-analyze --list-models                                        # list installed Ollama models
./wspy-analyze --rundir results/.../<run-id> --image                # narrate plots/*.topdown.png instead
```

`--critique` also asks each model to suggest improvements to the prompt template itself.
`--redact-command` omits the workload's literal command line, for use with a non-default
`--ollama-host` (pointing analysis at a remote host is a real exfiltration surface unlike the
local-only default). See `./wspy-analyze --help` for the full option list.

`--image` (bare, or with an explicit `plots/`-relative path) switches to narrating a `wspy-plot` chart
image via a vision-capable Ollama model instead of the run's text counter output — grounded in a real
numeric summary of the same CSV data (not the model's own reading of the chart's pixels for any
specific percentage), defaulting to `gemma4:26b`. Writes `aiprompt.vision.<image-stem>.md` and
`aivision.<image-stem>.<model-slug>.md`, picked up by the report page/curation studio the same way the
text narrative already is. See INVESTIGATION.md's "Vision-based topdown-chart analysis deep-dive" for
the design and a live model comparison.

## Web launcher and report browser

`web/server.py` is a stdlib-only Python web UI (no dependency, no build step), organized as
tabbed launcher pages plus a per-run report page:

* **Run** — a preset dropdown or a full configuration checklist, either way showing the exact
  command line about to run before it runs; a "Queue instead of running it now" checkbox hands the
  same job off to `wspy-queue` instead.
* **Validate** / **Store & Summary** — run `wspy-validate`/`wspy-store`/`wspy-summary`/
  `wspy-core-report` against a discovered or pasted file/database without leaving the browser.
* **Discovery** — `wspy --capabilities`/`--preflight`, no workload needed.
* **Phoronix** — decomposes an OpenBenchmarking result/installed suite into single-test-point
  suites (same logic as `wspy-phoronix-import`), a grouped/filterable inventory of what's already
  materialized, and a "Use in Run tab" prefill button per test point.
* **CPU2026** — the SPEC CPU2026 counterpart: discovers installed benchmarks/configs under a
  configurable `$SPECDIR`, tracks per-host install paths so a shared checkout works across
  multiple SPEC hosts, and offers **Build**/**Use in Run tab**/**Unregister** actions per
  benchmark×config (base/peak tracked independently; unregistering only removes the
  registration — `source.json`/`README.md`/`runs/` symlinks — never the real run data a
  `runs/` symlink points at).
* **Reference** — a benchmark reference matrix, computed on demand (no separate database): one row
  per materialized test point, one column per machine, with run counts and WordPress
  publish-status badges. Clicking a row opens a cross-machine metric-comparison detail page
  (verdict-driven warning highlighting, drill-down links to each machine's individual runs,
  archetype characterization badges per column); a "by machine" view slices the same data the
  other way (one machine's test points as rows). A machine with no local `wspy-store` presence can
  still contribute a column, recovered directly from its own already-published WordPress pages
  (real local data always wins when both exist, recovered cells marked distinctly); a "Discover
  from WordPress" button per suite finds published test points with no local trace at all. A
  "Publish reference matrix" button runs `scripts/publish_reference_matrix.py`'s site-wide static
  publish (dry-run checked by default).

The report page (one per run) has a curation studio (reorderable per-block/whole-report commentary,
inclusion depth) and export to WordPress/self-contained HTML/Markdown; a process-tree viewer/diff
page with a hot-process table, per-node `--target` counters, and a "▶ profile" drill-down that
resolves `--symbol-sample` data to real symbol names inline (no separate `wspy-symbolize` step); an
interactive timeline viewer for `--interval` CSVs (phase-shaded, synchronized-zoom small-multiple
charts); one-click "Generate characterization badge"/"Generate similarity panel" buttons
(`wspy-archetype --run`/`--nearest` output written as curatable artifacts); and AI narrative
analysis via `wspy-analyze`. Three publish buttons, each reachable without a terminal once
`wspy-publish configure` has been run (the Application Password itself is never entered through
these forms, only read from that config): "Publish to WordPress" (the single run, same pipeline as
`wspy-publish publish-page --from-rundir`), "Publish test-point report" (runs `wspy-testpoint
select-runs` + `render` for the *whole test point* this run belongs to), and, on the Reference tab,
"Publish reference matrix" above. `/history` is a searchable run browser; `/compare` puts two or
more runs side by side with an optional annotation layer.

```
python3 web/server.py                  # serves http://127.0.0.1:8765/ by default
python3 web/server.py --port 9000 --output-root /path/to/runs
python3 web/server.py --report-root /path/to/workload --report-root-remote git@example.com:you/workload.git
```

See `./web/server.py --help` for the full option list.

## Other contents

* `doc/ARTIFACT_CONTRACT.md` - the manifest/run-index/CSV/tree-file format contract (what's
  guaranteed to stay stable, how schema versioning works) plus a troubleshooting runbook for common
  partial-coverage, GPU, and validation issues
* `doc/PROFILE_COOKBOOK.md` - reading guide for wspy's analytical signals: `wspy-summary`'s
  confidence verdict, `wspy-archetype`'s classification confidence, `phase.c`'s phase output, and
  comparability signals (`mixed-pmu`, environment grouping) — what each one means and what to do
  when it fires, with real captured examples
* `doc/METRICS.md` - the index of every metric wspy/`wspy-store`/`wspy-core-report`/etc. can
  produce: canonical name, exact derivation, source function, whether/how it reaches the SQLite
  store, and (where it's genuinely meaningful) a high/low rule of thumb — read by both people and
  AI agents summarizing wspy output, and the working list for deciding what belongs in the database
* `doc/CONTRIBUTOR_GUIDE.md` - walkthrough for adding a new collector/metric/manifest-or-run-index
  field/store schema bump without breaking CSV shape, doc drift, or schema-migration checks along
  the way; expands `CLAUDE.md`'s "Common edits" section scenario by scenario
* `scripts/setup_perf.sh` - checks/adjusts `nmi_watchdog` and `perf_event_paranoid` for running
  perf counters as a non-root user, and checks/grants `CAP_PERFMON` on the `wspy` binary for
  `--power` (re-run after rebuilding — the grant is a file capability, not a sysctl)
* `workload/` - driver scripts for exercising wspy against external benchmark suites (SPEC
  CPU2017, pbbsbench, Phoronix), all calling `wspy-run --suite/--benchmark` rather than
  hand-rolling per-suite `wspy` invocations
* `web/` - the web launcher/report browser (`server.py`, see above) plus its static assets and
  job-queue library (`joblib.py`, shared with `wspy-queue`)
* `rocm/` - small standalone C++ utilities (`smi_monitor`, `smi_info`) for exploring the ROCm
  SMI API directly; not linked into wspy
* `archive/` - older version of the tool, kept for reference
* `INVESTIGATION.md` - the project's own development log/backlog: what's shipped, what's
  planned next and why, organized by release
* `doc/INVESTIGATION_ARCHIVE.md` - full design write-ups and validation narratives for work
  `INVESTIGATION.md` records as already shipped, moved out of the way of the open backlog

## License

MIT - see [LICENSE](LICENSE).
