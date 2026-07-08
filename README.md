# wspy 3.0

wspy - a workload spy

wspy is an instrumentation wrapper: it launches a child command, lets it run, and reports
runtime + hardware performance-counter + system metrics gathered while it ran. It's the
author's internal testbed for workload-characterization experiments, published to make it
easy to pull onto different machines; listed as public in case it's otherwise useful.

## Building

```
make                          # builds wspy, cpu_info, proctree (no GPU support)
make AMDGPU=1                 # also builds amd_smi, amd_sysfs (needs ROCm; auto-detects /opt/rocm or /usr)
make AMDGPU=1 ROCM_DIR=<path> # point at a non-default ROCm install
make test                     # build and run the unit tests
./run_tests.sh                # build + run unit tests + integration smoke tests
make clean                    # remove object files
make clobber                  # also remove built binaries
```

Performance counters and `--tree` (which uses `ptrace`) generally need root, or
`CAP_SYS_PTRACE` plus `perf_event_paranoid <= 1`. `scripts/setup_perf.sh` checks and, if you
confirm, adjusts the `nmi_watchdog` and `perf_event_paranoid` sysctls for the current session.

## Usage

```
wspy [options] -- <command> [args...]
```

Everything after `--` (or the first non-option argument) is treated as the child command to
launch and instrument. Running `wspy` with invalid/missing arguments prints the full,
current option list.

Some of the more commonly used options:

* `--version` - print the wspy version and exit

* Output
  * `-o <file>` - send output to a file instead of stdout
  * `--csv` - CSV output instead of human-readable
  * `--interval <sec>` - print a snapshot every `<sec>` seconds while the child runs
  * `--verbose` / `-v` - verbose diagnostics (repeat for more detail)
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
    - Intel/AMD topdown methodology counters at various levels
  * `--branch` / `-b`, `--dcache`, `--icache`, `--cache1`, `--cache2` / `-c`, `--cache3`,
    `--tlb`, `--memory`, `--opcache`, `--float` - individual hardware counter groups
  * `--software` - software counters (page faults, context switches, ...)
* AMD GPU metrics (only available when built with `AMDGPU=1`)
  * `--gpu-smi` - GPU info via ROCm's `amd_smi` library
  * `--gpu-busy` - instantaneous GPU busy percent, read from sysfs
  * `--gpu-metrics` - detailed GPU metrics (temperature, activity, power, clock), read from sysfs

Examples:

```
sudo ./wspy -- sleep 1                      # default IPC counters around `sleep 1`
sudo ./wspy --csv --topdown -- myapp arg1   # CSV output with topdown counters
sudo ./wspy --tree tree.out -- myapp        # record the process tree while myapp runs
./proctree tree.out                         # display the tree recorded above
sudo ./wspy --system --gpu-busy -- myapp    # system + GPU metrics (needs AMDGPU=1 build)
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
```

Builtin profiles: `quick` (one fast IPC+system pass), `deep-cpu` (the multi-pass counter sweep
used for topdown characterization), `deep-gpu` (`deep-cpu` plus GPU busy/metrics passes), and
`tree-heavy` (a single `--tree` pass with full command-line capture). `-c <file>` loads a custom
pass list instead (`<pass-name> <wspy-flags...>` per line) — see `./wspy-run --help` for the
full option list and config-file format. Each pass writes to
`<outdir>/<prefix><pass-name>.<csv|txt>`; `--manifest-dir`/`--run-index` pass those `wspy` flags
through per pass.

## Other contents

* `scripts/setup_perf.sh` - checks/adjusts `nmi_watchdog` and `perf_event_paranoid` for running
  perf counters as a non-root user
* `workload/` - driver scripts for exercising wspy against external benchmark suites (SPEC
  CPU2017, pbbsbench, Phoronix)
* `rocm/` - small standalone C++ utilities (`smi_monitor`, `smi_info`) for exploring the ROCm
  SMI API directly; not linked into wspy
* `archive/` - older version of the tool, kept for reference

## License

MIT - see [LICENSE](LICENSE).
