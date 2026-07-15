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
                               # wspy-store, wspy-summary, wspy-plot (no GPU support)
make AMDGPU=1                 # also builds amd_smi, amd_sysfs (needs ROCm; auto-detects /opt/rocm or /usr)
make AMDGPU=1 ROCM_DIR=<path> # point at a non-default ROCm install
make test                     # build and run the unit tests
./run_tests.sh                # build + run unit tests + integration smoke tests
./tests/arm_topdown_microbench.sh # ARM-only topdown-equivalent sanity check (skips elsewhere)
make clean                    # remove object files
make clobber                  # also remove built binaries
```

`wspy-store`/`wspy-summary` link against the system SQLite library — install `libsqlite3-dev`
(or your distro's equivalent) before building. `wspy-plot` shells out to a `gnuplot` binary at
run time, not a build-time dependency. `wspy-queue` and the web launcher (`web/server.py`) are
plain Python 3 scripts — stdlib only, nothing to build or install.

Performance counters and `--tree` (which uses `ptrace`) generally need root, or
`CAP_SYS_PTRACE` plus `perf_event_paranoid <= 1`. `scripts/setup_perf.sh` checks and, if you
confirm, adjusts the `nmi_watchdog` and `perf_event_paranoid` sysctls for the current session.
`./cpu_info` and `wspy --capabilities`/`--preflight` need no privileges at all.

## Usage

```
wspy [options] -- <command> [args...]
```

Everything after `--` (or the first non-option argument) is treated as the child command to
launch and instrument. Running `wspy` with invalid/missing arguments prints the full,
current option list.

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
    host/CPU info, counter coverage, provenance, output files produced)
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
* Process tree
  * `--tree <file>` - trace the child (and its descendants) via `ptrace`, recording
    fork/exec/exit events with timestamps to `<file>`
  * `--tree-cmdline` - also record each process's full command line
  * `--tree-vmsize` - also record virtual memory size
  * `proctree <file>` - the companion tool that reads a `--tree` file back and reconstructs
    the process hierarchy
* System-wide metrics
  * `--system` / `-s` - report load average, CPU time (`/proc/stat`), and network
    (`/proc/net/dev`) counters, plus GPU metrics if a `--gpu-*` option is also given
* Performance counters (combine as needed; `--ipc` is on by default)
  * `--ipc` / `-i` - instructions-per-cycle
  * `--topdown` / `-t`, `--topdown2`, `--topdown-frontend`, `--topdown-backend`, `--topdown-optlb`
    - Intel/AMD topdown methodology counters at various levels; on ARM, `--topdown`
      reports a topdown-equivalent decomposition from ARM PMU raw events
  * `--branch` / `-b`, `--dcache`, `--icache`, `--cache1`, `--cache2` / `-c`, `--cache3`,
    `--tlb`, `--memory`, `--opcache`, `--float` - individual hardware counter groups
  * `--software` - software counters (page faults, context switches, ...)

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
  * `--gpu-metrics` - detailed GPU metrics (temperature, activity, power, clock), read from sysfs
  * `--gpu-device=<idx>` - select a specific AMD GPU device by index for the above, on
    multi-GPU hosts (default: lowest-numbered card / SMI device 0); see `--capabilities`
    for the enumerated device list

Examples:

```
sudo ./wspy -- sleep 1                      # default IPC counters around `sleep 1`
sudo ./wspy --csv --topdown -- myapp arg1   # CSV output with topdown counters
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
at a 3600s timeout since an hour of process-tree records is already more than practical to
publish), and `ibs-basic`/`ibs-memory-deep` (single-pass AMD IBS collection, see the IBS flags
above). `-c <file>` loads a custom pass list instead (`<pass-name> <wspy-flags...>` per line), and
a comma-separated profile list (e.g. `deep-cpu,tree-heavy`) composes more than one builtin
profile's passes into a single invocation — see `./wspy-run --help` for the full option list and
config-file format. Each pass writes to `<outdir>/<prefix><pass-name>.<csv|txt>` by default, or
into the unified per-run directory layout above when `--suite`/`--benchmark` are given (see
`doc/ARTIFACT_CONTRACT.md`'s "Unified output layout" section for the full directory contents);
`--manifest-dir`/`--run-index` pass those `wspy` flags through per pass either way.

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

`wspy-summary` queries a `wspy-store` database directly and computes min/max/mean/median/stddev
and z-score outlier flags per `(group,metric)` bucket — grouped by workload command (default),
hostname, or CPU vendor — so a summary table can always be regenerated from indexed data with no
manual copy/paste.

```
./wspy-summary --db results/store.db                                # human table
./wspy-summary --db results/store.db --csv --metric ipc --metric retire
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

## Web launcher and report browser

`web/server.py` is a stdlib-only Python web UI (no dependency, no build step) for launching runs
(a preset dropdown or a configuration checklist, either way showing the exact command line about
to run), browsing/curating/exporting reports, comparing runs side by side, searching run history,
and running `wspy-validate`/`wspy-store`/`wspy-summary`/`wspy --capabilities`/`--preflight`
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
* `scripts/setup_perf.sh` - checks/adjusts `nmi_watchdog` and `perf_event_paranoid` for running
  perf counters as a non-root user
* `workload/` - driver scripts for exercising wspy against external benchmark suites (SPEC
  CPU2017, pbbsbench, Phoronix), all calling `wspy-run --suite/--benchmark` rather than
  hand-rolling per-suite `wspy` invocations
* `web/` - the web launcher/report browser (`server.py`, see above) plus its static assets and
  job-queue library (`joblib.py`, shared with `wspy-queue`)
* `rocm/` - small standalone C++ utilities (`smi_monitor`, `smi_info`) for exploring the ROCm
  SMI API directly; not linked into wspy
* `archive/` - older version of the tool, kept for reference
* `INVESTIGATION_4.0.md` - the project's own development log/backlog: what's shipped, what's
  planned next and why, organized by release

## License

MIT - see [LICENSE](LICENSE).
