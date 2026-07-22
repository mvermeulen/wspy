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
`wspy-queue`, `wspy-bundle`, `wspy-analyze`, and the web launcher (`web/server.py`) are plain Python 3
scripts — stdlib only, nothing to build or install (`wspy-analyze` additionally needs a running
Ollama daemon at use time, not build time, to do anything).

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
at a run-time-estimated timeout, 3600s floor), `ibs-basic`/`ibs-memory-deep` (single-pass AMD IBS
collection, see the IBS flags above), `gpu-compute` (tree tracing + system + power + both GPU
backends + topdown on one shared `--interval` timeline, for GPU-bound workloads a
separate-pass-per-category profile can't correlate tick-for-tick), and the Zen-family preset packs
`zen-portable` (`quick`+`ibs-basic`, warning-free across the whole Zen family) and `zen4plus-deep`
(`deep-cpu`+`ibs-memory-deep`, assumes Family 19h+). `-c <file>` loads a custom pass list instead
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

```
./wspy-ledger --run-index results/index.jsonl workloads.txt
./wspy-ledger --run-index results/index.jsonl --csv workloads.txt   # machine-readable
./wspy-ledger --run-index results/index.jsonl --strict workloads.txt  # nonzero exit if
                                                                        # anything's outstanding
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

See `./wspy-store --help` for the full option list.

## wspy-summary: regenerable summary tables

`wspy-summary` queries a `wspy-store` database directly and computes min/max/mean/median/stddev/CV,
a 95% confidence interval of the mean, a repeatability verdict (`PASS`, or `WARN:` plus any
combination of `thin` — too few runs, `noisy` — too much spread, `mixed-pmu` — contributing runs
differ in CPU vendor or requested/measured counter coverage), and z-score outlier flags per
`(group,metric)` bucket — grouped by workload command (default),
hostname, CPU vendor, `affinity_mode`/`preset_name`/`config_name`, or `cpu_governor`/`virt_role` —
so a summary table can always be regenerated from indexed data with no manual copy/paste.
`--group-by-option <name>` composes a *second* grouping axis from an arbitrary `--config-option`
key (e.g. a `wspy-sweep` cell's axis tag), for "this workload, broken out by X" comparisons.
`--strict` fails if any bucket is too thin (`--min-runs`), too noisy (`--max-cv`, default 5%), or
nothing matched.

```
./wspy-summary --db results/store.db                                # human table
./wspy-summary --db results/store.db --csv --metric ipc --metric retire
./wspy-summary --db results/store.db --group-by command --group-by-option affinity  # e.g. after
                                                                       # a wspy-sweep affinity sweep
./wspy-summary --db results/store.db --show-runs                    # append contributing
                                                                       # hostname:run_id per bucket
./wspy-summary --db results/store.db --trace myhost:1731000000-1234 # resolve one run to its
                                                                       # manifest/CSV/tree/plots
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

```
./wspy-core-report results/percore.csv
./wspy-core-report --csv results/percore.csv --metric ipc
```

See `./wspy-core-report --help` for the full option list.

## wspy-archetype: archetype scorecard

`wspy-archetype` classifies runs recorded in a normalized store (`wspy-store --db <path>`) along
four workload axes derived from `run_features`: `resource_dominance` (the headline axis —
compute-bound/frontend-bound/memory-bound/speculation-bound, ranked from topdown L1 percentages,
with a top-2 alternative and a confidence level) plus `parallelism_shape`/`control_flow_style`/
`runtime_stability` as simpler supporting tags, each `unknown` when its source feature wasn't
collected.

```
./wspy-archetype --db store.db                          # score every run, one row per run
./wspy-archetype --db store.db --run somehost:2026...    # detailed single-run scorecard
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
```

`--critique` also asks each model to suggest improvements to the prompt template itself.
`--redact-command` omits the workload's literal command line, for use with a non-default
`--ollama-host` (pointing analysis at a remote host is a real exfiltration surface unlike the
local-only default). See `./wspy-analyze --help` for the full option list.

## Web launcher and report browser

`web/server.py` is a stdlib-only Python web UI (no dependency, no build step) for launching runs
(a preset dropdown or a configuration checklist, either way showing the exact command line about
to run), browsing/curating/exporting reports, comparing runs side by side (with an optional
annotation layer), searching run history, viewing/diffing process trees interactively, and running
`wspy-validate`/`wspy-store`/`wspy-summary`/`wspy-core-report`/`wspy --capabilities`/`--preflight`
without leaving the browser.

```
python3 web/server.py                  # serves http://127.0.0.1:8765/ by default
python3 web/server.py --port 9000 --output-root /path/to/runs
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
