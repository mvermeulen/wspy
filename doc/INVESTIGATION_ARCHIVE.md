# wspy Investigation Archive

This file holds full design write-ups and validation narratives for work that has **shipped and is
done** — moved out of `INVESTIGATION.md` so that document stays focused on what's still open. Nothing
here is active backlog; if an archived item needs revisiting (a follow-up, a v2, a newly-discovered
gap), open a fresh entry in `INVESTIGATION.md` itself rather than editing history here.

`INVESTIGATION.md`'s "What shipped in 4.2" section links back to specific entries in this file by name.
`CLAUDE.md` remains the authority on current mechanism/behavior for every file/tool named below — this
archive is *why* a thing was built the way it was and *how it was validated*, not a substitute for
reading the code.

## Design deep-dives (fully shipped)

### Concrete design: blocking I/O + `/proc/<pid>/io` byte counters (2026-07-17, shipped)
**Shipped 2026-07-17,** exactly as designed below: `--tree-io`/`--tree-io-wait` (`wspy.c`/`topdown.c`),
`-I`/`-B` (`proctree.c`), and the matching web launcher checklist rows/`web/joblib.py` wiring. Validated
with a real delayed-blocking-read test program (`bash -c 'exec 3< <(sleep 0.3; echo hi); read -u 3
line'` under `wspy --tree --tree-io-wait`) cross-checked against `strace -f -T`: wspy measured
`io_read_wait_seconds=0.314739` for the blocking read, strace's own ground truth was `0.314351` —
within 0.4ms. `tests/capability_matrix.sh` gained a `tree-io-io-wait` bundle; the full test suite
(`make test`, `./run_tests.sh`, `tests/golden_output.sh`, `tests/capability_matrix.sh`,
`web/test_joblib.py`) passes unchanged. See `CLAUDE.md`'s `topdown.c`/`proctree.c` entries for the
shipped mechanism's actual behavior.

Investigated together because they're the two natural "how much I/O, and how slow was it" halves of
the same question, even though they're mechanically unrelated — a passive kernel-counter *volume* read
(no syscall tracing involved) versus a ptrace-based *latency* measurement, generalizing `--tree-futex`'s
own per-pid syscall entry/exit state exactly as its own design note anticipated ("benefits every future
syscall-latency flag, not just this one"). Neither subsumes the other: byte counts don't say whether a
process struggled to get them (cache-served vs. a real device/network wait), and wait time doesn't say
how much data was involved. Ships as two independent flags, like `--tree-open`/`--tree-futex` already
are — a run can use either alone or both together.

**`--tree-io` (byte volume):**
- No `trace_syscall` needed — mechanically identical to the existing `/proc/<pid>/stat` scrape
  already done in `ptrace_loop()`'s `PTRACE_EVENT_EXIT` handling, just a second file. At the same
  point `/proc/<pid>/stat` is read, additionally open `/proc/<pid>/io`, parse its 7 `label: value`
  lines (`rchar`/`wchar`/`syscr`/`syscw`/`read_bytes`/`write_bytes`/`cancelled_write_bytes`), and
  emit one line — `<time> <pid> io <rchar> <wchar> <syscr> <syscw> <read_bytes> <write_bytes>
  <cancelled_write_bytes>` — into the exit block, written *before* the `exit` line (same ordering
  rule `--tree-futex` already established, since `proctree.c`'s `handle_exit()` drops the pid from
  its lookup table as soon as it sees `exit`).
- Gated behind its own flag rather than unconditional like `-M`/`-N`/`-P`'s stat sub-fields: unlike
  those (free, since `/proc/<pid>/stat` is already being read regardless), `/proc/<pid>/io` is a
  second file open/read/close per exiting process — real added cost at the 100K+-process scale the
  tree mechanism has now been validated against. An unreadable file (permissions, an already-fully-
  reaped pid, or an old/minimal kernel built without `CONFIG_TASK_IO_ACCOUNTING`) just skips the `io`
  line — measured-vs-unavailable, not fatal.
- `proctree.c`: `struct process_info` gains the 7 fields; a new `handle_io()` parses the line; a new
  print-time toggle `-I`/`-i` (default off, *conditional* like `-C`/`-X` — the data only exists in
  the raw file if `--tree-io` was used, unlike `-M`/`-N`/`-P`'s unconditional stat fields) adds it to
  `print_tree()`'s per-line output; `print_statistics()`'s per-`comm` table gets a matching
  bytes-read/written aggregate column.
- No manifest/run-index change — same precedent as `--tree-futex` (a tree-file/`proctree`-only
  feature, no `MANIFEST_SCHEMA_VERSION` bump).

**`--tree-io-wait` (blocking latency):**
- Sets `tree_io_wait = 1; trace_syscall = 1;` — same shape as `--tree-futex`. Unlike futex, there's
  no op argument to decode: every syscall in scope is a candidate, and its own entry→exit duration
  *is* the signal (2µs means it didn't really block; 200ms means it did) — so this is actually
  simpler to decode than futex, not harder.
- Table-driven rather than one-off `if` branches: read-side
  (`SYS_read`/`SYS_pread64`/`SYS_readv`/`SYS_preadv`/`SYS_recvfrom`/`SYS_recvmsg`) and write-side
  (`SYS_write`/`SYS_pwrite64`/`SYS_writev`/`SYS_pwritev`/`SYS_sendto`/`SYS_sendmsg`).
- **Design call, differs from futex's own precedent:** two accumulator buckets on
  `struct ptrace_pid_entry` (`io_read_wait_{count,seconds}` / `io_write_wait_{count,seconds}`)
  rather than futex's single lumped bucket — blocked-reading (waiting on upstream data) and
  blocked-writing (downstream backpressure) are different bottleneck stories worth keeping apart, and
  splitting them costs nothing beyond one more `if` on direction at entry.
- Reporting line (same "before `exit`" placement as `futex`): `<time> <pid> io_wait <read_count>
  <read_seconds> <write_count> <write_seconds>`, emitted only when at least one count is nonzero.
- **Explicitly out of scope for this slice:** per-call bytes-transferred capture (would need new
  `PTRACE_SYSCALL_ARG3`/`PTRACE_SYSCALL_RET` macros in `ptrace_arch.h`, plus a remote-memory iovec
  walk for `readv`/`writev`/`preadv`/`pwritev` to sum vectored lengths). Correlating volume and
  latency happens at the report layer via `--tree-io`'s cumulative counters instead, not per traced
  call.
- `proctree.c`: `handle_io_wait()` parses the line; a new print toggle `-B`/`-b` ("blocked," unused
  letters) mirrors `-X`/`-x`'s treatment exactly, both per-line and in `print_statistics()`'s
  aggregate table.

Report-layer payoff: `io_wait_seconds / (read_bytes+write_bytes)` is a rough "seconds blocked per
byte" figure — high means a slow/contended I/O path, low means genuinely throughput-bound; and
`rchar - read_bytes` (both already inside `--tree-io`'s own line) separates "logical" reads
(including page-cache hits) from real device I/O, so a process with large `rchar`, tiny `read_bytes`,
and near-zero `io_wait` is cache-bound, not I/O-bound at all — a distinction neither counter can make
alone.

Both flags are independent (no fatal-combination rule between them, or with `--tree-open`/
`--tree-futex`) — a run can enable any subset.

### Concrete design: `/proc/<pid>/schedstat` run-delay/timeslice capture (2026-07-17, shipped)
Motivation: `--tree-futex`/`--tree-io-wait` tell apart "on-CPU and stalled" (topdown) from "blocked in
the kernel on a lock or I/O." But a degraded phase with *no* blocking-syscall activity at all was
previously assumed to be "a genuine hardware stall worth chasing with topdown/cache counters" — true
only if the process was actually given the CPU. A runnable-but-not-scheduled process (CPU
oversubscription, cgroup CFS throttling, an over-committed VM host) produces exactly the same
signature — no futex/io-wait, low IPC — for a completely different reason: it never got dispatched.
`/proc/<pid>/schedstat` is the kernel's own answer to "how long was this task runnable but waiting on
a runqueue," and closes that gap.

**Mechanism — passive read, not a ptrace feature:** like `--tree-io` and unlike
`--tree-futex`/`--tree-io-wait`, this needs no `trace_syscall`/syscall-table entry at all — it's a
second `/proc/<pid>/<file>` scrape at the exact point `topdown.c`'s `PTRACE_EVENT_EXIT` handler already
opens `/proc/<pid>/io` (right before the existing `/proc/<pid>/stat` dump). `/proc/<pid>/schedstat` is
one line, three whitespace-separated `u64` fields in nanoseconds/count: time spent actually running on
a CPU, time spent runnable-but-waiting on a runqueue (**run-delay**), and the number of timeslices the
task has been scheduled. Parsed with a single `sscanf(line, "%llu %llu %llu", &cpu_ns, &rundelay_ns,
&nr_timeslices)` — no per-label `sscanf` loop like `/proc/<pid>/io`'s `label: value` lines needed,
since schedstat has no labels at all, just three numbers in fixed order.

**The one real gotcha, and why it needs its own check:** `CONFIG_SCHEDSTATS` being compiled in is not
enough — since Linux 4.6 there's a runtime jump-label toggle, `/proc/sys/kernel/sched_schedstats`
(boolean), and when it reads `0` the file still *exists* and still *reads successfully*, it just
returns all-zero fields, indistinguishable at the syscall level from "this process was never delayed."
A silent all-zero here would misreport "zero run-delay" as a real measurement when it actually means
"not being measured," exactly backwards from what this feature exists to show.

**Revised during implementation (2026-07-17), after real-hardware testing:** the originally-designed
sysctl-based check turned out to be unreliable in practice — on the actual test host,
`/proc/sys/kernel/sched_schedstats` read `0` while `/proc/self/schedstat`/`/proc/1/schedstat` were
already returning real, substantial nonzero data, meaning something had force-enabled scheduler-stat
collection outside the sysctl's own write path, leaving the sysctl file's cached value stale. Trusting
it as designed would have produced a false-positive "readings will be zero" warning on a host where
readings were, in fact, real. The shipped `check_schedstat_enabled()` (`topdown.c`) instead reads this
process's own `/proc/self/schedstat` and warns only if its `nr_timeslices` field is genuinely zero —
this process has certainly been scheduled at least once by the time `main()` reaches this check, so a
real zero there means collection isn't happening, independent of whatever the sysctl file claims.
Confirmed via a real CPU-oversubscription test (32 busy-spin processes on a 16-core host, `run_delay`
totaling ~32.9s) against an undersubscribed control (8 processes, ~0.003s) — the signal tracks genuine
contention.

**CLI flag:** `--tree-schedstat` (`tree_schedstat = 1;`); no `trace_syscall = 1`, same as `--tree-io`.
Inert without `--tree`, same precedent as every other `--tree-*` flag.

**Reporting line, same "before `exit`" placement as futex/io_wait/io:** `<time> <pid> schedstat
<cpu_seconds> <rundelay_seconds> <nr_timeslices>` — `cpu_ns`/`rundelay_ns` converted to seconds for
consistency with `futex`/`io_wait`'s seconds-based fields, `nr_timeslices` left as a raw count. Emitted
whenever the read succeeds, **not** gated on `rundelay_ns > 0` — same "zero is real data" precedent as
`--tree-io`.

**`proctree.c`:** `struct process_info` gains three fields (`sched_cpu_seconds`,
`sched_rundelay_seconds`, `sched_nr_timeslices`); `handle_schedstat()` parses the line. **Design call —
only two of the three fields are surfaced by default:** the print toggle (`-D`/`-d`, default off) shows
`run_delay=%.3f timeslices=%llu` per line and a matching aggregate in `print_statistics()`'s per-`comm`
table, but deliberately leaves `sched_cpu_seconds` out of both — it's stored but not printed, because
`-U` already shows `utime`/`stime` from `/proc/<pid>/stat`, and schedstat's own nanosecond-precision
on-CPU time is a *second*, differently-quantized measurement of nearly the same thing from a different
kernel accounting path — printing both next to each other invites "which one is real CPU time"
confusion for no diagnostic benefit, when the whole point of this feature is the run-delay number
`/proc/<pid>/stat` has no equivalent of at all.

**Validation — weaker ground truth than futex/io-wait, worth saying up front:** those had `strace -f
-T` as an independent second measurement to cross-check against; this feature is a straight passthrough
of a single kernel-provided number, so there's no comparably independent oracle. Validated instead via
a synthetic CPU-oversubscription test (spawn more busy-spin child processes than available cores under
`wspy --tree --tree-schedstat`) and confirming `run_delay` responded to induced contention as expected.

**Report-layer payoff:** completes a three-way split of degraded phases: heavy futex/io-wait (blocked
in the kernel, waiting is the story), heavy `run_delay` with low futex/io-wait (runnable but not
scheduled — an oversubscription/placement problem, fixable with `--affinity` or fewer concurrent jobs,
not a counter-chasing exercise), or neither (a genuine on-CPU hardware stall, now the only case
topdown/cache counters are actually being asked to explain). Before this, the second and third cases
were indistinguishable from counters alone.

### Concrete design: memory footprint detail via `/proc/<pid>/status` (2026-07-17, shipped)
Motivation: the existing `--tree`/`proctree` `-M` toggle already shows `vmsize`/`rss` (from
`/proc/<pid>/stat`'s always-present fields), but that's a single combined resident-set number with no
peak and no composition — it can't distinguish a process whose memory footprint is real growing
anonymous heap from one that's large mostly because it mapped a big shared/file-backed region.

**A pre-existing wart this closed as a side effect:** `--tree-vmsize` already existed as a CLI flag
(`wspy.c`), but it was, and had always been, a complete no-op — `tree_vmsize` was set to `1` and never
read anywhere else in the codebase, a state `wspy.c`'s own comment on the flag's `case` documented
explicitly. Rather than leave that dead flag in place and add a second, confusingly-named sibling flag,
this design **repurposed `--tree-vmsize` to actually do something** — safe to do since a pure no-op has
no existing behavior anything could depend on. The web launcher's existing "vmsize samples" checkbox
already emitted `--tree-vmsize` when checked, so this also made that checkbox meaningful for the first
time rather than requiring a parallel UI change.

**Mechanism — passive read, same family as `--tree-io`/`--tree-schedstat`:** no `trace_syscall`, no
ptrace/syscall-table changes. A fourth scrape at the same `PTRACE_EVENT_EXIT` point, reading
`/proc/<pid>/status`: `VmHWM` (peak RSS), `RssAnon`, `RssFile`, `RssShmem` (the anon-vs-file-vs-shmem
composition of current RSS), and `VmSwap` — all `label:\tvalue kB` lines.

**Reporting line:** `<time> <pid> vmsize <hwm_kb> <rss_anon_kb> <rss_file_kb> <rss_shmem_kb>
<swap_kb>` — kept in the file's native kB units, keyed `vmsize` to match the flag name directly.
Emitted whenever the read succeeds, not gated on any field being nonzero.

**`proctree.c`:** `struct process_info` gains five fields; `handle_vmdetail()` parses the line. New
print toggle `-R`/`-r` (default off), deliberately a different letter than `-M` (which stays exactly as
it is today: unconditional, from `/proc/<pid>/stat`'s always-present fields) rather than overloading
`-M` itself — the two toggles cover genuinely different data sources with different availability
guarantees.

**Real-workload testing surfaced a genuine caveat, not a bug:** validated against two versions of a
200MB-anon-allocation Python test program under `wspy --tree --tree-vmsize`. A version that let
CPython's normal interpreter-shutdown sequence run (freeing the allocation before the process actually
exits) recorded `VmHWM` accurately (~266MB, matching a concurrent direct `/proc/<pid>/status` read
almost exactly) but `RssAnon` near-zero (~2MB) — because by the time `PTRACE_EVENT_EXIT` fires, the
memory had already been freed/`munmap`'d during the interpreter's own teardown. A second version calling
`os._exit(0)` immediately after allocating (skipping that teardown) recorded `RssAnon` at ~208MB, as
expected. **Conclusion:** `VmHWM` is a true kernel-tracked historical high-water mark and stays accurate
regardless of what the process does right before exiting, but `RssAnon`/`RssFile`/`RssShmem`/`VmSwap`
are an *exit-time snapshot*, not a peak-time composition — a process that frees or unmaps memory as part
of its own normal shutdown (common in interpreted-language runtimes) will under-report its composition
here relative to what it held at its actual peak, even though the peak *total* (`VmHWM`) is unaffected.
Worth documenting as an inherent limitation of this data source, not something a future revision needs
to "fix."

**Report-layer payoff:** pairs with `--tree-io`'s existing `rchar`/`read_bytes` distinction to separate
three different "why is this process big/slow" stories that all currently look similar from
`vsize`/`rss` alone: heavy `RssFile`/`rchar`-without-`read_bytes` is page-cache-bound; heavy `RssAnon`
with a high `VmHWM` is a genuine allocation-heavy workload; nonzero `VmSwap` under memory pressure
explains a slowdown no hardware counter would otherwise account for.

### Concrete design: `--tree-connect`/`--tree-wait`/`--tree-poll`/`--tree-nanosleep` (2026-07-17, shipped)
Ships the last four of six named syscall-latency candidates in one PR — `connect`, `nanosleep`/
`clock_nanosleep`, `wait4`/`waitid`, and the `poll`/`epoll_wait` family (futex and blocking I/O shipped
first, see above) — bundled together because mechanically they're all near-identical extensions of the
exact per-pid entry/exit-timing mechanism `--tree-io-wait` already established: no new `ptrace_arch.h`
macros, no new architecture, just four more syscall-number comparisons feeding the same
`ptrace_pid_entry` accumulate-at-exit/flush-in-exit-block pattern `--tree-futex` uses. Confirmed x86_64
syscall numbers against real headers: `SYS_connect`=42, `SYS_wait4`=61, `SYS_waitid`=247, `SYS_poll`=7,
`SYS_ppoll`=271, `SYS_select`=23, `SYS_pselect6`=270, `SYS_epoll_wait`=232, `SYS_epoll_pwait`=281,
`SYS_nanosleep`=35, `SYS_clock_nanosleep`=230 — notably **no separate `SYS_waitpid` exists on x86_64**
(glibc's `waitpid()` is a thin wrapper over the `wait4` syscall on this arch), so the "wait" bucket only
needs to watch `wait4`/`waitid`.

**Scope decision:** originally the plan called `connect`/`nanosleep` "naturally one line per
occurrence" (rare events, like `--tree-open`'s existing `open <path>` lines) and reserved per-pid
aggregation for high-frequency events. The shipped design instead **aggregates all four the same way**
(count + total seconds per pid, exactly `--tree-futex`'s own shape) rather than building a new per-call
event-log print path for two of them — because that path doesn't actually exist in `proctree.c`:
`--tree-open`'s own `open <path>` lines aren't parsed by `proctree.c` at all (every one triggers an
"unknown command" warning). Building per-call log-line UI machinery just for `connect`/`nanosleep`
ahead of a real need for it (a `--tree-open` report-layer summary) would be solving the same problem
twice with two different shapes. Aggregating uniformly keeps this the size of "four more
`--tree-futex`-shaped buckets," fully bounded and predictable in tree-file size regardless of call
frequency.

**The four buckets:**
- **`--tree-connect`** (`tree_connect`): `SYS_connect` only. `connect_count`/`connect_seconds` per
  pid. No sockaddr/remote-address decoding in this slice.
- **`--tree-nanosleep`** (`tree_nanosleep`): `SYS_nanosleep` + `SYS_clock_nanosleep`.
  `nanosleep_count`/`nanosleep_seconds` per pid.
- **`--tree-wait`** (`tree_wait`): `SYS_wait4` + `SYS_waitid` (both mean "blocked waiting for a
  child" — no reason to split into separate buckets). `wait_count`/`wait_seconds` per pid.
- **`--tree-poll`** (`tree_poll`): `SYS_poll`/`SYS_ppoll`/`SYS_select`/`SYS_pselect6`/
  `SYS_epoll_wait`/`SYS_epoll_pwait` via `classify_poll_syscall()`. `poll_count`/`poll_seconds` per
  pid. No timeout-argument decoding in this slice.

**Mechanism:** all four need `trace_syscall = 1` since they're ptrace-timed. No new entry-stop
decoding needed for any of the four — every matched syscall's own entry→exit duration is itself the
signal. `struct ptrace_pid_entry` gains 8 new fields (`{connect,nanosleep,wait,poll}_{count,seconds}`);
the exit-stop dispatch (`ptrace_loop()`) gains four more `if (tree_X && ...)` checks alongside the
existing futex/io-wait ones — a fully generalized "syscall name → number → decode → log-vs-aggregate"
table is more machinery than four more `if` branches justify at this count; revisit that generalization
if a *seventh* syscall family is added, not before.

**Reporting lines, all "only when count>0" like `--tree-futex`:**
```
<time> <pid> connect <count> <seconds>
<time> <pid> nanosleep <count> <seconds>
<time> <pid> wait <count> <seconds>
<time> <pid> poll <count> <seconds>
```

**`proctree.c`:** four new print toggles: `-K`/`-k` (connect), `-J`/`-j` (wait — `-W`/`-w` was
unavailable, `-w` already takes an argument for the output-width option), `-L`/`-l` (poll), `-Z`/`-z`
(nanosleep, "zzz"). All four follow `-X`'s exact shape.

This ships all six of the Critical-path/synchronization-latency deep-dive's named candidates (futex,
blocking I/O, connect, nanosleep, wait, poll). The general table-driven mechanism (mapping syscall name
→ number → decode → log-vs-aggregate) was deliberately **not** built — six syscall families were still
cheap enough as individual `if` branches; revisit only if a seventh comes up.

### Concrete design: CPU energy/power via the `power`/`power_core` perf PMUs (2026-07-17, shipped)
Motivation: none of wspy's counter groups reported CPU energy or power before this — the only
power/energy signal anywhere in wspy was GPU-side (`amd_smi.c`/`amd_sysfs.c`). That's a real gap for
perf-per-watt analysis: a workload whose IPC or topdown numbers look identical across two
configurations (different governor, SMT on/off, affinity placement) can still differ substantially in
energy cost. Confirmed live on the dev host (AMD Zen5, `family 19 model 74`): `/sys/bus/event_source/
devices/power/` (type 14, event `energy-pkg`, package-scope Joules) and `/sys/bus/event_source/devices/
power_core/` (type 15, event `energy-core`, per-physical-core Joules) both exist and are readable by
`perf`/`perf_event_open()`, unprivileged. This is the same kernel `power` PMU family that has reported
Intel RAPL's `energy-pkg`/`energy-cores`/`energy-ram` events for years — AMD Family 19h support is
newer, but the event *names* wspy opens (`energy-pkg`, `energy-core`) are the same across both vendors,
so — unlike IBS — this group needs no `VENDOR_AMD`/`VENDOR_INTEL` branching in the probe path itself,
only in whether the device nodes happen to exist. Unverified on real Intel hardware at time of writing.

**Mechanism — dynamic PMU discovery, same shape as `ibs.c`'s `ibs_probe()`, but much simpler:**
`power`/`power_core` each expose exactly one format field (`event`, a plain 0-255 raw index) and one
real event apiece (`energy-pkg`/`energy-core`), each with a sidecar `<event>.scale` (Joules-per-LSB,
`2^-32` on the dev host — read at runtime, not hardcoded) and `<event>.unit` (`"Joules"`, read too).
`power_probe()` (`power.c`/`power.h`) reads these into a `struct power_capabilities`; absent sysfs
directories just mean `present=0`, never fails. `power_counter_group()` opens `energy-pkg` as one
`PERF_TYPE_RAW`-equivalent counting event using the discovered dynamic `type`, exactly like
`raw_counter_group()`'s existing L3/IBS escape hatch for a non-standard `type_id`.

**Scale handling — the one genuinely new piece of plumbing:** every existing counter group's raw delta
is either printed as-is (a count) or divided by another counter in the same group (a ratio/percentage);
none of them multiply by an event-specific floating-point scale read from sysfs. `struct counter_info`
gained an optional `double scale` (default 1.0, unused by every other group) that `power_counter_group()`
sets from the probed `.scale` file; `read_counters()`'s existing multiplex-scaling step also multiplies
by `.scale` when set, so `.value` ends up in Joules directly and `print_power()` never needs to know the
raw LSB encoding.

**CLI flag:** `--power`/`--no-power` (new `COUNTER_POWER` bit), default off, **deliberately excluded
from `COUNTER_ALL`**, following the IBS precedent — `--capabilities` gets its own dedicated
`power_probe()`/`print_power_capability_report()` path.

**CSV/human output:** `pkg_joules` (cumulative delta over the run) plus a derived `pkg_watts`
(`pkg_joules / elapsed_seconds`). System-wide only (like software counters/IBS), not per-core, for v1.

**V1 scope deliberately excluded `power_core` (per-core energy):** unlike `energy-pkg`, `power_core`'s
own `cpumask` meant a real per-core breakdown needed opening N events, one pinned per representative
CPU, and aggregating into `--per-core`'s existing per-core row shape — a separate unit of work, not a
bigger version of the same call. Shipped since (INVESTIGATION.md's "What shipped in 4.2", "Per-core
energy support" item) — see `CLAUDE.md`'s `power.c` entry for the full mechanism and real-hardware
validation.

**Validated against real hardware on the dev host:** the sysfs-derived `--capabilities` report, graceful
degradation without `CAP_PERFMON`, and (via `sudo`) real non-zero `pkg_joules`/`pkg_watts` values from a
`sudo ./wspy --csv --no-ipc --power --interval 1 -- sleep 3` run — `pkg_watts` was internally consistent
on every row (~60W steady state) and correctly tracked each row's *actual* accumulation window
(reconstructible as `pkg_joules/pkg_watts`: ~3.0s, ~1.0s, ~1.0s, ~0.005s for that run's four rows) rather
than assuming a fixed interval. **That first-row 3.0s (not 1.0s) window surfaced a genuine, pre-existing
wspy behavior `--power` is the first counter group to make visible:** every group's counters start
(`start_counters()`) before `wspy.c`'s fixed 2-second pre-launch `sleep(2)`, so a first read's raw delta
always covers that ~2s of pre-launch time too — invisible for self-normalizing ratios like IPC/topdown,
but visible in `--power`'s absolute Joules. `pkg_watts` is unaffected (it divides by the real window
either way); only a bare first-row `pkg_joules` value needs this caveat.

**Web launcher support** shipped alongside it: a dedicated "CPU power" checklist card (mirroring AMD
IBS's own dedicated card rather than folding into "Performance counters", since `--power` isn't part of
the `--passes`-bin-packed `ALL_GROUPS` vocabulary either) plus custom-plot column autofit for
`pkg_joules`/`pkg_watts`. The Run tab's "Check" button also gained a real `--power` probe
(`power_probes_for_request()`/`probe_power()`) mirroring its existing AMD IBS probe: sysfs-presence
discovery (`--capabilities`) can't see that RAPL/`energy-pkg` access needs root or `CAP_PERFMON`
specifically — confirmed live, `--ibs-basic` opens fine at the same `perf_event_paranoid` level that
denies `--power` with `EACCES` — so only an actual `perf_event_open()` attempt catches it before a real
run wastes time on it. On that `EACCES` specifically, the probe's detail message tells the user what to
do (run under `sudo`, or `sudo setcap cap_perfmon+ep <path to wspy>` once), and notes explicitly that
`scripts/setup_perf.sh`'s sysctl adjustments don't fix this on their own.

### Core/thread affinity control (`--affinity`, shipped ahead of schedule)
Landed as `--affinity=all|thread=<id>|nosmt|domain=<id>|coretype=<id>|cpuset=<c0,c1,...>` (`wspy.c`),
applied via `sched_setaffinity()` on the forked child before `execve` in `topdown.c`'s `launch_child()`:
`all` (default, every CPU currently visible to this process), `thread=<id>` (that single logical CPU,
letting a caller deliberately avoid its SMT sibling), `nosmt` (one primary/lowest-numbered SMT thread
per core, across every core — the "turn off hyperthreading" preset), `domain=<id>` (every thread on one
L3-sharing core-complex/CCD — e.g. picking Zen5's 16 MiB-L3 complex vs. Zen5c's 8 MiB-L3 complex on a
mixed part), `coretype=<id>` (every thread of one MIDR-distinct microarchitecture — e.g. a big.LITTLE
ARM part's "big" Cortex-A7xx cores vs. its "little" Cortex-A5xx ones, added once a real such host — 8x
Cortex-A720 + 4x Cortex-A520 sharing one combined 12 MiB L3, so `domain=<id>` alone couldn't separate
them — came up), and `cpuset=<c0,c1,...>` (explicit enumerated core list/ranges — the general form the
others are shorthand for).

`affinity.c`'s own topology discovery (SMT sibling grouping via `topology/thread_siblings_list`,
L3-domain grouping via `cache/index*/{level,shared_cpu_list,size}`, core-type grouping via each cpu's
own `regs/identification/midr_el1` implementer+part fields) covers the real prerequisite, kept in its
own module rather than added to `cpu_info.c`'s `struct cpu_core_info` (a placement concern, not a
counter/PMU one). `wspy --list-affinity` (no privileges needed) discovers domain/thread/core-type ids
up front — mirroring what `scripts/map_cpu_hierarchy.py` maps out for a human, read directly from sysfs
here since a real run can't shell out to a helper script — and is also folded into `--capabilities`'
combined report. The resolved core list is recorded in `--manifest`/`--run-index`'s new
`options.affinity`/`affinity` object (`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` `1.5.0` →
`1.6.0`) so a run's placement is part of its provenance rather than only implicit in how it was
launched. `wspy-run --affinity <spec>` and the web launcher's Run tab "CPU affinity" card (preset
radios including a core-type picker, plus a discovery-backed explicit-CPU checkbox list) both thread
the same spec through to every pass alike; `wspy-queue add --affinity`/job files carry it too.

Detecting x86 hybrid parts (Intel Atom+Core, already tracked as `CORE_INTEL_ATOM`/`CORE_INTEL_CORE` in
`cpu_info.c`'s own per-core vendor field) as a `coretype=` grouping too is a natural follow-up, not
implemented.

### Local LLM (Ollama) narrative-analysis deep-dive (shipped)
Motivation: a run directory already holds validated, structured numbers (CSV, manifest, coverage,
topdown classification) but no prose explaining what they mean to someone who didn't design the counter
groups. A local model (Ollama — no data leaves the machine by default, unlike a hosted API) can turn
already-computed numbers into a readable narrative without wspy taking a dependency on any remote
service.

Design decisions, all shipped:
1. **Classification-by-code, narration-by-model.** Topdown's retire/frontend/backend/bad-speculation
   split (and `wspy-validate`'s PASS/WARN/FAIL checks) are already deterministic — the model is never
   asked to *derive* a bottleneck category, only to explain an already-computed one in prose. The
   model should never be the source of a numeric classification that code could compute instead.
2. **Raw numbers always inlined verbatim** near the top of the rendered prompt, not summarized or
   paraphrased — same "never paraphrase, always show the real thing" rule this codebase applies to
   command-line previews. Lets a reviewer spot-check any claim the model makes against the actual data.
3. **Runs after, and is informed by, `wspy-validate`.** Its PASS/WARN/FAIL lines feed the prompt
   context so the model isn't asked to (re)discover known-bad data itself.
4. **Prompt as a versioned template + a per-run rendered artifact.** The template lives in the repo
   like any other code (`prompts/perf_analysis.tmpl`, `PERF_ANALYSIS_TEMPLATE_VERSION`); each run
   writes its own rendered prompt into the run directory as `aiprompt.txt`.
5. **Prompt customized by which counter groups are actually present**, reusing `web/joblib.py`'s
   `COLUMN_TO_GROUP` mapping — only include a group's interpretation blurb when its columns are
   actually in this run, keeping a small model's limited context focused.
6. **Multi-model sweep + prompt-critique feedback loop.** `--all-models` runs one rendered prompt
   against every installed model; `--critique` asks each model to suggest improvements to the prompt
   template itself (raw input for a human to fold back by hand, never auto-applied).
7. **Curation/report integration.** An `aianalysis.*.txt` output is just another text artifact in the
   run directory, so `collect_run_files()` (the web launcher's curation studio) picks it up
   automatically; it gets a friendly label and an `ai_generated` flag that rides through every studio
   save, surfacing an "AI-generated" badge everywhere the content appears (studio, curated view, all
   three export renderers) plus a "copy analysis into commentary" button a human then edits.
8. **Comparative mode.** `wspy-analyze --rundir <A> --compare-rundir <B>` renders
   `prompts/perf_compare.tmpl` asking what changed between two runs and why, built from both runs'
   already-computed raw counter text/`wspy-validate` results/counter groups. Output is namespaced by a
   slug identifying run B so it never collides with a plain single-run analysis of run A.
9. **Remote-host redaction.** `--redact-command` omits the workload's literal command line for use
   with a non-default `--ollama-host`, since pointing that off-box is a real exfiltration surface
   unlike the local-only default.
10. **No coverage in `make test`/`run_tests.sh`.** Needs a real running daemon and downloaded models;
    `./test_ai_analyze.sh` is its own opt-in smoke test, gated on `command -v ollama`.
11. **Degrades, doesn't fail, when Ollama is unreachable** — same "measured vs unavailable" idiom used
    throughout `coverage.c`/`provenance.c`/`wspy-plot`'s missing-gnuplot handling.

Shape: a new top-level `wspy-analyze` script (Python stdlib, `chmod +x`, no framework; Ollama's HTTP
API is plain JSON over `urllib.request`, no third-party dependency needed), CLI-first per the "CLI-first
model stays primary" stance — a web-UI wrapper is a natural follow-up, not part of the first slice.

### Critical-path / synchronization-latency: full candidate rationale (shipped)
This is the original motivation and per-syscall design-fork reasoning that led to all six shipped
syscall-latency flags (`--tree-futex`, `--tree-io-wait`, `--tree-connect`, `--tree-nanosleep`,
`--tree-wait`, `--tree-poll`) — kept here for the reasoning trail; the shipped mechanism itself is
covered by the concrete-design entries above and by `CLAUDE.md`.

Motivation: hardware counters (topdown/IPC/cache/branch/TLB) characterize how efficiently the CPU
executed while it was running, but say nothing about time spent not running at all — blocked on I/O, a
lock, a child process, or deliberately sleeping. For a workload whose wall time is dominated by waiting
rather than by inefficient execution, no amount of counter analysis explains the bottleneck. The
tree-wide ptrace mechanism `--tree`/`--tree-open` already established was positioned to fill this gap:
single-stepping every syscall entry/exit across the whole process tree already pays the cost of
observing every syscall boundary; the open design question was which of those boundaries were worth
decoding and how to report on them, not whether the mechanism could reach them.

Core mechanism insight: `ptrace_loop()`'s syscall-stop branch already fires once per syscall entry and
once per exit, and `elapsed` (the run-relative timestamp) is computed at every stop already. The
entry→exit delta for a matched syscall *is* that call's latency/blocking duration, purely from
correlating timestamps already being captured.

Candidates, in priority order, all now shipped:
1. `futex` — highest value; uncontended pthread mutex/condvar fast paths never reach the kernel, so
   any observed futex call is itself a contention signal.
2. Blocking I/O (`read`/`pread64`/`recvfrom`/etc.) — entry→exit delta on a blocking fd separates "CPU
   busy" from "blocked on a pipe/socket/slow storage," invisible to a bandwidth counter alone.
3. `connect` — entry→exit delta is literal connection-setup latency.
4. `nanosleep`/`clock_nanosleep` — deliberate idle time; a large share of wall time here rules out a
   hardware explanation for a low-IPC interval outright.
5. `wait4`/`waitpid` on tree nodes that are themselves orchestrators — time here is pure "blocked on a
   child," separating orchestration/serialization overhead from compute on the critical path.
6. `poll`/`ppoll`/`select`/`pselect6`/`epoll_wait`/`epoll_pwait`.

Report-layer payoff: cross-referencing blocking-syscall time against `phase.c`'s IPC-based
warmup/steady/degraded segmentation. A degraded phase that overlaps heavy futex-wait/read-wait time
means the CPU had nothing to do (waiting, not stalling); a degraded phase with no blocking-syscall
activity at all is a genuine hardware stall worth chasing with topdown/cache counters. Combined with
`--tree-schedstat`'s run-delay signal (shipped separately, see above), this became a full three-way
split — see that entry's own "Report-layer payoff" for the completed picture.

Design forks resolved during implementation: log-per-call vs. aggregate-per-pid (resolved: aggregate
uniformly, see the connect/wait/poll/nanosleep entry above for why); argument decoding varies per
syscall (futex's op argument needed bitmask decoding, the rest needed none); generalizing `tree_open`
into a syscall-name→decode-function table was considered and deliberately deferred — six `if` branches
stayed cheaper than the generalization, revisit only if a seventh syscall family is needed.

Caveat, still true and worth remembering when reading any of this data: `ptrace` itself imposes a real
stop-the-world cost on every syscall of the traced process, so absolute latency numbers collected this
way are inflated relative to an untraced run. The *relative* split (fraction of wall time in
futex-wait vs. read-wait vs. on-CPU) stays informative even when absolute numbers are skewed, but this
is an inherent limitation of the ptrace-based mechanism, not clean latency data — a lower-overhead
tracing alternative (`ftrace`/eBPF) remains open, see `INVESTIGATION.md`'s infra tier.

### Concrete design: feature normalization prerequisites (2026-07-21, shipped)
Grounding for 4.2 Tier 1's "Feature normalization prerequisites" item — the first of the two
characterization-track items, and a hard input dependency for the second (the archetype scorecard),
since both need a consistently-shaped input to score against.

`wspy-store`'s `metric_values` table (`store.c`) is long/tall — `(run_id, row_index, tick_time, core,
phase, metric_name, value, is_percent, raw_text)` — with column *identity* coming from the CSV header
text itself, not from which flags produced it. That already gives topdown L1 (`retire`/`frontend`/
`backend`/`speculate`), cache/TLB miss-rate columns, the always-present rusage columns
(`nvcsw`/`nivcsw`/`minflt`/`majflt`, emitted on every run regardless of `counter_mask` — see
`topdown.c`'s base CSV header), and `phase` (`phase.c`, present only when `--interval` + `COUNTER_IPC`
were both active) as queryable rows. `runs` carries `counter_mask`/`counters_requested`/
`counters_measured`/`cpu_vendor`/`elapsed_seconds` alongside.

What's missing is the step from that raw table to a **feature vector**: two runs of the same workload
can have very different available columns (different `--counters=` selection, vendor-dependent raw-event
availability, aggregate vs. `--interval` vs. `--per-core` CSV shape), so nothing downstream can compare
runs directly against `metric_values` without re-solving "which columns exist this time" on every query.
This is the same shape of problem `summary.c`'s `mixed-pmu` verdict and `coverage.c`/`provenance.c`'s
"measured vs unavailable" fields already solve one level down (per-run, not per-feature) — this item is
that idiom applied to a fixed feature vocabulary instead.

Proposed shape:
- **A fixed feature vocabulary**, each entry with an explicit derivation rule and a stated coverage
  requirement (which counter groups/flags it needs) rather than assuming universal availability:
  `ipc_mean`; `retire_pct`/`frontend_pct`/`backend_pct`/`speculate_pct` (topdown L1); per-instruction
  `dcache_miss_rate`/`l2_miss_rate`/`l3_miss_rate`/`tlb_miss_rate`; `branch_mispredict_rate`;
  `fault_rate` = (`minflt`+`majflt`)/`elapsed_seconds`; `ctxswitch_rate` = (`nvcsw`+`nivcsw`)/
  `elapsed_seconds`; `io_rate` (needs `--tree-io`); `phase_stability` = fraction of ticks with
  `phase='steady'` vs `'degraded'` (needs `--interval`); `parallelism_proxy` = cross-core CV of a
  per-core metric (needs `--per-core`).
- **Normalization rules**, decided once here rather than left for each consumer to reinvent: rate
  features divide by `elapsed_seconds` or by instruction count depending on the feature (topdown-style
  ratios are already self-normalizing; raw fault/context-switch counts are not); multi-row shapes
  (`--interval` ticks, `--per-core` rows) collapse via `AVG()` first, mirroring `summary.c`'s existing
  per-run collapse convention rather than inventing a second one; a feature whose required
  columns/groups weren't collected is `NULL` (explicit absence), never zero or silently omitted, so a
  scorer downstream can tell "measured near-zero" from "not measured."
- **Storage**: a new `run_features` table (`run_id, feature_name, value, coverage`) populated by a pass
  over `metric_values`+`runs` — a new `wspy-store` mode or standalone tool, following the same
  `#ifndef TEST_X`/direct-`#include` testability convention every other tool in this codebase uses.
  Versioned independently of `STORE_SCHEMA_VERSION` (its own `FEATURE_SCHEMA_VERSION` or similar), since
  the feature vocabulary/derivation rules will keep evolving after the table shape itself stabilizes —
  same reasoning `TOPDOWN_FORMULA_VERSION` exists separately from `MANIFEST_SCHEMA_VERSION`.

**What actually shipped, and where implementation diverged from this sketch:** landed inside `store.c`
itself (`extract_run_features()`, called automatically from `upsert_run()`) rather than as a standalone
tool — the same file that owns `metric_values` was the natural place, and `--no-feature-extract` gives
the opt-out `--no-manifest-enrich`/`--no-metrics-ingest` already established the pattern for. The version
tag is `FEATURE_SET_VERSION` (a plain `#define`, not a second schema-version-style constant) — value
`"1.0"`. `io_rate` was dropped entirely: `--tree-io`'s `rchar`/`wchar` live in the *tree* output file,
which nothing ingests into the store today, so there was no `metric_values` column to derive it from —
a real scope correction found during implementation, not a deliberate deferral written in ahead of time.
Real CSV column names turned out to differ from this sketch's placeholders in several cases —
`dcache_miss_pct` (not `dcache_miss_rate`) reads `metric_name='L1-dcache miss'`; `icache_miss_pct` reads
`'icache'`; `l2_miss_pct`/`l3_miss_pct` read `'l2miss'`/`'l3miss'`; `branch_mispredict_pct` reads
`'branch miss'`; `itlb_miss_per1k`/`dtlb_miss_per1k` (per-1000-instructions, not a miss rate) read
`'itlb2'`/`'dtlb2'` — all taken directly from `topdown.c`'s own `PRINT_CSV_HEADER` strings rather than
invented, since `metric_values.metric_name` is exactly the literal CSV header text. `phase_stability`
counts `DISTINCT row_index,phase` pairs rather than raw rows, since `metric_values.phase` (like `.core`)
is a per-tick dimension repeated across every metric column collected that tick — grouping on raw rows
would multiply every tick's weight by however many counter columns were selected. Verified end-to-end
against the real `wspy-store` binary (a synthetic run-index record + CSV, not real `wspy` hardware
counters): all designed features round-tripped correctly, features with no source column present
correctly landed `coverage='unavailable'`/`value=NULL` rather than a silent zero, `--no-feature-extract`
correctly suppressed extraction, and a v3-shaped hand-built database migrated cleanly to v4. See
`store.c`'s and `test_store.c`'s own `INVESTIGATION.md`-linked comments for the full up-to-date behavior.

→ Direct input dependency for 4.2 Tier 1's "Archetype scorecard" item (parallelism shape/resource
dominance/control-flow style/runtime stability scoring needs one consistently-shaped feature vector to
score against, not per-rule handling of "what if this run didn't collect topdown"). Also underpins 4.3's
stated goal of using the normalized store for regression detection and clustering — both need the same
fixed, coverage-aware feature set this item established.

### Hierarchical topdown schema (L1→L2→L3) (4.2, shipped)
`print_topdown()`'s already-computed per-vendor L2 breakdown (ucode/fastpath, frontend latency/
bandwidth, backend cpu/memory, speculation branch/pipeline) reaches CSV, not just human text, as 9 new
trailing columns (`contention_pct` + 8 `<parent>_<child>_pct` columns), all expressed as a fraction of
the same contention-adjusted `slots_no_contention` denominator L1 already uses — a real AMD-only
consistency fix, since the pre-existing human-text L2 lines had divided by raw `slots` instead.
`TOPDOWN_FORMULA_VERSION` (`wspy.h`) is recorded in the manifest/run-index (`topdown_formula_version`,
`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.6.0 → 1.7.0), `null` when a run collects no
topdown counters. `wspy-plot`'s `topdown-detail` template charts the 9 new columns on their own, since
the pre-existing `topdown` template only matches the 4 L1 columns and would otherwise dump them into the
generic fallback plot.

**L3 tie-in (fast-follow):** `--topdown-backend`'s own `l1_bound`/`l2_bound`/`l3_bound`/`dram_bound`/
`store_bound` detail (`print_topdown_be()`, a genuinely separate perf counter group with its own
independent `cpu-cycles` reading) also reaches the same `slots_no_contention` denominator, via 5 new
`*_slots_pct` columns and a small cross-group sharing mechanism (`topdown.c`'s
`shared_slots_no_contention`, published by `print_topdown()` and read by `print_topdown_be()` — safe
only because `setup_counter_groups()`'s check order guarantees the former always runs first for the same
row; see `CLAUDE.md`'s `topdown.c` entry for the exact mechanism). The original 5 cpu-cycles-normalized
columns are untouched for backward compatibility; the new columns are explicitly documented as *not*
guaranteed to sum to `backend_memory_pct` (independent measurement chains, same caveat as the L1 sanity
check). `wspy-plot`'s `memory-bound-detail` template covers the 5 new columns. Also fixed two previously
unguarded unsigned-subtraction underflow risks in `print_topdown_be()`'s own `l2_bound`/`l3_bound`
computation (same bug class as the AMD L2-split fix), via `safe_sub()`. Intel/ARM only — AMD has no
`COUNTER_TOPDOWN_BE` raw events, so `print_topdown_be()` is never called there.

### Zen-family preset packs (4.2, shipped)
`wspy-run`'s `zen-portable` (`quick`+`ibs-basic`) and `zen4plus-deep` (`deep-cpu`+`ibs-memory-deep`)
builtin profiles are the first defined purely as a composition of other builtin profiles
(`load_profiles()`, the same machinery that resolves a user-supplied comma list) rather than
hand-written flag strings. `zen-portable` avoids `--power` (AMD Family 19h+ only) and IBS `l3missonly`
filtering (Zen5-only) so it runs warning-free across the whole Zen family; `zen4plus-deep` assumes
Family 19h+ hardware where both are real, with `l3missonly` degrading gracefully (not failing) on Zen4.
Verified end-to-end on real Zen5 hardware.

### Per-core energy (`power_core`) support (4.2, shipped)
`--power --per-core` opens a real `power_core`/`energy-core` event per representative CPU
(`power_core`'s own sysfs `cpumask` names one representative logical CPU per physical core — e.g. the
16 even-numbered CPUs out of 32 on a real Zen5/SMT2 host) and adds `core_joules`/`core_watts` trailing
columns to `--per-core`'s row shape, alongside (not replacing) the existing systemwide
`pkg_joules`/`pkg_watts`. Every per-core-eligible CPU gets a structurally identical group; a CPU that
isn't one of `power_core`'s representative CPUs gets a placeholder counter marked with a new sentinel
(`POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE`, `power.h`) that `setup_counters()` skips before even
attempting `perf_event_open()` — genuinely never-attempted, not "requested but failed," so it doesn't
skew `counters_requested`/`counters_measured` or `preflight.c`'s budget estimate. `--power` alone (no
`--per-core`) is unaffected.

**Confirmed on real Zen5 hardware (root):** representative CPUs showed real nonzero values correlating
with actual scheduling activity, sibling CPUs read exactly `0.000`, `pkg_joules`/`pkg_watts` stayed
unchanged across every row, and coverage counts confirmed exactly 16 representative attempts (not 32).
Also confirmed, a genuine finding rather than a units bug: summed per-core energy across representative
CPUs was roughly 16× smaller than package energy for the same window — core-domain energy is a real,
meaningfully smaller subset of package energy (excludes uncore/IO/memory-controller/L3/idle-package
power). See `CLAUDE.md`'s `power.c` entry for the full mechanism.

### ROCm SMI + sysfs GPU fusion layer, and GPU telemetry provenance (4.2, shipped)
`--gpu-metrics` now merges `amd_sysfs.c` and `amd_smi.c` into one fused column set instead of requiring
a separate `--gpu-smi` for VRAM — sysfs supplies temp/activity/power/freq (the actively-used path;
`amd_smi.c` is "legacy"), SMI fills in temp/activity only when sysfs's reading failed, and SMI remains
the sole VRAM source. New `gpu_temp_source`/`gpu_activity_source` columns record which backend actually
supplied each value (power/freq/VRAM each have exactly one possible source, so they keep this
codebase's usual zero-means-unmeasured convention instead of a redundant flag). The precedence logic
(`gpu_fusion.c`'s `gpu_fusion_combine()`) is a pure, unit-tested function (`test_gpu_fusion.c`)
separated from the hardware-dependent glue, mirroring `power.c`/`ibs.c`'s own testability split. Also
collapsed 4 previously hand-duplicated GPU-metrics print sites (CSV header, per-core CSV, aggregate
CSV, human output) into one shared `print_gpu_metrics()`, closing off the exact column-ordering bug
class the `--gpu-smi --interval` fix (below) already ran into once.

`struct manifest_gpu_info` (`manifest.h`) adds an `options.gpu` object to the manifest/run-index
(`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.7.0 → 1.8.0) — which `--gpu-*` flag(s) were
requested, the resolved AMD/NVIDIA device index, and whether each backend
(`amd_sysfs`/`amd_smi`/`nvidia`) actually produced valid data on the run's last read. Deliberately
provenance-only, not a duplicate of the measured values (same role `counters_requested`/
`counters_measured` play for perf counters); device-index fields are gated on the `requested` flags
rather than the index's sign, so a zero-initialized struct reports `null` rather than looking like
"device 0".

**Verified live on real AMD GPU hardware:** SMI's `gpu_metrics_info` call failed independently of
sysfs, and the fused row still correctly reported `sysfs`/`sysfs` sources plus a real VRAM reading from
SMI's separate (successful) VRAM call; the same combination round-tripped correctly into both the
manifest and run-index.

### System-wide disk I/O and memory pressure stats (4.2, shipped)
Two new `system.c` bits, both default-on in `system_mask` (only printed with `--system`/`-s`, no
separate CLI flag):

- **`SYSTEM_DISK`** reports per-block-device read/write bytes and time-in-I/O deltas from
  `/sys/block/<dev>/stat` (devices enumerated via `/proc/partitions`, filtered to whole disks) as three
  new columns per device (`disk <dev> read,disk <dev> write,disk <dev> time,`) — the same per-device
  breakdown `SYSTEM_NETWORK` already gives for `/proc/net/dev`. `wspy-plot` gained matching
  `disk-io`/`disk-time` fallback plots (kept separate since bytes and milliseconds don't share a useful
  scale). Device enumeration excludes `loop`/`ram`/`zram` names unconditionally
  (`is_virtual_disk_device()`) — found via live testing: a real dev host's 35 snap-package loop devices
  pushed a realistic multi-flag `--interval` CSV to 137 columns, past `plot.c`'s `MAX_CSV_FIELDS` (128)
  cap, silently truncating header parsing and dropping the `topdown-detail` plot with no error;
  filtering brought the same CSV to 35 columns and restored correct plotting. Loop devices' own
  `/sys/block/loopN/stat` also never reflects real backing-file I/O, so this is the correct default
  independent of the column-budget concern. Verified live: real `dd`-driven writes to the root
  filesystem tracked actual bytes/I/O-time tick-for-tick, while a tmpfs-backed write correctly showed
  zero disk activity.
- **`SYSTEM_MEM`** reports 6 fixed `/proc/meminfo` fields — `MemFree`/`Cached`/`Dirty`/`Writeback`/
  `SwapFree`/`Committed_AS` — as `mem_free_mb,mem_cached_mb,mem_dirty_mb,mem_writeback_mb,
  swap_free_mb,committed_as_mb,` columns (kB converted to MB at print time). Distinct from
  `--tree-vmsize`'s per-process snapshot — this is host-wide. Absolute point-in-time gauges, not deltas.
  `wspy-plot` gained a real `memory-pressure` template (not a fallback bucket, since these 6 columns are
  fixed names sharing one MB scale). Verified live: a Python process touching a 300MB buffer moved
  `mem_free_mb` measurably across `--interval` ticks on a 62GB host.

### `proctree` JSON export + interactive viewer + run-to-run diff (4.2, shipped)
`proctree --json <tree-file>` emits one JSON document (per-`comm` summary + full process tree, every
field unconditional rather than gated by the text-mode `-M`/`-N`/`-P`/`-U`/`-X`/etc. toggles) instead of
the text tree/summary — the interchange format both the new web viewer and `--diff` mode consume,
versioned via `PROCTREE_JSON_SCHEMA_VERSION` (see `doc/ARTIFACT_CONTRACT.md`'s "Tree JSON export").
`proctree --diff [--json] <a.json> <b.json>` matches subtrees structurally (ancestor-`comm`-path,
disambiguated by sibling occurrence order, since pids never correspond across two separate runs),
reporting `added`/`removed`/`changed`/`same` per node plus a `comm`-keyed `summary_diff` overview; exits
1 if any difference was found, 0 if the trees matched exactly.

`web/server.py` gained an on-demand `GET /api/tree-json/<suite>/<benchmark>/<run_id>` (shells out to
`proctree --json`, no artifact written to disk) feeding a client-side-rendered
`/tree-viewer/<suite>/<benchmark>/<run_id>` page (`web/static/proctree_viewer.js`: collapsible tree,
search/filter by `comm`/pid, auto-detected column toggles for whichever `--tree-*` annotations this run
collected), linked from every report that has a `process.tree.txt`. `GET /tree-diff?r=...&r=...` reuses
the homepage's/`/history`'s run-selection checkboxes (a second "Tree diff selected" button) to drive the
same viewer against `GET /api/tree-diff-json`'s merged diff tree, rendering per-node
added/removed/changed/same badges.

Graphviz export for an already-filtered small subtree remains a possible optional secondary output, not
implemented — the interactive viewer is the main way to view a whole run's tree now.

### `wspy-core-report`: per-core diagnostics, and AMD Zen5/Zen5c core detection (4.2, shipped)
A new standalone binary (`core_report.c`) reports cross-core min/max/mean/stddev/coefficient-of-
variation for every metric column in an existing `--per-core --csv` file, naming the "hot" (max) and
"cold" (min) core by index — a post-hoc report over an already-collected artifact (matching
`wspy-validate`/`wspy-plot`'s own pattern), not a live collection-time feature. When a host's cores
aren't all the same type, an additional breakdown groups the same stats by core class. Must be run on
the same host that collected the CSV (or one with identical topology) — core classes are re-detected
fresh via `inventory_cpu()`, there's no per-core class column in the CSV itself. The class-grouping
logic (`gather_core_values()`/`distinct_classes_present()`) takes a plain class-per-core-index array
rather than reading `cpu_info` directly, so `test_core_report.c` exercises it against a synthetic
heterogeneous-host assignment without needing real hardware. `--csv` output:
`metric,scope,scope_value,n,min,min_core,max,max_core,mean,stddev,cv_percent`; `--metric <name>` filters
columns. (Process/thread migration diagnostics — did a process's threads move between cores — was split
out into its own 4.4 backlog entry, since it needs new instrumentation, not just new analysis of data
already collected.)

**AMD Zen5/Zen5c core detection** feeds this directly: `cpu_info.c` previously classified every
family-0x1a AMD core as `CORE_AMD_ZEN5` uniformly, unable to tell full Zen5 cores apart from the
physically compact Zen5c cores on hybrid parts (e.g. Ryzen AI 300 "Strix Point") — cpuid family/model
alone can't distinguish them. `resolve_amd_zen5_dense_cores()` clusters on per-core `cpufreq` max
instead (any family-0x1a core whose max frequency reads below the highest seen among its siblings is
reclassified `CORE_AMD_ZEN5C`), mirroring the heuristic `scripts/map_cpu_hierarchy.py` already used;
degrades to leaving every core `CORE_AMD_ZEN5` when frequency data isn't readable. Fixed two consumers
that would otherwise silently mishandle the new class: `topdown.c`'s slots-per-cycle formula (folded
into the same 8-wide branch as full Zen5) and `wspy.c`'s `core_is_per_core_eligible()` (without it,
Zen5c cores would silently collect zero per-core counters). **Verified live:** `./cpu_info`'s
Zen5/Zen5c split matched `map_cpu_hierarchy.py` exactly, CPU-for-CPU, on a Ryzen AI 9 HX 370 (4 Zen5 +
8 Zen5c cores).

A new "Per-core class comparison" section on the web launcher's Validate tab runs `wspy-core-report`
against a discovered or pasted `--per-core` CSV (gated on a real `core` header column), with
`--metric`/`--csv` options exposed; report pages gained a "Compare cores" link next to any `--per-core`
CSV artifact that lands pre-filled on that section.

### cgroup identity, limits, and throttling in the manifest/run-index (4.2, shipped)
A new module (`cgroup.c`/`cgroup.h`) adds a top-level `"cgroup"` object to the manifest/run-index
(`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` 1.8.0 → 1.9.0) — cgroup v2 identity (the
unified-hierarchy path from `/proc/self/cgroup`'s `"0::"` line), resource limits (`cpu.max`'s
quota/period, `cpu.weight`, `memory.max`/`memory.high`), and CPU-throttling stats (`cpu.stat`'s
`nr_periods`/`nr_throttled`/`throttled_usec`) — needed for fair comparison of runs in containerized
environments, where a `cpu.max` quota or an ongoing throttling episode can explain a degraded result
that has nothing to do with the workload itself. cgroup v2 (unified hierarchy) only; a pure cgroup v1
host degrades the whole thing to unavailable, and every limit field degrades independently (a real,
confirmed-live case: a desktop terminal-emulator's leaf cgroup had `memory.max`/`memory.high` but no
`cpu.max`/`cpu.weight`/throttling fields at all, since the cpu controller wasn't enabled on it).
Identity/limits are read once; `cpu.stat`'s cumulative counters are read twice — once near workload
launch, once at manifest-write time — and the *delta* is what's reported, mirroring `read_counters()`'s
before/after idiom for perf counters. `cgroup_state`/`cgroup_throttle_baseline` are module-owned
run-lifetime state, mirroring `affinity.h`'s precedent. Tested against fake
`/proc/self/cgroup`+`/sys/fs/cgroup` fixtures (`test_cgroup.c`), including a regression fixture for the
real no-cpu-controller case found during development.

### Archetype scorecard (4.2, shipped)
`wspy-archetype` (`archetype.c`) classifies a run along four axes scored from `run_features` —
`resource_dominance` (the headline axis: `compute-bound`/`frontend-bound`/`memory-bound`/
`speculation-bound`, ranked from topdown L1 percentages, with a top-2 alternative and a margin-based
confidence level) plus three simpler supporting tags (`parallelism_shape`, `control_flow_style`,
`runtime_stability`, each `unknown` when their source feature wasn't collected). No taxonomy/threshold/
confidence-formula spec existed anywhere in this repo before this item — every rule is a from-scratch v1
design, confirmed with the user as 4 independent axes (not a single composite cross-product label)
specifically because `resource_dominance` is the one axis with a natural ranked percentage to define
"top-2 alternatives" against.

Real prior art grounded the design: a 2024 clustering analysis (~240 Phoronix tests + 23 SPEC CPU2017
benchmarks, k-means into 30 clusters, see `mvermeulen.org/perf/2024/06/08/clustering/`) used exactly
`retire`/`frontend`/`backend`/`speculation` as its core clustering metrics, directly validating the
`resource_dominance` approach, and separately used `on_cpu` (cores actively used) as a clustering
dimension distinct from load balance — motivating the new `active_core_count` run_feature (`store.c`,
`FEATURE_SET_VERSION` 1.0 → 1.1, alongside `smt_contention_pct`) that `parallelism_proxy` alone didn't
capture.

Two CLI modes mirror `summary.c`'s bulk/`--trace` duality: default scores every run matching
`--command`/`--hostname` filters (one row per run, CSV or human table; deliberately excludes runs with
zero `run_features` rows at all rather than showing them as all-`unknown`); `--run <hostname>:<run_id>`
prints one detailed `key=value` scorecard. Designed for extensibility: a new simple threshold-based axis
is one rule-table addition plus one `classify_simple_axis()` call site, no changes needed elsewhere. See
`CLAUDE.md`'s `archetype.c` entry for the full design.

### Compare-view curation, Phase 1 (4.2, shipped)
`GET /compare` gained an optional annotation layer — `compare.json` (`COMPARE_SCHEMA_VERSION`,
`web/server.py`), the first cross-*run* state file in this codebase (`curation.json` is strictly
per-run; `run_index.jsonl`/`store.db` are flat per-run logs with no relationship between specific runs),
stored at `<output_root>/compares/<id>.json` where `id` is a hash of the sorted, deduped run-key set
(order-independent, exact-match — a different run set gets a different id and starts uncurated, no
fuzzy reattachment). Scoped to Phase 1 only: one `overview_note` for the comparison as a whole plus one
commentary note per filename row, reusing today's exact filename-based row identity — no cross-run
alignment of differently-named files yet. A separate `GET`/`POST /compare/curate?r=...&r=...` edit page
mirrors the studio/report split rather than an inline-edit toggle. Covered by
`web/test_compare_curation.py`. See `CLAUDE.md`'s "Compare-view curation" entry for the full design.

**Deferred out of Phase 1, not dropped:** manually aligning two differently-named files from different
runs as "the same measurement" (e.g. two runs that used different profiles/passes and so named
conceptually-equivalent output differently) needs a real new alignment concept (a group/label spanning a
per-run file mapping), not an extension of the current commentary layer. Worth revisiting once real
multi-profile comparisons actually need it.

### Release-prep checklist/script and doc/version consistency check (4.2, shipped)
`scripts/release_prep.sh` captures the v4.0/v4.1/v4.1.1 release process as a repeatable script:
pre-flight checks, a merged-PR/release-label audit, version bump, stale-version-reference grep, the
full test matrix, a release-notes draft, and doc bookkeeping reminders, ending with print-only
tag/push/publish commands it never executes itself. Not hypothetical: the label audit found real, live
drift the first time it ran — PR #124 was missing its `v4.2` label, now fixed. Two real bugs found and
fixed while building it, worth recording since they're non-obvious: (1) `gh pr list --search
"merged:>=<date>"` is imprecise at same-day tag/PR boundaries (`v4.1.1`'s own tagged commit's
author-date collided with its own PR's merge time and got double-counted) — fixed by using `git log
<tag>..HEAD --merges` for exact PR ancestry instead; (2) `gh pr edit --add-label` fails outright on this
repo with a "Projects (classic) is being deprecated" GraphQL error before the label is ever applied —
fixed by using `gh api repos/{owner}/{repo}/issues/<n>/labels` instead. See `CLAUDE.md`'s
`scripts/release_prep.sh` entry for the full phase-by-phase design.

`tests/doc_version_check.sh` (wired into `run_tests.sh`, once, not per GPU-build axis) is a grep-based
doc/version drift check — and wasn't a hypothetical exercise: running it for the first time found the
exact class of drift the backlog item described, live in the repo. `doc/ARTIFACT_CONTRACT.md`'s
manifest/run-index JSON examples *and* its own separate "Current versions as of this writing" prose
summary had each independently drifted to a stale `1.5.0` against the real `1.9.0`
`MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION`, and `README.md` had no section at all for
`wspy-core-report`/`wspy-archetype` despite both being built by `make all`. All three fixed in the same
change as adding the script. See `CLAUDE.md`'s `tests/doc_version_check.sh` entry for the full design.

### Size `wspy-run`'s `--tree` pass timeout from an actual run-time estimate (4.2, shipped)
`estimate_tree_pass_timeouts()` (`wspy-run`) sizes the timeout generically for any pass whose flags use
`--tree` — not hardcoded to `tree-heavy` by name, so `gpu-compute` (which also uses `--tree`, previously
with no timeout at all) now gets one too. Reuses `web/joblib.py`'s already-validated Phoronix
runtime-estimation logic (`estimate_phoronix_workload_seconds()`) via a new small CLI wrapper,
`scripts/estimate_tree_timeout.py`, rather than reimplementing the same `phoronix-test-suite info` text
parsing a second time in bash.

Design settled through direct discussion, correcting two assumptions from the original backlog line: the
timeout's real purpose is a ptrace-hang backstop, not primarily a data-volume cap — losing a key ptrace
event for a traced process can leave `wspy` hung waiting to clean up — and real Phoronix runs
legitimately exceed the historical 3600s constant, so the floor stays at exactly that constant (this can
only raise the cap for a workload confirmed to legitimately need longer, never lower it) with a generous
6-hour ceiling as a true hang backstop, not a normal-operation limit. `phoronix-test-suite info <test>`'s
own per-test estimate is treated as a floor, not a target, and multiplied up more aggressively for
`batch-run` specifically (`BATCH_RUN_MULTIPLIER=5.0` vs. `RUN_MULTIPLIER=2.0`) — confirmed live against a
real installed test profile (`blender-1.2.1/test-definition.xml`) that a full `batch-run` sweep runs
every configured option combination (5 blend files × 2 compute backends = 10 full renders for that one
test), something `info`'s single-test estimate doesn't account for. Falls back to the exact historical
`3600` whenever no estimate can be derived — never blocks a run over a missing/failed estimate.

### Profile cookbook + interpretation playbook, and reproducibility bundle export (4.2, shipped)
`doc/PROFILE_COOKBOOK.md` is a reading guide for `summary.c`'s `verdict` column, `archetype.c`'s
`confidence`, `phase.c`'s `phase` output, and the two real comparability mechanisms (`mixed-pmu`,
environment `--group-by`) — what each signal means and what to do when it fires, not a restatement of
the artifact format (`doc/ARTIFACT_CONTRACT.md` already owns that). Every numeric example is real
captured output from a small synthetic 4-run dataset built specifically to trigger a genuine
`WARN:noisy,mixed-pmu` bucket and a real low-confidence `wspy-archetype` classification, rather than
invented figures. Also directly resolves the backlog line's ambiguous "cluster" wording: states plainly
that statistical clustering is **not** shipped yet (still its own distinct 4.3 item), rather than
describing a feature that doesn't exist.

`wspy-bundle` (new, stdlib-only Python) bundles one run directory's manifest(s), raw per-pass output,
and derived artifacts (plots, summary, curation, AI narrative) into a single checksummed `.tar.gz`, so a
run can be archived or handed off without access to the machine's live output-root/`store.db`. The
actual enumeration/bundling logic (`collect_run_files()`, `build_reproducibility_bundle()`) lives in
`web/joblib.py`, shared with `web/server.py`'s own "Download reproducibility bundle" report-page link —
same "one shared implementation, two front ends" pattern the job queue established. A
`bundle_manifest.json` index at the tar root classifies every file as `manifest`/`raw`/`derived` and
records its sha256. This closed out 4.2 entirely.

### wspy-analyze: AMD IBS counting-mode CSV data reaches the AI narrative prompt (4.3, shipped)
IBS counting mode (`--ibs-basic`/`--ibs-memory-deep`) writes `ibs_fetch`/`ibs_op`/etc. to a CSV time
series (`ibs.csv`), which `collect_raw_text()` never read (only `summary.txt`/`*.txt` reached the
prompt) and which had no entry at all in `web/joblib.py`'s `COLUMN_TO_GROUP` — so on the most common IBS
profiles, the AI narrative never saw an IBS value or even knew the group existed. Found by inspecting a
real published report (`706.stockfish_r-gcc_O3-base`) whose narrative said nothing about IBS at all.

Fix: a new streaming per-column summarizer (`summarize_csv()`, Welford's algorithm, O(1) memory
regardless of row count) renders min/max/mean/stddev/count into a new `{{CSV_SUMMARIES}}` prompt
section, deliberately bounded by column count rather than row count so an arbitrarily long
`--interval`/`--per-core` CSV can never blow up the prompt; a `--csv-summary-max-bytes` cap (default
5MB) skips summarizing a pathologically large file rather than reading one in full.
`PERF_ANALYSIS_TEMPLATE_VERSION`/`PERF_COMPARE_TEMPLATE_VERSION` bumped (2.2→2.3, 1.1→1.2). Verified
against `test_ai_analyze.sh`'s extended fixtures (both single-run and `--compare-rundir` modes, plus the
size-cap skip path) and a real live Ollama call (`qwen2.5-coder:3b`) that correctly reported
`groups=ibs,topdown`. First of several possible AI-narrative-analysis improvements raised in the
originating discussion (multimodal/vision analysis of `wspy-plot`'s PNG output, `phase.c`-derived trend
summaries, `process.tree` critical-path narration) — none of the others scoped or started.

### wspy-testpoint: run selection, aggregation, and curated README rendering for a test point (4.3, shipped)
Full design scoped 2026-08-01 for the test-point-level curated performance-summary README (`doc/
REPORT_HIERARCHY.md`'s `<test-point>/<machine>/README.md` level), then built as four pieces across PRs
#183-186. The author's own observation drove the design: a test point's linked runs are frequently *not*
interchangeable repeats — motivating a role-assignment step between run enumeration and aggregation, not
an implicit "use every linked run" default.

**Design.** Run enumeration needed no new tracking: suites with no option axis already land every run at
`wspy-run`'s unified `<outdir>/<suite>/<benchmark>/<run-id>/` layout, and Phoronix option-combination
test points use the existing `runs/<run-id>/` symlinks. The real gap was **why** a test point accumulates
multiple runs — at least three distinct reasons needing different treatment: (1) a redo after a problem
(bad run should be excluded, not averaged in), (2) diving into more detail with a different collection
scope (a supplementary source of specific artifacts, not another statistical sample), (3) genuinely
repeated runs for statistical power (the one case naive aggregation handles correctly). This produced
four roles — **stats-pool**, **supplementary**, **excluded/superseded**, **primary** — with a sensible,
always-overridable default (command-text match + no `wspy-validate` FAIL → stats-pool; FAIL → excluded;
differing pass/profile set → supplementary), mirroring 4.1's curation studio's own "generate a default,
human edits it, never silently regenerate over an edit" idiom applied one level up, to *which runs* feed
a report rather than *which artifacts within one run*. Template/storage reused the curation studio's
existing block-list-plus-commentary JSON directly (not a second format), and the studio's
source-pointer-vs-commentary separation (`source_file` regenerates, `commentary` never does) was applied
to a second thing: role-assignment decisions themselves, so a later re-curate (a new run landing, a
stats-pool run turning out bad) never silently overwrites a human's explicit call.

**`wspy-testpoint select-runs` (PR #183).** Implements role assignment: enumerates a `(suite, benchmark,
machine)` combination's runs (reusing `web/server.py`'s `load_run_history_entry()`,
`web/joblib.py`'s Phoronix test-point identity), defaults roles from status + pass-set majority, and
persists `runs.json` under the report-root, committed locally via a new shared `web/report_root.py`
(the report-root git plumbing extracted out of `wspy-publish`). CLI-only, no web UI yet — matching
`wspy-analyze`'s own "CLI-first, web wrapper is a natural follow-up" precedent. Two bugs fixed before the
formal test suite existed: a `--primary` override silently reverting on the next plain re-run (needed
its own `primary_human_set` flag, independent of any single run's own `human_set`), and a `generated_at`
timestamp that defeated the idempotent-commit no-op path (removed — git history already tracks when a
file last meaningfully changed). `tests/testpoint_smoke.sh` (a local bare git repo standing in for the
report-root remote, no network/hardware dependency) covers both.

**`wspy-testpoint aggregate` (PR #184).** Turns a resolved `stats-pool` run set into real statistics. The
original plan to reuse `wspy-summary --command --hostname` turned out to be a real correctness bug: text
matching can't distinguish a redo sharing byte-identical command text from the runs it's redoing —
exactly the case role-assignment exists to separate. Fixed with a `summary.c` `--run-id
<hostname>:<run_id>` filter (repeatable, `TEMP TABLE`/`EXISTS`-based, applied inside `summarize()` itself
so every caller gets it for free). Verified live against a real `wspy-store` database: without the
filter, a redo's outlier dragged a bucket's mean from 1.85 to 1.4 with a spurious `WARN:noisy`; with
`--run-id` naming just the real two runs, `mean=1.85` as expected. `aggregate` also read-only-prechecks
which `stats-pool` runs are actually present in the target store, warning by name on any missing rather
than silently dropping or auto-ingesting them.

**`wspy-testpoint render` (PR #185).** Turns aggregated statistics into a curated `README.md`. "Template
rendering" and "README writing" collapsed into one piece once researched: `web/server.py`'s curation-
studio block model (`new_block()`, `load_curation()`/`save_curation()`, `export_block_content()`,
`render_export_markdown()`) already had exactly the separation needed, reused **unmodified** — each
counter-group section is written to a generated file under `sections/`, referenced by an ordinary
`"artifact"` block, so a block whose section file already existed keeps its human-edited
title/depth/commentary/position across a re-render. A `"freeform"` block (commentary-only, no backing
file) was considered and rejected — it would conflate "the data" with "the human's notes." Two bugs
fixed before the formal tests existed: `render` never called `ensure_report_root()` (unlike
`select-runs`), so a fresh report-root clone failed with a raw git error; and the idempotent-commit check
string-matched `git commit`'s own "nothing to commit" message, whose exact wording depends on unrelated
repo state (an untracked file elsewhere in the clone produces different phrasing) —
`commit_paths()` (now shared by every subcommand) checks the git index directly instead, and scopes each
commit to exactly its own paths rather than a bare `git commit` that could sweep up unrelated staged
changes.

**`wspy-testpoint render`: archetype cross-run stability (PR #186).** Closes the one piece deferred from
both #184 and #185: `wspy-archetype`'s four classification axes were computed per run with no cross-run
comparison, so a workload's characterization could silently drift across its history with nothing to
show it. Simpler than `aggregate`'s own fix — `wspy-archetype --run <hostname>:<run_id>` already names
one exact run per call, so none of the bulk-filtering machinery was needed. `render` now calls it once
per stats-pool run and adds a "Workload characterization" section reporting whether `resource_dominance`
(the headline axis) agrees across the pool or diverges. Verified live: two runs both classifying
compute-bound render "Consistent"; re-ingesting one run's data to a memory-bound shape renders "Diverges"
with the correct per-run breakdown.

`wspy-testpoint select-runs`/`aggregate`/`render` together form the working end-to-end pipeline this
item set out to build; only remaining open scope is pulling specific artifacts (not just a name/reason
listing) from `supplementary`-role runs.

### WordPress REST publishing pipeline: auth, page/media primitives, publish button (4.3, shipped)
Built as eight ordered sub-steps (2026-07-27 through 2026-08-01) toward Tier 3 item 2's "static-site
publishing pipeline" goal, each independently useful/testable before the next. Real publish target: a
new WordPress site at `mvermeulen.org/workload`, parallel to (not replacing) the author's existing
hand-curated `mvermeulen.org/perf/workloads/`. Reviewing that existing site as prior art first confirmed
`doc/REPORT_HIERARCHY.md`'s levels 1-3 already work by hand (one wide reference-matrix suite page linking
to per-benchmark pages), and that its 4th level (`<test-point>/<machine>/`) had no precedent — settling
the URL/slug scheme before automating was real design work, not just plumbing.

1. **Site stood up (2026-07-30).** `https://www.mvermeulen.org/workload` live, `wp-json/` confirms
   `authentication.application-passwords`; two hand-created top-level suite stub pages (`cpu2026`,
   `phoronix`).
2. **Dedicated service account (2026-07-30).** A low-privilege `wspy` account (Contributor + custom
   capabilities: `edit_pages`/`edit_published_pages`/`publish_pages`/`upload_files`), never the author's
   own admin login, authenticated via a WordPress Application Password rather than a real login password.
3. **Minimal REST client (2026-07-31).** `web/wp_client.py` (stdlib `urllib` only) plus a new top-level
   `wspy-publish` tool (`configure`/`test-connection`). One real obstacle: IONOS (the site's host) drops
   the `Authorization` header before PHP ever sees it, defeating every standard server-side recovery
   trick — worked around by also sending the credential under a custom `X-WSPY-Authorization` header,
   recovered by a small installed plugin (`scripts/wp-auth-bridge.php`). See "Non-obvious implementation
   traps" below for the full diagnosis. Credentials live in `~/.config/wspy/publish.json` (mode 600,
   written via `getpass`). Same tool also clone-or-verifies the report-root git repo
   (`github.com/mvermeulen/workload`), local-commit-only.
4. **Idempotent create-or-update, keyed on test-point+machine (2026-07-30).** WordPress's native page
   parent/child nesting maps one page per `doc/REPORT_HIERARCHY.md` level; the lookup key is `(slug,
   parent)` (WordPress's own uniqueness rule for hierarchical post types), walked top-down one level at a
   time by `wp_client.find_or_create_page_path()` — not a custom `meta` field, which would need an
   Administrator-only `register_post_meta(..., show_in_rest=>true)` plugin install for no real benefit
   over a stock slug+parent lookup. This resolved *lookup* only, not the full create-or-update *content*
   flow — merging generated content against a page a human has since hand-edited remains open (see
   INVESTIGATION.md's current Tier 3 item 2 for that and other still-open scope).
5. **Draft-first, not direct-publish (2026-08-01).** `wp_client.get_page()`/`publish_page()` plus a
   `wspy-publish publish-page` subcommand: find-or-create-as-draft and content update happen immediately,
   the `status=publish` flip needs an explicit `--publish` flag, so dry-running against the real site
   never publishes by accident. Unit-tested (`web/test_wp_client.py`, HTTP layer mocked).
6. **Media upload endpoint (2026-08-01).** `wp_client.upload_media()` — WordPress's raw-binary upload
   method (`Content-Type` + `Content-Disposition: attachment`, body = raw bytes) rather than
   `multipart/form-data`, so no library beyond stdlib `mimetypes` is needed. `update_media()` sets
   `alt_text`/`title`/`caption` as a separate follow-up POST. New `wspy-publish upload-media` subcommand.
   Verified against the real site (a throwaway test PNG).
7. **Reuse `render_export_wordpress()`'s Gutenberg block markup as the publish payload (2026-08-01).**
   `wspy-publish` imports `web/server.py` directly (guarded by its own `__main__` check) and calls the
   same `_export_data()`/`render_export_wordpress()` the web UI's export tab already uses, rather than
   generating block markup a second way. One real gap surfaced: the exporter always baked in this
   server's own `/files/...` URL for a curated image block, which only resolves while that server keeps
   running — fixed with an optional `image_url` resolver parameter (default `None`, existing export tab
   unaffected) that `publish-page --from-rundir` uses to substitute each image block's just-uploaded
   WordPress media URL. Verified against the real site end to end: a real run directory's plot PNGs,
   published via `publish-page --from-rundir`, correctly carry `mvermeulen.org` media URLs, not
   `127.0.0.1`.
8. **"Publish to WordPress" button in the web UI (2026-08-01).** The Export tab's `?format=wordpress`
   view gained a panel (slug/parent-id/title, a "Publish immediately" checkbox defaulting off) posting to
   a new `/publish/<suite>/<benchmark>/<run_id>` route, sharing `render_wordpress_content_for_rundir()`
   and a new `wp_client.publish_page_content()` with the CLI so the two paths can't drift into different
   find-or-create behavior. No credential entry point in the web form — reads the config `wspy-publish
   configure` already wrote. Caught a real bug in sub-step 7's original merge: a *failed* image upload
   left that filename out of the `{filename: url}` map, and `image_url=image_urls.get` then fed a literal
   `None` as the image `src` instead of falling back to the local URL — `export_block_content()` now
   treats a falsy `image_url()` return as "no override," matching `dict.get()`'s own missing-key default.
   Verified end to end through the actual running server via `curl`-submitted form POST.

Net result: full REST auth/page/media primitives, a CLI (`wspy-publish`), and a per-report web-UI publish
button all exist and are verified against the real site. What Tier 3 item 2 originally asked for — an
automated pipeline that walks the whole store and publishes/updates suite- and cross-suite-level rollup
pages, not just one report at a time on a human's click — was never built; only the primitives it would
be built from are. `publish_page_content()`'s find-or-create also turned out to use a flat `(slug,
parent)` rather than calling `find_or_create_page_path()`'s hierarchy walk, so today every page still
publishes at WordPress root regardless (tracked as its own Tier 3 backlog item, since it affects every
publish path, not just this one).

*Correction (2026-08-03): both follow-up gaps in the previous paragraph have since closed, in later
work not otherwise write-up'd here — `wp_client.publish_page_at_path()`/`wspy-publish publish-path`
(shipped alongside PR #190) is the hierarchy-walking primitive that was missing, and PR #197 added
idempotent content-merge protection (`WPContentDriftError`, fingerprint tracking in
`~/.config/wspy/publish_state.json`). See `INVESTIGATION.md`'s "What shipped in 4.3" for both. Left the
original paragraph above unedited per this file's own "don't edit history" convention; this note exists
so a reader doesn't take it as still-current.*

### Symbol-level profiling: `--symbol-sample`/`wspy-symbolize`/web UI drill-down (4.3, shipped)
Design and shipped implementation for the symbol-level profiling feature, scoped to the
`--target`-matched-process drill-down use case (2026-07-29/30) — not `perf record`/`perf report`'s
whole-system capture-then-drill-down model, which `perf` itself already covers.

**Capture + parsing (2026-07-30).** `perf_ring.c`/`perf_ring.h` (generic mmap ring-buffer plumbing
factored out of `ibs_sample.c`, which now sits on top of it unchanged in externally observable
behavior), `symbol_sample.c`/`symbol_sample.h` (the `PERF_SAMPLE_IP` capture module below), the
`--symbol-sample`/`--symbol-sample-event=<event>` CLI flags, the `PTRACE_EVENT_EXEC`/`PTRACE_EVENT_EXIT`
wiring in `topdown.c` producing real `targetsample`/`targetsamplelost`/`targetmap` tree-file lines, and
`proctree.c` parsing those three into `target_samples`/`target_samples_lost`/`target_maps` on each
`tree` JSON node (`PROCTREE_JSON_SCHEMA_VERSION` 1.1.0 → 1.2.0; see `doc/ARTIFACT_CONTRACT.md`) —
deliberately *not* rolled up into `summary`'s per-`comm` totals the way `target_counters` is, since a
raw address is only meaningful together with the specific process's own `target_maps` (ASLR generally
gives two instances of the same comm different load addresses); summing addresses across pids the way
`target_counters`' plain scalar values are summed would be actively wrong, not just imprecise. Verified
live end-to-end against real hardware (`--no-ipc --tree ... --target=comm=sleep --symbol-sample
--symbol-sample-event=cycles -- sh -c '/bin/sleep 0.2 & wait'`): real
`targetcounter`/`targetsample`/`targetmap` lines, including kernel-space sample addresses (expected —
`perf_event_attr.exclude_kernel` isn't set, same as every other counter in this codebase — and
correctly falling outside any `targetmap` region, so they land in `wspy-symbolize`'s "unresolved" bucket
rather than being mis-resolved), and one sample's file-relative-offset arithmetic checked by hand
against its containing `libc.so.6` map. Also added as permanent regression coverage: `golden_output.sh`'s
`symbol-sample-grammar`/`targetmap` checks (a real `targetmap` line is guaranteed whenever the counter
opens at all — an idle 0.2s sleep may genuinely accrue zero `targetsample` lines, so that one isn't
asserted) and `capability_matrix.sh`'s `tree-target-symbol-sample`/`symbol-sample-without-target-
incompatible` bundles.

**`wspy-symbolize` (resolution half).** A stdlib-only Python tool (`wspy-analyze`/`wspy-bundle`'s own
category), not linked into `wspy`: shells `proctree --json` against a raw tree file, selects one process
(`--pid`) or every same-`comm` process (`--comm`), resolves each `target_samples` address against its
own process's `target_maps` (PIE bias: `addr - map.start + map.file_offset`), batches offsets per
binary/library into one `addr2line -f -C -a` call each, and emits a sorted symbol table as JSON
(`--json`) or a human-readable table (default) — see `doc/ARTIFACT_CONTRACT.md`'s "Symbol table" section
for the full shape. `--comm` merges by `(symbol, file)` only *after* per-process resolution, never by
raw address, matching the same ASLR-driven reasoning `proctree.c` already declined to roll
`target_samples` up by comm. Five unresolved reasons are surfaced distinctly rather than one generic
bucket: `no backing map` (kernel/JIT addresses), `binary deleted during run` (a `target_map` path with a
kernel-appended `(deleted)` suffix — deliberately never attempted, since what's now at that path, if
anything, may not be the code that actually ran, so resolving against it risks a *wrong* answer, not
just a missing one), `binary unavailable` (path unreadable from this machine), `addr2line failed`
(binary exists but couldn't be processed, or `addr2line` itself isn't installed — degrades to this
rather than crashing), and `?? (unresolved symbol)` (resolved fine, but no name — typically a stripped
binary). Verified live against two real workloads: a `sleep 0.2` (mostly `no backing map` — an idle
process's few samples land mid-syscall/interrupt) and a `yes >/dev/null` busy loop for 0.5s (742/1826
samples resolved, correctly dominated by `libc.so.6`'s `write`/`syscall_cancel` internals, plus 63
samples correctly reported `?? (unresolved symbol)` against this host's own stripped Rust-`coreutils`
`yes` binary) — both matching real-world expectations by inspection, not just "didn't crash." Every
degradation path (`--pid`/`--comm` not found, missing tree file, missing/broken `addr2line`) was also
exercised live and confirmed to exit non-fatally with a clear message rather than a traceback, except
the two usage errors (`--pid`+`--comm` together, neither given), which correctly exit 2 via `argparse`.

**Web UI drill-down.** `web/server.py` gained `/api/symbolize/<suite>/<benchmark>/<run_id>?pid=<n>` (or
`?comm=<name>`), mirroring `/api/tree-json`'s exact shape (on-demand, nothing written to disk, always
reflects the current `process.tree.txt`) but shelling `wspy-symbolize --json` instead of `proctree
--json` (`web/joblib.py`'s `build_symbolize_argv()`, plus a `--wspy-symbolize` CLI flag/
`wspy_symbolize_bin` cfg entry alongside the server's other tool paths). The tree viewer
(`web/static/proctree_viewer.js`) gained a "▶ profile" toggle on any node carrying `target_maps` (the
same gate `topdown.c`'s own `have_symbol_sample` check uses — present whenever the counter opened at
all, regardless of whether any samples were actually collected) that fetches and renders a symbol table
inline, right below that node's row; result and expanded/collapsed state are cached on the node object
itself so they survive a rerender from an unrelated column-checkbox toggle (an improvement over this
file's own plain expand/collapse state, which does reset on rerender — a pre-existing, unrelated
limitation, not fixed here), and a second click doesn't refetch. Diff mode (`/tree-diff`) doesn't get
this — `target_samples`/`target_maps` were never part of the diff-mode node shape in the first place,
same as `target_counters`.

No real browser was available in this session (the user declined the Chrome extension install prompt),
so this was verified two ways instead: (1) the `/api/symbolize` endpoint directly via `curl` against a
real server + a real `--target --symbol-sample` run (success, both-selector-missing, both-given,
no-such-run, no-such-pid); (2) the actual unmodified `proctree_viewer.js` file executed under `gjs`
(GNOME JavaScript/SpiderMonkey) with a hand-written DOM+`fetch` shim, fed real JSON fixtures captured
from the live server — confirmed exactly one profile button renders for the one node with `target_maps`,
clicking shows a synchronous "Loading profile..." state then the resolved table (row counts matching the
fixture's `symbols`/`unresolved` arrays exactly) once the fetch promise chain settles, collapsing hides
it, and re-expanding reuses the cached result without a second fetch; a separate run of the same harness
against a synthetic error response confirmed `resp.error` renders as a plain message rather than
throwing, matching this viewer's existing top-level tree-load error handling (`resp.output` is
deliberately not surfaced, same limitation the existing `/api/tree-json` error path already has).
`web/test_joblib.py` gained `BuildSymbolizeArgvTest`. This is a lighter bar than a real browser
click-through, worth re-verifying in one if that becomes available.

**Attachment point.** Same place `--target` already attaches a pid-scoped counter group:
`ptrace_loop()`'s `PTRACE_EVENT_EXEC` handler (`topdown.c`, the block guarded by `if (target_active)`
that calls `setup_counter_groups()`/`setup_counters()`/`start_counters()` on a match). On a match, a
single-counter `symbol_sample_counter_group()` (generic `PERF_TYPE_HARDWARE` event, `is_symbol_sample=1`)
gets prepended to `pid_entry->target_counters` the same way the `COUNTER_SOFTWARE` group already is, so
it flows through the *existing* pid-scoped `setup_counters()`/`start_counters()`/`read_counters()`/
`close_counters()` pipeline via `is_symbol_sample` special-casing (mirroring `is_ibs_sample`'s existing
special-casing there) rather than needing a separate open/drain/close path. Drains at that pid's own
`PTRACE_EVENT_EXIT`, the same spot `target_counters` are read back and closed today (via the existing
`read_counters(pid_entry->target_counters,1)` call, which now also drains any `is_symbol_sample` ring) —
before `/proc/<pid>/maps` becomes unavailable. One deviation from the original plan: sampling is
**period-based** (`pe.sample_period`, reusing the exact generic per-counter plumbing `setup_counters()`
already has for AMD IBS), not frequency-based (`pe.freq=1`/`sample_freq`) — wspy has no per-counter
frequency-sampling plumbing yet, and adding it purely for this one feature was judged not worth it over
reusing what already exists. Default periods are a first-cut heuristic per event (`symbol_sample.c`):
1,000,000 for `cycles`/`instructions`, 10,000 for `cache-misses`/`branch-misses` (rarer events per cycle,
needing a much smaller period to fire at all against a short-lived process) — not frequency-normalized,
no `--symbol-sample-period` override yet. Unlike `ibs_sample_state` (one system-wide instance for the
whole run, left for the OS to reclaim at process exit), `close_counters()` explicitly frees
`symbol_sample_state` (`symbol_sample_free()`) whenever it frees any counter's fd — a `--target`-matched
process's ring buffer is opened fresh per match, so a long matching-heavy run needs it actually unmapped
rather than accumulating for the run's whole lifetime.

**v1 scope, stated up front rather than discovered later:**
- Flat self-hit profile only — `PERF_SAMPLE_IP`, no `PERF_SAMPLE_CALLCHAIN`. A call-graph needs
  frame-pointer or DWARF unwinding at record time, a materially bigger step; candidate follow-on item,
  not part of this one.
- Drain only at process exit — same poll-loop gap `ibs_sample.c` has (walking/decoding ring records
  isn't async-signal-safe and there's still no poll/epoll loop anywhere in wspy). A long-lived target
  risks ring wraparound before drain; `PERF_RECORD_LOST` gets counted the same way
  `ibs_sample_state.samples_lost` already does, not silently dropped.
- One `/proc/<pid>/maps` snapshot, taken at the exit-time drain, covering only file-backed executable
  regions. Covers everything ever mapped (nothing unmaps a live library on its own), but a `dlclose()`'d
  region before exit loses its map entry — those samples land in an explicit "unresolved" bucket, same
  bucket JIT'd/anonymous-mapped code and stripped-binary addresses (`addr2line` returns `??`) fall into.
- Addresses are aggregated to a bounded in-memory histogram as samples drain (hot loops → few distinct
  IPs), not one wire-format line per sample — keeps the tree file size independent of sample volume.
- Root required — same `perf_event_open()` sampling privilege class as everything else in wspy.

**Wire format.** Three new tree-file line kinds, parallel to the existing `targetcounter` line (all
implemented in `topdown.c`'s `PTRACE_EVENT_EXIT` handling and `write_target_maps()`):
```
%5.3f %d targetsample <event> <hexaddr> <count>
%5.3f %d targetsamplelost <event> <count>
%5.3f %d targetmap <hexstart> <hexend> <hexfileoffset> <path>
```
`targetsamplelost` only appears when `symbol_sample_state.samples_lost > 0` — kept as a real, structured
line kind rather than a `#`-comment aside, consistent with the rest of this file format's "everything is
a parseable `<elapsed> <pid> <keyword> ...` line" grammar. `targetmap`'s `path` field is parsed via
`%n`-based offset math rather than a plain `%s` token, since a backing file's path can contain spaces
(most commonly a kernel-appended `(deleted)` suffix). `proctree.c` parses all three into
`target_samples[]`/`target_samples_lost`/`target_maps[]` on each `tree` JSON node —
`PROCTREE_JSON_SCHEMA_VERSION` bumped 1.1.0 → 1.2.0; see `doc/ARTIFACT_CONTRACT.md` for the field shapes.

**Symbolization is a separate post-hoc tool, not inline in wspy** (`wspy-symbolize`, Python — same
category as `wspy-analyze`/`wspy-bundle` — rather than shelling `addr2line` out of the ptrace loop
itself, which would block the traced children). Reads a tree JSON plus a pid or comm selector, finds
each sample address's containing `targetmap` region, computes the standard PIE/`.so` file-relative
offset (`addr - map.start + map.file_offset`, the same formula `perf script`/`eu-addr2line` use — a
non-PIE executable has `file_offset≈0` so this degrades to the identity map), batch-shells `addr2line -e
<path> -f -C -a <offsets...>` once per file rather than once per address, and emits a sorted symbol
table (symbol, file, count, % of resolved samples, plus an explicit "unresolved" row) as its own JSON
file — new output shape, not shoehorned into the run's CSV.

**CLI.** Two flags, not the originally-planned single `--symbol-sample[=event]` — this codebase has no
prior use of `getopt_long`'s `optional_argument` anywhere, and it has a real gotcha (`--symbol-sample
cycles`, space-separated, silently fails to bind "cycles" as the value and can misroute the workload
command's own `--` boundary), so it was judged not worth introducing for this one flag:
**`--symbol-sample`** (boolean, defaults to `cycles`) and **`--symbol-sample-event=<event>`**
(`required_argument`, implies `--symbol-sample`), `event` ∈ a small curated set of generic
`PERF_TYPE_HARDWARE` events — `cycles` (default, matches `perf record`'s own default) / `instructions` /
`cache-misses` / `branch-misses` — not arbitrary raw events in v1, and portable across AMD/Intel/ARM
with no per-vendor event table needed. Fatal without `--target` active, which is itself already fatal
without `--tree` — so the dependency chain is `--symbol-sample`/`--symbol-sample-event` → `--target` →
`--tree`, consistent with how `--target` is gated today.

**Web UI (design intent).** The tree viewer's per-node/per-comm panel already rendered `target_counters`
("PID-targeted counter attachment", PR #167) — this feature added a sibling "Profile" view next to it
calling `wspy-symbolize` and rendering the resulting symbol table, same UI pattern as the existing
hot-process table (PR #163); see "Web UI drill-down" above for what actually shipped.

### GPU kernel-level instrumentation: scope decision (2026-07-08, revised 2026-07-18)
GPU *kernel*-level instrumentation (CUDA/Vulkan profiling — tracing individual compute kernels/shaders,
not point-in-time busy%/VRAM monitoring) was cut from the roadmap on 2026-07-08 as a project scope
decision: this codebase has no CUDA/Vulkan profiling code, and building one is a different kind of
project than a `perf`-counter/system-metrics wrapper. Revisit only if the project's mission changes to
include GPU kernel-level profiling.

**Revised 2026-07-18:** that decision was specifically about kernel-level instrumentation, not
cross-vendor GPU *monitoring* — the narrower, AMD-parity capability (busy%/VRAM via a vendor management
API, exactly what `amd_smi.c`/`amd_sysfs.c` already do for AMD) shipped in 4.0 as `--gpu-nvidia`
(`nvidia_nvml.c`, `NVIDIA=1` build flag): NVML is `dlopen()`d at runtime rather than linked at build
time, so unlike the AMD path there's no ROCm-equivalent header/toolkit dependency at build time. See
`CLAUDE.md`'s "GPU support" section and `nvidia_nvml.c`'s entry in the Architecture list for current
mechanism.

### Sub-~10ms target processes can read back all-zero/`-nan` counters (root-caused 2026-07-22)
A real perf-subsystem timing limitation, not a wspy bug — no fix planned, recorded here so it isn't
re-investigated from scratch later. Root-caused on carlsbad (real Intel hybrid hardware) via `strace` on
a failing `--ipc -- true` run: the raw `read()` bytes showed `value=0, time_enabled≈10.8ms,
time_running=0` — the counter was armed the whole window but the kernel never scheduled it onto a real
PMU register during the child's brief life, so both the parent's own count and (per
`perf_event_exit_task`'s inherited-count rollup) the child's are genuinely zero, not misread. Confirmed
generic across counter families — plain `--ipc` (ordinary `PERF_TYPE_HARDWARE`, no Perf Metrics
involved) reproduces it at a similar rate to `--topdown2`, contradicting this item's original working
theory that it was Perf-Metrics/fixed-counter-specific.

Essentially absent on realistic workloads: 0 failures across ~130 combined runs of a 0.3-0.4s CPU-bound
workload (both `--ipc` and `--topdown2`), vs. ~15-30% of runs against `true` (~3ms). A standalone probe
(open on self + `inherit=1` before fork, matching `setup_counters()`/`launch_child()` exactly,
`/tmp/claude-*/inherit_probe.c`, not part of the tree) reproduced the same failure mode at lower but
nonzero rates and showed it scales with how many counters share a perf group (single event: ~0.7-1.7%;
a 2-member group: ~5.3%) — consistent with a larger/more-complex group simply taking the kernel
measurably longer to finish installing onto real hardware after the task starts, which a
short-enough-lived child can outrun entirely. Also ruled out one likely-sounding "fix": switching from
wspy's self+`inherit=1`-before-fork model to directly targeting the child's pid (the `perf stat`-style
approach, opening after a `PTRACE_TRACEME` exec-trap stop) made failures dramatically *worse* (81% vs.
<2% in the same probe) — the extra ptrace stop/`PTRACE_CONT` round-trip itself eats into the
already-tiny time budget. No wspy-side action item: don't chase this further without new evidence.

## Validation narratives (4.2-era)

### Zen5/IBS platform-behavior findings (4.2/4.3)
What was confirmed from current Linux perf/PMU behavior for AMD Family 1Ah (Zen5), informing 4.2's
"Zen-family preset packs"/"PMU-capability-aware comparability warnings" (both shipped) and AMD IBS
sampling-mode support (shipped in 4.3 — icache/TLB/dcache/L2/L3/branch rate estimates decoded from real
per-sample tag data, not just counting-mode sample counts):

1. Zen5-specific IBS load-latency filtering enables L3-miss-only filtering via a Zen5 feature check.
2. Generic `PERF_COUNT_HW_*` mapping on Family 1Ah still follows the Zen4 event-map path in current
   kernel PMU logic — there isn't yet a distinct "Zen5-only" generic hardware-event map.
3. IBS capability extensions (L3-miss-only, load-latency/fetch-latency filters, richer memory-source
   decoding) are the strongest near-term source of additional signal.
4. L3-miss-only filtering is documented to skew sampling-period behavior — runs using it need explicit
   annotation (shipped — see `topdown.c`'s `print_ibs()`).

**AMD IBS sampling-mode's decode scope was deliberately left partial** (`ibs_sample.c`/`ibs_sample.h`):
the fixed-offset prefix every record carries (op-side `dc_miss`/`dc_l1tlb_miss`/`dc_l2tlb_miss`/
`op_brn_misp`, `IbsOpData2`'s `dram_rate`/`remote_node_rate`, fetch-side `ic_miss`/`l1tlb_miss`/
`l2tlb_miss`) decodes cleanly, but the "variable-position words" — `IbsBrTarget`, `IbsDcLinAd`/
`IbsDcPhysAd` — are raw addresses, not rate-shaped data, and don't fit wspy's reporting model without a
NUMA-node-of-address or symbol-resolution layer that's really its own separate feature; `IbsOpData4` has
no documented bitfield layout anywhere in current kernel/`perf` source, so decoding it would mean
inventing bit positions, which this project's own conventions forbid. Recorded here as the reason those
three fields are permanently out of scope for this decode path, not a "not yet" deferral.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update presets
and coverage logic without changing the report schema. See `INVESTIGATION.md`'s Zen5/IBS deep-dive for
the one thread this didn't settle (finer per-scheduler breakdown events for the "platform formula
registry").

### AMD IBS real-hardware validation (Zen5, 2026-07-15)
Exercised `--ibs-basic`/`--ibs-memory-deep` against real `ibs_fetch`/`ibs_op` PMUs on real Zen5 (family
25 model 116) hardware. Surfaced a real bug: `ibs.c` derived IBS's MaxCnt from a sysfs `format` field
named `"maxcnt"` that doesn't exist on real kernels (MaxCnt actually comes from
`perf_event_attr.sample_period`, per `perf_ibs_init()` in `arch/x86/events/amd/ibs.c`), so every IBS
counter had silently failed `perf_event_open()` with `-EINVAL` since the feature shipped —
`test_ibs.c`'s synthetic-sysfs-only coverage never called `perf_event_open()` and so never caught it.
Fixed (`sample_period` threaded through `ibs.h`/`ibs.c`/`cpu_info.h`/`topdown.c`); confirmed live:
`ibs-basic` now measures 2/2 counters, `ibs-memory-deep` 3/3, with real nonzero `ibs_fetch`/`ibs_op`
values, and `--interval` combined with `--ibs-basic` produces a genuine per-tick time series (not just
an aggregate row) — mechanically it was never aggregate-only, `wspy-run`'s builtin `ibs-basic`/
`ibs-memory-deep` profiles just never passed `--interval`. Also added a real-hardware IBS probe to the
web launcher's "Check" button (`ibs_probes_for_request()`/`probe_ibs()`) so a run that would use IBS
gets this same live `perf_event_open()` verification before launching, not just `--capabilities`'
sysfs-presence check. `ibs-basic`/`ibs-memory-deep` (`wspy-run`) now always pass `--interval 1`, the web
checklist's IBS row has its own optional `interval_secs` field, and `plot.c` gained `ibs`/
`ibs-accepted-ratio` templates — confirmed live: both profiles render real gnuplot PNGs from genuine
per-tick `ibs_fetch`/`ibs_op`/`ibs_op_accepted_ratio` time series on this hardware.

**Still not exercised:** real filtering behavior (l3missonly/ldlat skew) hasn't been specifically
compared filtered-vs-unfiltered on real hardware — this session's runs used `ibs-memory-deep`'s
defaults but didn't isolate the effect. Carried forward as an open validation item.

### `proctree`/tree-file robustness fix (`8271e55`, 2026-07-14) + fork-heavy real-workload validation
Commit `8271e55` ("topdown: fix ptrace_loop() double-continue race dropping fork events") fixed two
things: (a) a real bug — two `WIFSTOPPED` branches in `ptrace_loop()` each issued their own
`ptrace(CONT/SYSCALL)` call and then fell through to the loop's unconditional second `CONT` at the
bottom with no intervening `wait4()`, so under a burst of concurrent forks/exits the stray second call
could race ahead and consume the tracee's next real stop (e.g. the very next `PTRACE_EVENT_FORK`)
before the main loop ever logged it — a genuinely lost `fork` line, not just a misordered one; and (b)
writer-side reordering tolerance — `ptrace_pid_table[]`, a small hash table keyed by pid that defers a
not-yet-known pid's buffered "comm"/"cmdline"/"exit" block until its own "fork" line has been written,
so the file's line order always has `fork` before `exit` for the same pid regardless of which ptrace
stop the kernel delivers first.

Confirmed against a genuinely large real workload (`workload/phoronix/run_test.sh`'s `deep-cpu,
tree-heavy` profile against `phoronix-test-suite batch-run build-gcc`, run 2026-07-16, `sacramento`
host): 155,780 fork events across a 342-second, 155,781-process run, reconstructed cleanly by
`proctree` (0.46s, no `exit for unknown pid`/"unable to remove process" warnings).

**One small, distinct residual gap remains,** discovered from this same run: 7 of the 155,781 processes
(~0.0045%) got a plain `WIFEXITED` reap with **no** preceding `PTRACE_EVENT_EXIT` ptrace-stop at all
(confirmed by diffing `process.tree.txt`'s `fork`-target pids against its `exit`-block pids — each of
the 7 has a `fork` line and an `exited` line but no `comm`/`cmdline`/`exit`-stat block in between). This
is a different mechanism than the reordering bug above — not a stop arriving in the wrong order, but a
stop the kernel apparently never delivered before the process was reaped — so `8271e55`'s fix doesn't
cover it, and it isn't yet understood whether it's a further ptrace corner case (e.g. a process that
exits between its `SIGSTOP`-after-fork and the tracer re-enabling `PTRACE_O_TRACEEXIT` on it) or
something else. Not urgent: `proctree.c` already degrades gracefully for it (a pid whose "exit" block
never arrives just keeps its zeroed `finish`/`cpu`/`vmsize`/`utime` fields and `??` in place of `comm` —
no crash, no warning), so this is tracked as a known, now-quantified small imprecision rather than an
open correctness bug requiring a code change.

### `wspy-ledger` orphaned run-index record handling (PR #81, 2026-07-18)
Real use surfaced a concrete messiness case: after deleting a failed run's whole output directory (the
common cleanup step when a run fails for an environment reason — missing tool, bad permissions — rather
than a real workload problem), its now-orphaned run-index record kept `wspy-ledger` permanently
reporting that workload as `needs-tool-support`, with no way to get back to `skipped` short of
hand-editing the run-index file. Fixed: `wspy-ledger` now checks each matching record's own
`output_path`/`tree_output_path`/`manifest_path` against disk and excludes ones whose files are gone
from `runs_matched`/`runs_succeeded` scoring (so the workload degrades back toward `skipped`), while
still counting/reporting them as `runs_stale` — a CSV column plus a report detail note, both suppressed
by `-q` the same way `done` rows already were — so the exclusion stays auditable rather than silent.
This is a read-time check against the run index as it stands; nothing rewrites or prunes the file
itself.

### ARM64 topology/topdown/ptrace: shipped and validated on real hardware
`cpu_info.c`'s `__cpuid()`/`<cpuid.h>` use is guarded behind `#ifdef __x86_64__`, with a
`/proc/cpuinfo`/`/sys/devices/system/cpu` fallback inventory path for everything else
(vendor/family/model, core count, `armv8_pmuv3_*` PMU-cluster discovery for mixed big.LITTLE systems)
and a topdown-equivalent decomposition wired through raw ARM PMU events in `topdown.c`'s
`print_topdown()`/`print_branch()`/`print_l2cache()`/`print_memory()`. `setup_counters()` also honors
per-core `target_cpu` binding in `--per-core` mode so mixed-PMU clusters route raw events to the right
core's PMU type. This is real ARM64 `cpu_info` support, distinct from the earlier `ptrace_arch.h`
`__aarch64__` register-access branch (also validated on real hardware). Two gaps found by code review
(PMU counter chunking/bin-packing and topdown sanity-tolerance warning checks) were fully addressed;
both topology and ptrace support have been validated on real ARM64 hardware.

### `gpu-compute-profile` builtin + CSV correctness fixes (PR #84, real yquake2/ollama testing)
`wspy-run gpu-compute` is one `wspy` invocation/one execution of the workload combining syscall-latency
tree tracing, system, power, both GPU backends, and topdown on a shared `--interval` timeline, for a
GPU-bound/latency-driven workload where `deep-cpu`/`deep-gpu`'s separate-re-execution-per-category shape
can't be correlated tick-for-tick. Surfaced and fixed two independent, pre-existing CSV correctness
bugs while building it: `--gpu-metrics`/`--gpu-smi`/`--gpu-nvidia` were silently dropped from output
whenever `--system` was also requested (header and value shared the same wrong `!sflag` gate, so no
column-count mismatch ever caught it); and `timer_callback()`'s per-tick print order didn't match the
CSV header/final-row order whenever a counter group (e.g. `--power`) was combined with any GPU flag
under `--interval`.

Also shipped alongside it, from the same real-workload testing round:
- **CPU temperature (`cpu_temp`) system metric.** New `SYSTEM_TEMP` bit (`system.c`), on by default
  alongside load/cpu/network/freq — hwmon-based discovery (`k10temp`/`coretemp`/`cpu_thermal`), a
  single sysfs read, no privileges needed, same cost class as `--freq`.
- **GPU-aware shared plot templates + stable per-metric colors.** `plot.c` gained `gpu-utilization`
  (GPU busy % on its own chart), `gpu-vram` (VRAM usage on its own MB-scale chart), `gpu-thermal` (GPU
  temp vs. frequency), and `temp-vs-frequency`/`temp-vs-power`/`temp-vs-utilization` (CPU temp
  pairings) — found necessary from a real ollama run where an ~8151 MB VRAM column was flattening
  every other metric in the generic fallback plot. Also added `metric_line_color()`: a stable
  per-column-name line color (curated table + hash fallback) instead of gnuplot's own per-invocation
  positional cycling, so the same metric renders the same color across every chart it appears in.
- **Dual process-tree output (`process.tree.simple.txt`).** Every automatic proctree step now writes
  both `process.tree.summary.txt` (every annotation the tree pass actually captured) and
  `process.tree.simple.txt` (proctree's own bare invocation — just `cpu=`/`start=`/`finish=` per
  process), since a heavily-annotated summary gets visually busy enough that seeing the raw process
  hierarchy gets harder, not easier.
- **Web launcher GPU-build verification.** The Check button now verifies wspy was actually built with
  the GPU backend(s) (`AMDGPU=1`/`NVIDIA=1`) a request's preset/checklist would use
  (`check_gpu_build()`), instead of that only surfacing as a "not built" line buried in a run's log;
  also fixed `power_probes_for_request()`, which had unconditionally skipped the power probe for every
  preset on a stale claim that none used `--power` (`deep-cpu`'s systemtime pass has carried it all
  along).

### Concrete design: repeatability policy + confidence metadata (2026-07-19, shipped)
**Shipped 2026-07-19,** exactly as designed below: `summary.c`'s `emit_bucket()` now computes a 95%
confidence interval of the mean and a repeatability verdict for every reported bucket, alongside the
pre-existing mean/stddev/`cv_percent`. `test_summary.c` gained 14 new tests (the t-table/CI helper in
isolation, `compute_verdict()`'s four PASS/WARN combinations, and five `summarize()`-level integration
tests covering the default-threshold, `--max-cv`-adjusted, and combined-thin-and-noisy cases) — all
passing, alongside the full pre-existing suite unchanged. Manually smoke-tested against a real SQLite
fixture: CSV comma-quoting for the `WARN:thin,noisy` verdict (which itself contains a literal comma)
round-trips correctly through `print_csv_field()`, and `--strict` returns exit 1 when any bucket carries
a `WARN` verdict and exit 0 once `--max-cv`/`--command` narrow the request to only `PASS` buckets.

Design for 4.2 Tier 2's "Repeatability policy + confidence metadata... as default output" item, worked
out before implementation since the original backlog line understated how much had already shipped and
undersold the one real design question (the verdict/policy half) — `emit_bucket()` already computed and
printed mean, stddev, and CV (`cv_percent`) unconditionally for every reported bucket before this item
started; what was actually missing was the confidence interval and a pass/fail-style verdict layer.

**Confidence interval (95%, Student's t):**
- Two-tailed 95% CI of the per-run mean, using Student's t rather than a normal/z approximation —
  with `n` typically 3-10 wspy-level repeats, t's fatter tails matter more than they would at sample
  sizes where a normal approximation is usually fine.
- No stats library is linked (`summary.c` only pulls in `sqlite3`/`math.h`), so this needed a small
  hardcoded critical-value table — same idiom as `validate.c`'s `sanity_bounds[]` or `topdown.c`'s
  event tables, not a general-purpose stats routine. `t95_table[30]`, indexed by `df = n - 1`, covers
  `df=1..30` (12.706, 4.303, 3.182, ... down to 2.042 at `df=30`); `df > 30` falls back to `z = 1.96`
  (t and normal are close enough there, and repeat counts this high will be rare in practice).
- `n < 2` (`df < 1`, no table entry) returns a zero-width interval (`ci_low = ci_high = mean`) directly,
  without consulting the table — consistent with `compute_stats()`'s existing convention that `stddev`
  is 0 (not NaN/undefined) for `n < 2` ("nothing to vary against"); since half-width is
  `t * stddev / sqrt(n)`, this is also what the formula would produce anyway once `stddev = 0` — the
  branch just avoids needing a `t(df=0)` table entry to get there.
- Not configurable — no `--confidence-level` flag. `compute_stats()`'s `stddev` is likewise fixed at
  sample (`n-1`) with no user-facing knob; one sane fixed default (95%) beats a flag for every
  statistical choice this tool makes.
- `compute_ci95(mean, stddev, n, *ci_low_out, *ci_high_out)` is a standalone helper called from
  `emit_bucket()` right after `compute_stats()` returns — deliberately *not* folded into
  `compute_stats()`'s own signature, since CI only needs the mean/stddev/n that function already
  returns, and keeping it separate meant `test_summary.c`'s existing `compute_stats()` coverage didn't
  need to change shape, only grow independent tests for the new helper.
- New CSV/human columns: `ci95_low`, `ci95_high` (actual bounds, not a bare margin) — mirrors `min`/
  `max` already being two separate columns in this same table rather than one combined range.

**Verdict layer (the actual "policy" half):**
- Two states only, `PASS`/`WARN` — no `FAIL`. This is a confidence signal about how much to trust a
  number, not a data-validity check (that's `validate.c`'s job); nothing a verdict here catches is
  "broken," so `validate.c`'s three-state PASS/WARN/FAIL vocabulary doesn't fully transfer.
- Computed only for buckets that already cleared `--min-runs` and were emitted — a bucket skipped for
  `--min-runs` stays skipped exactly as before this item (unchanged default behavior at `--min-runs`'s
  default of 1); the verdict layer never sees it.
- `WARN:thin` if `n < 3` (`VERDICT_MIN_RUNS_FOR_CONFIDENCE`) — reuses, rather than invents, the exact
  threshold `compute_stats()`'s own outlier flagging already applies ("flagging with fewer samples has
  no real meaning"). Independent of `--min-runs`: `--min-runs` controls what's even shown, this
  controls what's shown *with* a confidence caveat attached, and the default `--min-runs=1` means most
  of the "is this thin" work happens here, not there.
- `WARN:noisy` if `cv_percent > opts.max_cv` — new `--max-cv <percent>` flag, default `5.0`, symmetric
  with the existing `--outlier-stddev` flag: one blunt global default, user-overridable, not per-metric.
  (`validate.c`'s per-column `sanity_bounds[]` table is the precedent for a future per-metric override
  if 5% turns out wrong for some metrics and not others — deliberately not built here without real data
  to justify differentiated thresholds.)
- Both conditions can fire together: one `verdict` column holding `PASS` or `WARN:<reasons>` (e.g.
  `WARN:thin,noisy`) — single field, not two, mirroring how `outlier_ids` is already one field carrying
  a list rather than exploded across columns. Since that combined value contains a literal comma,
  `print_csv_field()` quotes it in CSV output exactly like any other comma-bearing field.
- `--strict` gained a third failure condition: any emitted bucket with a non-`PASS` verdict, on top of
  its existing two (`groups_skipped_min_runs > 0`, nothing matched at all) — directly matches
  `wspy-validate`'s own documented `--strict` behavior ("also fails on any WARN," `CLAUDE.md`), keeping
  the two tools' `--strict` semantics consistent instead of diverging. `summary_totals` gained a
  `groups_warned` counter for this, surfaced in the trailing (non-`--quiet`) summary line alongside the
  existing skipped/scanned counts.
- Column order (CSV and human table both): `...,stddev,cv_percent,ci95_low,ci95_high,verdict,
  outlier_count,outlier_run_ids[,contributing_runs]` — CI sits next to stddev/CV (the dispersion
  numbers it's derived from), verdict right after (the rollup conclusion from `n` and `cv`), outlier
  detection stays a separate, later concern (a per-value diagnostic, not part of the repeat-confidence
  rollup) exactly where it already was.

**Caveat: wrapped multi-trial harnesses confound wspy-level repeat counting.**
`wspy-summary`'s repeat unit is one wspy invocation — it has no visibility into what happens inside the
child process. That's a clean assumption for a bare workload, but not when the wrapped command is
itself a multi-trial harness: Phoronix Test Suite runs each test at least N times and adds more
internally if its own variability check exceeds its own threshold; SPEC CPU2017 runs a fixed multiple
times and reports high/low/median. Two concrete failure modes follow directly from that mismatch:
- **False `WARN:noisy`:** two wspy invocations of "the same" Phoronix test can legitimately span a
  different number of Phoronix-internal sub-runs (3 vs. 6, say, because Phoronix's own adaptive-N
  decided one run needed more), so wspy-level CV across repeat invocations reflects hardware/
  measurement noise *and* swings in the harness's own internal trial count, conflated into one number.
  A `WARN:noisy` here might be correctly flagging real instability, or might just be an artifact of
  Phoronix having auto-added runs on one invocation and not another — wspy has no way to distinguish
  the two.
- **Misleading `WARN:thin`:** the reverse case — a workload with excellent harness-internal
  repeatability (Phoronix/SPEC already settled on a tight median across several internal trials) still
  reads as `n=1` at the wspy-summary level if only one wspy invocation wrapped it, triggering
  `WARN:thin` even though the underlying measurement is already well-repeated, just invisibly so from
  wspy's side.
- **Deliberately not fixed in `summary.c` itself:** the tool stays harness-agnostic, matching its
  existing design (it doesn't know or special-case what produced the command line, and Phoronix/
  SPEC-specific logic belongs at the layer that already has it — `web/server.py`'s
  `parse_phoronix_test_names()`/`ledger.c`'s `--phoronix-profiles-dir` scanning are the precedent for
  where harness-specific detection already lives in this codebase, not the stats tool). No
  differentiated `--max-cv`/`WARN:thin` threshold for detected harnesses without real data to justify
  one.
- **Mitigation is documentation, not code:** this caveat should be surfaced wherever verdict output is
  explained to a reader (4.2 Tier 8's "profile cookbook + interpretation playbook" item is the intended
  home once it ships), with the actionable fix stated plainly: pin the harness's own internal run count
  (Phoronix supports fixing it instead of letting it auto-add) if clean, unconfounded wspy-level
  repeatability data is wanted for a harness-wrapped workload.
- **Explicitly deferred, not folded into this item:** capturing a wrapped harness's own internal trial
  count into manifest/provenance (so a future reader could reconcile the two layers automatically) is a
  real gap but a separably-scoped piece of work — worth its own backlog line later if it turns out to
  matter in practice, was not built here.

### Concrete design: comparison matrix mode (2026-07-19, shipped)
**Shipped 2026-07-19,** as designed below: `store.c` ingests `preset_name`/`config_name`/`affinity_*`/
`run_config_options` (`STORE_SCHEMA_VERSION` 2→3, `MIGRATION_V2_TO_V3`); `summary.c` gained five new
`--group-by` values and the composable `--group-by-option`; `wspy-run` gained a `--config-option`
passthrough; the new `wspy-sweep` tool cross-products `--affinity` values against workloads. Verified
end to end on real hardware: `wspy-run`/`wspy-store`/`wspy-summary` round-tripped a real `--affinity
all`/`nosmt` sweep through real `wspy` invocations (not synthetic fixtures) into a correctly-grouped
`wspy-summary --group-by command --group-by-option affinity` report; `wspy-sweep`'s quick and spec
forms, `--dry-run`, and its error paths (unrecognized axis key, empty workloads, `--spec` combined with
quick-form flags) were all exercised directly. `test_store.c` gained 5 new tests (ingestion, re-ingest
value updates, missing-provenance degrade-to-NULL, and a `test_schema_migration_v2_to_v3` alongside the
existing v1→v2 test) and `test_summary.c` gained 5 (new fixed groupings including the `run_environment`
join, `--group-by-option` composing with the primary group, its inertness when unset, and its
"(unknown)" degrade for a run with no matching option) — all passing alongside the full pre-existing
suite. One real mistake caught during manual testing, not a design flaw: an ad hoc test invocation
placed `--dry-run` after `wspy-sweep`'s own `--` separator, which is by design treated as part of the
workload command rather than a flag — it actually launched a real `phoronix-test-suite batch-run
coremark` (including installing and running coremark) before being caught and killed; no data or state
was corrupted, but it's a sharp edge worth remembering when testing any tool with a `--` splitting
convention.

Design for 4.2 Tier 2's "Comparison matrix mode (sweep compiler/kernel/governor/SMT/VM-native)" item.
Split into two pieces that had to land together: making sweep results *comparable* at all (nothing
before this item could group runs by anything but command/hostname/cpu_vendor), and the sweep *runner*
itself. Scope narrowed substantially from the backlog line's five example axes once checked against
what actually exists: Phoronix/SPEC own their own compiler/build-variant machinery (external to wspy,
not something this tool drives), a kernel version can't be switched without a reboot, and nothing in
this codebase has ever written `scaling_governor` (`provenance.c` only reads it) — so of the five, only
**SMT/core-type/L3-domain placement** (via the existing `--affinity=<spec>` mechanism) is something
wspy can actually flip and re-measure in one sitting. Everything else became a human-supplied,
uniform-per-invocation context tag, not a swept axis.

**Piece 1 — make results groupable (`store.c`/`summary.c`):**
- Two things already flowed through every `run-index.jsonl` record, completely unused before this item:
  `options.affinity.{requested,mode,cpus}` (`run_index.c:136-144`) and
  `configuration_provenance.{preset,configuration,options[]}` (`run_index.c:146-164`, the
  `--preset-name`/`--config-name`/`--config-option k=v` metadata `manifest.h`'s
  `manifest_config_provenance` already carries). `grep -c affinity store.c` was 0 — neither was ingested
  into the SQLite store, so `wspy-summary` couldn't see either one, let alone group by it.
- `STORE_SCHEMA_VERSION` 2→3, a new `MIGRATION_V2_TO_V3` (same `ALTER TABLE ADD COLUMN`/`CREATE TABLE`
  shape as the existing `MIGRATION_V1_TO_V2`, dispatched from `ensure_schema()`'s `user_version==2`
  branch, chained after `MIGRATION_V1_TO_V2` when starting from `user_version==1` so a v1 database
  reaches v3 in one `ensure_schema()` call): `runs` gained five columns — `preset_name`, `config_name`
  (from `configuration_provenance.preset`/`.configuration`), `affinity_mode`, `affinity_requested`,
  `affinity_cpus` (from `options.affinity.*`) — small, fixed-cardinality, one-value-per-run fields, the
  same shape as `per_core`/`system_flag` already on that table. A new child table `run_config_options
  (run_id, option_name, option_value, PRIMARY KEY(run_id,option_name))` holds the genuinely open-ended
  `configuration_provenance.options[]` array, populated by a new `replace_run_config_options()` that
  copies `replace_run_command_args()` (`store.c:372-396`)'s DELETE-then-INSERT idempotent-reingest shape
  — with one addition beyond a pure copy: an `ON CONFLICT(run_id,option_name) DO UPDATE`, since (unlike
  `run_command_args`' array-index key, which can never collide within one INSERT loop) `wspy`'s
  `--config-option` parsing never deduplicates repeated keys, so the same `option_name` can legitimately
  appear twice in one record's `options[]` — last value in array order wins, rather than the second
  occurrence silently failing the `PRIMARY KEY` constraint.
- `summary.c`'s `--group-by` whitelist (`command`/`hostname`/`cpu_vendor` before this item) gained
  `affinity_mode`/`preset_name`/`config_name` (plain `runs` columns, no join) and `cpu_governor`/
  `virt_role` (already-ingested `run_environment` columns that `summarize()`'s query had never joined —
  needed regardless of the config-provenance work, since provenance fields had been sitting in the
  store, ungroupable, since 4.0). `group_by_column()` now returns a fully-qualified `r.`/`e.`-prefixed
  reference (previously a bare column name the query template itself prefixed with `r.`), since the two
  new `run_environment` groupings need the `e.` alias instead.
- New `--group-by-option <name>` flag: the truly open-ended case, since a `--config-option` key is a
  front end's own invented vocabulary, not a fixed enum. Implemented as a **parameterized** join to
  `run_config_options` (`option_name = ?3`, bound — not interpolated) alongside the existing
  `--group-by`, not instead of it: `struct bucket` gained a second key field (`secondary_val`), the
  query gained a second `SELECT`/`ORDER BY`/bucket-boundary column, and `emit_bucket()` prints one new
  column, present only when `--group-by-option` was actually given (same conditional-column precedent
  `--show-runs`' `contributing_runs` already set). `run_environment`/`run_config_options` are both
  unconditionally `LEFT JOIN`ed regardless of whether either grouping is in use — binding `?3` to `""`
  when `--group-by-option` isn't given never matches a real `option_name` (`store.c` never stores an
  empty one), so `secondary_val` is uniformly `NULL`/inert rather than needing a second query shape.
  This is a real, moderate change to `summarize()`'s query shape, not just a whitelist extension — but
  it's what actually delivers "for this workload, broken out by SMT on/off" rather than a single flat
  regrouping. `parse_group_by()`'s existing comment about the whitelist being what makes raw-SQL
  interpolation safe stayed true and unchanged — `--group-by-option`'s value never goes anywhere but a
  bound parameter, precisely because it isn't drawn from a fixed set.

**Piece 2 — the sweep runner (`wspy-run` + new `wspy-sweep`):**
- One small prerequisite gap in `wspy-run`: `--affinity <spec>` was already a top-level flag forwarded
  to every pass (`run_pass()`) — exactly the mechanism a sweep needs for its one real controllable axis,
  no change needed there. `--config-option <k>=<v>` was not forwarded at all before this item (`wspy-run`
  only ever auto-emitted its own `--preset-name`/`--config-name`) — needed both to tag which cell's
  axis value produced a run (so `--group-by-option` can find it) and to carry the human's uniform
  context labels (compiler/kernel/governor-as-observed). Added as a repeatable `--config-option`
  top-level flag, forwarded in `run_pass()` exactly like `--affinity` already is (guarded with bash's
  `"${CONFIG_OPTIONS[@]+"${CONFIG_OPTIONS[@]}"}"` idiom so an empty array doesn't trip `set -u`).
- New tool `wspy-sweep` (Python, stdlib-only — matching `wspy-queue`/`wspy-analyze`'s "thin client"
  convention rather than `wspy-run`'s bash, since this manipulates structured data — cross products,
  JSON — where bash gets awkward; deliberately does not import `web/joblib.py` despite some logic
  overlap with its `run_store_ingest_besteffort()`/`shell_preview()`, to keep this tool's fate decoupled
  from `web/server.py`'s own internal module structure). Two invocation shapes, mirroring `wspy-run`'s
  own builtin-profile-vs-`-c <file>` duality: a quick CLI form (`wspy-sweep --affinity all,nosmt
  --profile deep-cpu -- <command>`) for a one-off sweep, and a declarative JSON `--spec <file>` for
  anything bigger (multiple workloads, uniform tags):
  ```json
  {
    "suite": "sweep-smt-coremark",
    "workloads": [{"name": "coremark", "command": ["phoronix-test-suite", "batch-run", "coremark"]}],
    "axes": {"affinity": ["all", "nosmt"]},
    "profile": "deep-cpu",
    "tags": {"compiler": "gcc13", "kernel": "6.12.0"}
  }
  ```
  `load_spec_from_args()` builds an equivalent in-memory spec from either invocation shape so
  `build_cells()`/`build_wspy_run_argv()` never need to know which form was used; the two forms are
  mutually exclusive (`--spec` combined with any quick-form flag is a usage error, not silently
  ignored).
- `axes` is a dict (not a hardcoded two-field format) so a second genuinely controllable axis could be
  added later without a spec redesign, but v1 wires up exactly one handler: `affinity`. Crucially, that
  handler is **generic** (`AXIS_HANDLERS["affinity"]`) — each value is passed straight through as
  `--affinity=<value>` unmodified, not a semantically-typed "SMT on/off" enum. That one handler covers
  SMT sweeps, L3-domain splits, *and* core-type comparisons for free, since `affinity.c`'s spec grammar
  already treats `nosmt`/`domain=<id>`/`coretype=<id>` as peers (`["coretype=0", "coretype=1"]` sweeps
  Zen5-vs-Zen5c or Cortex-A720-vs-A520 with zero additional code). An unrecognized `axes` key is a hard
  spec error (`fatal()`, exit 2), not silently ignored — matching `--passes`' own
  fatal-on-unsupported-combination idiom. `tags` maps straight onto `--config-option`, applied
  identically to every cell in one invocation.
- Each cell tags its own axis value(s) via `--config-option <axis-name>=<value>` (e.g.
  `--config-option affinity=nosmt`) in addition to actually applying the axis — deliberately using the
  axis's own name as the config-option key (not a separately-invented label), and deliberately *not*
  relying on `summary.c`'s new `affinity_mode` column for this specific purpose: `affinity_mode` is only
  the resolved spec's *category* (`"coretype"`), not which id was swept, so a `coretype=0`-vs-`coretype=1`
  sweep would collapse into one `affinity_mode` bucket despite being exactly the comparison the sweep
  exists to make — `--group-by-option affinity` (the exact raw value) is what actually distinguishes
  them.
- Coretype/domain IDs are assigned per-host by `affinity_topology_discover()`'s ascending-CPU scan
  order, not portable labels — a sweep spec comparing core types needs its IDs looked up first via
  `wspy --list-affinity` (already prints `coretype N: implementer=0x.. part=0x.. cpus ...`). Noted in
  the tool's own module docstring; auto-expansion (e.g. a magic `"all-coretypes"` keyword) was not
  built — a cheap, obvious follow-up once the plain mechanism sees real use, not v1 scope.
- Execution: cross product of `workloads × axes` values, run strictly sequentially (matching
  `wspy-queue`'s own one-PMU-at-a-time rule, not something to relax here), each cell an ordinary
  `wspy-run --affinity <val> --config-option <axis>=<val> --config-option <tag_k>=<tag_v> ... --suite
  <suite> --benchmark <name> --run-id <sweep-timestamp>-<index>-<cell-id> <profile> -- <command>`
  invocation, the literal command line printed before running it — never paraphrased, same principle
  the web launcher already holds itself to. The run-id's timestamp is generated once per `wspy-sweep`
  invocation (not per cell), so cells within one sweep share it while a later re-run of the identical
  spec gets a fresh one — re-running a sweep accumulates new repeats in the store rather than colliding
  with (and silently updating) the prior invocation's run identities. `--dry-run` prints every cell's
  command line without executing anything. A cell whose `wspy-run` invocation exits nonzero doesn't
  abort the sweep (the remaining cells still run) but is tracked and reported, with exit code, once the
  sweep finishes, and makes `wspy-sweep`'s own exit code nonzero. After the sweep: best-effort
  `wspy-store` ingest (same idiom as `web/joblib.py`'s `run_store_ingest_besteffort()`), then **print,
  not run**, the matching `wspy-summary --group-by command --group-by-option <axis> ...` command line —
  execution and analysis stay two separate, inspectable steps.

**Scope boundary, stated as a rule rather than a v1/v2 cutoff:** `wspy-sweep` only ever automates axes
that are process-scoped and side-effect-free outside the measured run. `--affinity` clears that bar —
`sched_setaffinity()` on the forked child only, nothing outlives the run, no other process on the
machine is affected. Governor and kernel version both fail it, for two different reasons: kernel version
can't be changed without a reboot at all (not a capability gap, a hard impossibility for a tool running
on the current boot); governor is a global sysfs write that affects every other process on the machine
and persists after `wspy` exits — a measurement tool being in the business of mutating shared system
state like that is a different, larger feature with its own safety design, not a natural extension of
this one. Left genuinely open-ended, not merely deferred to a later phase.

### AMD IBS `ldlat` hardware minimum, and filtered-vs-unfiltered validation (Zen5, 4.2)
Attempting the long-carried-forward "compare filtered vs. unfiltered IBS sample distributions on real
hardware" validation immediately hit `--ibs-memory-deep`'s filtered `ibs_op` counter failing to open
(`errno=22`/`EINVAL`) on real Zen5 hardware (family 1a model 70). A bit-by-bit `perf_event_open()` sweep
of the `ldlat` config field (bypassing wspy entirely) found a clean, reproducible threshold: every value
100–127 is rejected, every value ≥ 128 succeeds — the kernel enforces a real minimum load-latency
threshold of 128 for `ibs_op`. `ibs.h`'s `IBS_DEFAULT_LDLAT_THRESHOLD` was **120**, below that minimum,
so every `--ibs-memory-deep` run that didn't explicitly override `--ibs-ldlat` had been silently failing
to open the filtered counter (degrading to 2/3 measured — not a fatal error, so this went unnoticed).
Fixed: default bumped to 128. `IBS_DEFAULT_FETCHLAT_THRESHOLD` (also 120) is deliberately left unchanged
— no hardware available exposed a working `fetchlat` sysfs format field on `ibs_fetch` to test the same
way (see `INVESTIGATION.md`'s "Known gaps").

With the fix, the originally-requested comparison now works: a deliberately cache-unfriendly
pointer-chase workload (256MB randomized permutation cycle) showed `ibs_op_accepted_ratio` averaging
~6.8% across 3 trials (0.0630/0.0662/0.0750) versus ~2.6% for an idle `sleep` baseline
(0.0425/0.0190/0.0176) over 3 trials each — non-overlapping ranges, confirming the l3missonly+ldlat
filter's accepted-ratio signal genuinely tracks real memory-bound behavior rather than sampling noise.

### `wspy-validate`/`wspy-ledger` exercised at accumulated real scale (4.2)
Built up a real `--run-index`-accumulated file (100+ genuine `wspy` runs, mixed successful/failing/
varied workloads) rather than relying only on `test_ledger.c`/`test_validate.c`'s small synthetic
fixtures.

**Interrupted runs:** a process killed well before reaching the manifest/run-index write phase leaves
no trace (clean, expected) across 150 trials at randomized early-startup timing; a further ~250 trials
with a precise `clock_nanosleep`-timed `SIGKILL` deliberately swept across the exact
`sleep(2)`-pre-launch-boundary/record-write window — every resulting run-index line remained valid
JSONL with zero corruption, consistent with `run_index.c`'s buffered-then-single-flush write pattern
being effectively atomic in practice for typical record sizes (not claimed mathematically provable).

**Mixed schema versions:** hand-stamped real records with a same-major/older-minor version (1.4.0,
1.0.0, predating structured configuration provenance/affinity) were silently tolerated with no warning,
exactly as designed; a genuinely different-major version (2.0.0) triggered `wspy-ledger`'s
one-time-per-distinct-version warning (not per-record) without affecting `--strict`'s exit code; a
record with no `schema_version` field triggered its own one-time warning; a hand-truncated malformed
JSON line was skipped with a line-numbered error rather than aborting the rest of the file.
`wspy-validate` against 5 manifests spanning current/old-minor/major-mismatch/missing-field/truncated
variants behaved identically: major-mismatch is `WARN` not `FAIL`, missing-field is `FAIL`, truncated
JSON fails with a precise parse-error location, and every other manifest in the batch still gets a full,
independent report. No bugs found — this validation confirms existing designed behavior rather than
fixing anything.

### GPU multi-device enumeration on a real multi-GPU host (4.2)
Built `AMDGPU=1 NVIDIA=1` and ran against a real laptop with both an AMD iGPU (Strix 880M/890M, sysfs
`card2`) and an NVIDIA dGPU (RTX 5070 Laptop GPU) present simultaneously. `--gpu-device=2`/
`--gpu-nvidia-device=0` (the correct indices) select the right card on each backend and report real,
distinct data (`gpu_*` and `nv_*` CSV columns coexisting on the same row, including under `--interval`);
an out-of-range index and a nonexistent NVIDIA index both degrade gracefully (logged error, zero-valued
columns, run continues) rather than crashing or silently reading the wrong device. Running both GPU
backends' counters alongside real IPC/topdown hardware counters confirmed no interaction between the
GPU and PMU collection paths.

Surfaced one real bug: `wspy --capabilities`' AMD sysfs device list never showed which device was
selected (unlike the AMD SMI/NVIDIA NVML lists right next to it), because `run_capabilities_probe()`
(`wspy.c`) called `amd_sysfs_print_capability_report()` without ever calling `amd_sysfs_initialize()`
first. Fixed by adding the missing initialize/finalize pair, matching the `amd_smi_*`/`nvidia_nvml_*`
pattern immediately below it.

Also confirms a real-hardware finding that is not a wspy bug: on this specific AMD Strix APU, the ROCm
`amd_smi` backend's `gpu_metrics` blob query fails with `AMDSMI_STATUS_UNEXPECTED_DATA` (43) —
`--gpu-smi`'s `gpu_smi_temp`/`gpu_smi_activity` degrade to 0 as designed while
`gpu_smi_vram_used`/`gpu_smi_vram_total` (a separate ROCm query) still succeed; the plain-sysfs backend
is unaffected. Confirmed via `./test_amd_smi.sh`/`./test_nvidia_nvml.sh` and the full `./run_tests.sh`
matrix (default + `AMDGPU=1` + `NVIDIA=1` builds), all passing.

### Small correctness fixes found during 4.2 hand-testing
- **`--gpu-smi --interval` CSV column-count fix:** `timer_callback()` (`topdown.c`) never read
  `amd_smi` per tick, only in aggregate mode, so a periodic `--gpu-smi --interval` row silently missed
  the 4 columns (`gpu_smi_temp`/`gpu_smi_activity`/`gpu_smi_vram_used`/`gpu_smi_vram_total`) the header
  claims. Fixed to mirror the aggregate/tail-row block exactly, positioned to match column order.
- **`--interval` tail-print/last-tick signal race:** `wspy.c` now blocks `SIGALRM` and disarms the
  periodic timer (`sigprocmask`/`alarm(0)`) as the very first thing after the blocking wait for the
  child returns, before any of the final tail row's `fprintf()` calls — `is_still_running=0` alone only
  stopped the *next* re-arm, it didn't retract a `SIGALRM` the kernel had already queued, so a signal
  delivered partway through the tail row could let `timer_callback()` splice a full periodic row into
  the middle of it. **Validation note:** three escalating black-box reproduction attempts (natural
  near-boundary timing, external `SIGALRM` injection around child exit, sustained injection across the
  whole process lifetime — several dozen trials, thousands of signal deliveries each) did not trigger
  the malformed-line symptom even on the pre-fix binary, so this fix is verified by code-level reasoning
  and the full test suite rather than an empirical repro of the race itself. The narrowed window is not
  claimed to be mathematically zero.
- **`deep-gpu` now carries `--power`:** `wspy-run`'s `deep-gpu` systemtime pass was missing `--power`
  even though it's the exact same zero-hardware-counter shape as `deep-cpu`'s systemtime pass (which
  already carried it) — a pre-existing asymmetry, not a deliberate difference. Also fixed
  `web/server.py`'s `POWER_PRESET_NAMES` (had been silently skipping `deep-gpu`'s power probe) and
  `web/joblib.py`'s `PROFILE_PLOTTABLE_COLUMNS`.
- **Web launcher custom GPU checklist gained an NVIDIA checkbox:** the "GPU metrics" card only exposed
  AMD's `--gpu-busy`/`--gpu-metrics`/`--gpu-smi` checkboxes, so a custom (non-preset) run had no way to
  opt into `--gpu-nvidia` — only presets that hardcode it (`gpu-compute`) got NVIDIA data. Added the
  missing checkbox; the "Device index" field stays AMD-only, NVIDIA always uses its default device.

### Phoronix `result_notifier` hook capture: real-host findings (4.2)
`scripts/pts_hooks/{pre,post}_test_run.sh` (PTS `result_notifier` hook scripts) and
`scripts/setup_phoronix_hooks.sh` (one-time host registration helper) landed ahead of the full
`wspy-phoronix-segment` item (see `INVESTIGATION.md`'s 4.3 infra tier). Relocation of the staging log
into a per-pass `<pass-name>.pts_hooks.log` artifact lives in `wspy-run`'s own `run_pass()` rather than
only in `workload/phoronix/run_test.sh`, so every front end that funnels through `wspy-run` (the web
launcher's preset path, `wspy-queue`, direct use) captures hook data the same way; the web launcher's
*custom checklist* path (which calls `wspy` directly) needed its own equivalent
(`_archive_stale_pts_hooks_log()`/`_capture_pts_hooks_log()`, `web/joblib.py`).

**Real-host testing found registration had never actually worked at all:** two compounding bugs, one
ours (`setup_phoronix_hooks.sh` wrote a hyphenated `modules-data/result-notifier/` directory; PTS's own
module lookup resolves the underscored `result_notifier`, matching the module's literal PHP class name,
so registration silently no-opped — fixed), one upstream (PTS's bundled `result_notifier.php`
unconditionally dereferences a null `test_result_buffer` and calls a nonexistent
`pts_test_result::get_result()`, fatally crashing `phoronix-test-suite` itself as soon as *any* real
hook script is configured — filed and fixed upstream at
[phoronix-test-suite/phoronix-test-suite#924](https://github.com/phoronix-test-suite/phoronix-test-suite/pull/924)/
[#925](https://github.com/phoronix-test-suite/phoronix-test-suite/issues/925), verified live). Until
that upstream fix ships in a release, registering the hooks on an unpatched PTS install turns "no
telemetry" into "the benchmark run crashes with zero results" — a locally-patched `result_notifier.php`
is the stopgap; the web launcher's Check button also warns about the unpatched case
(`check_phoronix_result_notifier_bug()`). Full detail: `doc/phoronix_hook_investigation.md`'s
"Implementation Update"/"Follow-up"/"Real-Host Findings" sections.

**Still open:** teaching `wspy-phoronix-segment.py` (the still-unbuilt 4.3 item) to prefer
`pts_hooks.log` over the composite.xml/log-timestamp correlation it plans to use otherwise.

### Non-obvious implementation traps found and fixed (moved from CLAUDE.md, 2026-07-21)
Two specific bugs, kept here since they're the kind of thing that could be silently reintroduced by a
similarly-shaped future change and aren't written down anywhere else.

**`getopt_long` `val` collisions silently misrouted bad flags (`wspy.c`).** `--no-phase-detect` and
`--tree-connect` had been assigned `getopt_long()` `val`s (63, 83) that collide with `'?'` (the sentinel
`getopt_long()` itself returns for any unrecognized option or missing required argument) and `'S'` (a
stray, undocumented short option that was in the optstring with no `case` to handle it). Either collision
meant a genuinely bad/malformed flag matched the wrong `case` in `parse_options()`'s switch instead of
falling through to `default: return 1` (the usage error) — confirmed live: an unrecognized flag given
alongside a real workload command printed `getopt_long`'s own "unrecognized option" line, then ran the
workload anyway. Fixed by renumbering to unused values and dropping the dead `'S'` short option. Lesson:
when adding a new long-only flag, pick a `val` that can't collide with any single-character short option
or with `'?'`/`':'`, not just "the next unused-looking number."

**A `power` PMU's dynamic `type` value coincidentally collided with `PERF_TYPE_L3`'s sentinel (`power.c`,
`wspy.c`'s `run_capabilities_probe()`).** On the dev host used for `--power` testing, the `power` PMU's
real dynamic type (read from sysfs) happened to equal `cpu_info.h`'s internal `PERF_TYPE_L3` sentinel
value, which routes `setup_counters()` through different `perf_event_open()` arguments than the generic
path. A capabilities-probe implementation that hand-duplicated a `perf_event_open()` call instead of
routing through the real `setup_counters()` missed this and reported a misleading `EINVAL` where a real
`--power` run gets the true `EACCES` (RAPL access needs root/`CAP_PERFMON`, not just `perf_event_open`
generally). Fixed by having the probe build a throwaway `power_counter_group("power")` and run it through
the actual `setup_counters()`/`coverage_entries` path rather than reimplementing the call. Lesson: any
future "probe without a full run" feature should reuse the real setup path rather than hand-rolling a
second `perf_event_open()`, since this codebase's per-vendor/per-PMU dynamic-type dispatch has sharp
edges a naive duplicate won't know about.

**A hosting provider's edge proxy silently drops the `Authorization` HTTP header before PHP ever sees
it, breaking WordPress Application Password REST auth with a generic "not logged in" (`web/wp_client.py`,
4.3 Tier 3 static-site publishing, found live on `mvermeulen.org`'s IONOS shared hosting, 2026-07-31).**
Every documented server-side fix for a missing `Authorization` header assumes it's merely *renamed* by
Apache's rewrite engine (`REDIRECT_HTTP_AUTHORIZATION`, recoverable via a `wp-config.php` snippet) or
dropped only between Apache and PHP-CGI/FastCGI (recoverable via `.htaccess`'s `CGIPassAuth On` or a
`RewriteRule`/`RewriteCond %{HTTP:Authorization}` pair) — both were tried here and neither had any effect.
A temporary debug plugin dumping every `$_SERVER` key containing `AUTH` on a real request (with and
without the header set) proved the true cause: on this host the header is dropped upstream of Apache
entirely, at the edge proxy layer, so there is nothing left in `$_SERVER` for any Apache- or PHP-level
trick to recover — confirmed by the identical (empty) result whether the client sent a correct
Application Password, a wrong one, or nothing at all. Fixed by working around the proxy instead of fighting
it: the client (`wspy-publish`) sends the identical Basic-Auth value under a second, custom
`X-WSPY-Authorization` header (which the proxy has no reason to strip), and a small WordPress plugin
(`scripts/wp-auth-bridge.php`, installed active on the target site, not part of this codebase's own
build/test) copies it back into `HTTP_AUTHORIZATION`/`PHP_AUTH_USER`/`PHP_AUTH_PW` early in the request —
the exact variables WordPress's own Application Passwords code already knows how to check, so no auth
logic needed reimplementing. Lesson: when an `Authorization` header goes missing, dump `$_SERVER` directly
(a one-file, no-dependency diagnostic) before reaching for a fix — "renamed" vs. "dropped upstream of the
webserver" look identical from the client side (same generic 401) but need entirely different fixes, and
guessing through the renamed-header fixes first cost two full round-trips here before the debug plugin
settled it in one.

### Intel hybrid / counter-grouping real-hardware findings and fixes (4.3, "carlsbad", 2026-07-22)
Real Intel hybrid hardware became available for the first time this cycle (a Raptor Lake HX host,
codenamed "carlsbad") and immediately surfaced a cluster of confirmed, hardware-verified counter-
grouping bugs in `topdown.c` — these predate this investigation entirely (the shared-group design dates
to commit `273e9af`, Dec 2023) and were never caught before because no Intel hardware existed in this
environment to exercise them.

**Finding 1 — `--per-core` on Intel silently measured only the first core.** `intel_group_id`, a
module-static "current Intel perf-group leader fd," was scoped to outlive a single `setup_counters()`
call; `--per-core`'s setup loop calls `setup_counters()` once per eligible core back-to-back with no
`close_counters()` in between, so every core after the first tried to open its counters as members of a
group led by a *different* CPU's fd — the kernel requires group members to share their leader's
cpu/task target, so those opens failed `EINVAL` silently. Fix: reset `intel_group_id = -1` at the top of
every `setup_counters()` call.

**Finding 2 — topdown/topdown2 silently reported all-zero whenever any other Intel counter opened
first.** Intel's Perf Metrics fixed-counter feature (`slots` + its `core.topdown-*` sub-events) is a
genuine kernel-enforced special case: those sub-events are only valid as members of a group whose
*literal* leader is `slots` itself. Because every Intel group funneled into one shared `intel_group_id`
regardless of which group opened first, and `ipc` (default-on, list-ordered ahead of `topdown`) opens
its own `instructions` event first, `slots`'s sub-metrics tried to join a group led by `instructions`
and failed — silently zeroing `--topdown`'s output in its single most common invocation. Fix: a second,
dedicated leader variable (`intel_topdown_group_id`) scoped to exactly the groups whose mask includes
`COUNTER_TOPDOWN`/`COUNTER_TOPDOWN2`.

**Finding 3 — the single-shared-Intel-group design cascades into wholesale counter loss once the
combined group exceeds real hardware PMU capacity, and cannot mix counters across different underlying
PMUs.** A perf event *group* requires every member to be simultaneously schedulable — no within-group
multiplexing by design — so once that's impossible the kernel refuses further members with `EINVAL`
rather than degrading gracefully. Confirmed live: a realistic multi-group combo
(`--counters=dcache,icache,tlb,branch,cache2`) measured only 10/19 counters, with one *whole* group
(`cache`, 9 counters) failing 0/9 outright rather than partial degradation; `wspy --capabilities`
(`COUNTER_ALL`) makes this maximally visible (20/48 available). Separately, `--power` (aggregate) tries
to put RAPL's `energy-pkg` event — a *different* dynamic PMU (`type=35` on this host, not the
general-purpose `cpu`/`cpu_core` PMU) — into the same shared group as whatever opened first; a perf
group can't span two PMUs, so `energy-pkg` fails `EINVAL` whenever anything else is set up in the same
call (this specific RAPL-scope symptom's actual root cause is Finding 4 below — the grouping fix here
stops RAPL from fighting for a slot in Intel's general group, but doesn't by itself fix RAPL's own
`pid=0` scope bug). Fix: moved Intel away from "one shared group across every requested counter" toward
AMD's model (ungrouped, independently-multiplexed general-purpose events, with only the topdown
Perf-Metrics family kept as its own small dedicated group per Finding 2) — `cache_counter_group()`/
`raw_counter_group()` now chunk Intel counters into hardware-budget-respecting groups
(`is_group_leader` every `available_counters`/`num_counters_available` counters, same as AMD/ARM
already did) instead of one unbounded shared group, while the topdown/topdown2 Perf Metrics family
stays exactly one dedicated group regardless of size (a kernel-enforced "literal `slots` leader"
requirement, not a PMC-budget one). This also let `setup_counters()` drop the `intel_group_id`/
`intel_topdown_group_id` module statics entirely in favor of the same `is_group_leader`-driven local
`group_id` AMD/ARM already used — structurally removing the class of cross-call state-leak bug Finding
1 above had to patch.

**Finding 4 — RAPL/`energy-pkg` opened with the wrong scope (`pid=0` instead of `pid=-1`) on Intel.**
Confirmed even fully isolated (`--power --no-ipc`, ruling out Finding 3). `setup_counters()`'s dispatch
only special-cased system-wide/uncore PMU semantics (`pid=-1`) for `pe.type == PERF_TYPE_L3`
specifically; every other counter, including RAPL's `power` PMU (whose driver sets `task_ctx_nr =
perf_invalid_context` and rejects task-scoped opens outright), fell through to the generic per-process
branch. **Not actually Intel-specific** — on the author's AMD dev host, the `power` PMU's dynamic type
apparently *coincidentally* equalled `PERF_TYPE_L3`'s sentinel (14), routing it through the correct
branch by accident (the same coincidence independently documented in `manifest.c`'s own history); on
this Intel host `power`'s real type is 35, doesn't collide, and took the wrong branch. Any host — AMD or
Intel — where the `power` PMU's type doesn't happen to equal 14 hit this identically; it was only ever
masked by chance. Fix: a new explicit `requires_system_wide` marker (`struct counter_info`,
`cpu_info.h`) set by `power_counter_group()`/`ibs_counter_group()`/`raw_counter_group()`'s AMD L3
entries, replacing the incidental `PERF_TYPE_L3` type-value match entirely — verified live on carlsbad:
the real `perf_event_open()` failure changed from `EINVAL` (wrong args, rejected regardless of
privilege) to `EACCES` (right args, just needs `CAP_PERFMON`/root), exactly the signature this finding
predicted.

**Finding 5 — Intel topdown-family counters intermittently read back as zero/`-nan` despite full counter
coverage — root-caused 2026-07-22, turned out to be two independent things, not one.** The
corrupt-percentage half was a real code bug: the originally-observed
`spec_pipeline_pct=72407003082176.4` wasn't a divide-by-near-zero/denominator issue as first guessed —
it was `print_topdown()`'s Intel L2 splits (`backend_cpu`/`speculation_pipeline`/`frontend_bandwidth`/
`retire_fastpath`) using plain unsigned subtraction instead of the `safe_sub()` clamp AMD's equivalent
code already uses, wrapping to near-`ULONG_MAX` whenever an independently-read child counter ticks
fractionally above its parent. Reproduced live with a branch-heavy workload
(`spec_pipeline_pct=173616230410.5`) and fixed by routing all four L2 splits through `safe_sub()`
(regression test `test_intel_topdown_l2_underflow` confirmed to fail pre-fix and pass post-fix). The
all-zero/`-nan` half is real but isn't Perf-Metrics-specific, contrary to this finding's original "plain
hardware/raw groups haven't shown this" read — plain `--ipc` (ordinary `PERF_TYPE_HARDWARE`, no
fixed-counter family involved) reproduces the identical symptom at a similar rate; this half is a
documented, non-actionable perf-subsystem limitation, not a wspy bug — see `INVESTIGATION.md`'s "Known
gaps" for the full write-up (kernel-level counter-scheduling timing against short-lived children, not
fixable from userspace).

**Per-core-type-aware raw event tables.** `cpu_core`'s dynamic PMU type is `4` on this host (which
happens to equal `PERF_TYPE_RAW`'s own numeric value — the likely reason `intel_raw_events[]`'s
hardcoded `PERF_TYPE_RAW` had silently "worked" for P-cores despite never doing a real per-core
PMU-type lookup the way `cpu_info.c` already did for ARM); `cpu_atom`'s type is `10` — confirmed
different, and every event in `intel_raw_events[]` was P-core-only-correct, so Gracemont E-cores needed
their own encodings entirely. Fix: `cpu_info.c` now resolves each core's real dynamic PMU type the same
way it already did for ARM (reusing `mark_cpus_for_pmu()`); a new `intel_atom_raw_events[]` (`topdown.c`)
carries Gracemont-correct encodings for instructions/cpu-cycles/topdown (4 fields, no L2
breakdown)/branch/L2 — every value read directly off this host's live `cpu_atom` PMU
(`/sys/devices/cpu_atom/events/`, `perf stat -vv`), not guessed; `raw_counter_group()`/
`setup_counter_groups()` gained a `core_class` parameter to select it. Gracemont has no `slots`/
fixed-counter register at all — `print_topdown()` now synthesizes one from `cpu-cycles * 5`, a width
measured empirically (4.9997 across 4 independent real runs). Verified live via `strace`: E-core opens
now show `type=0xa` (10) with Gracemont's own configs, P-cores unchanged.
`core_is_per_core_eligible()` no longer excludes `CORE_INTEL_ATOM`.

Findings 1-4 and half of 5 (the underflow fix) shipped this cycle; finding 5's other half is a
documented, non-actionable perf-subsystem limitation (`INVESTIGATION.md`'s "Known gaps"), not open
backlog. The E-core raw-event gap above (not one of the original 5 findings) has also shipped — nothing
remained open from this pass, which also removed the last blocker from 4.3 Tier 2's "Core-class-aware
topdown" item (later shipped, see `INVESTIGATION.md`'s "What shipped in 4.3").

## Validation narratives (4.3-era)

Full design/validation detail for every "What shipped in 4.3" pointer-list entry in
`INVESTIGATION.md`, moved here once the 4.3 backlog emptied out -- same convention as the
4.0/4.1/4.2 archive above. One `###` heading per originally-shipped item, in original shipped
order; body text is otherwise unedited from its original `INVESTIGATION.md` "Shipped since 4.2"
entry.

### Intel counter-grouping correctness fixes
**Intel counter-grouping correctness fixes:** two hardware-verified bugs found on real Intel hybrid
hardware, both in `topdown.c` (PR #129) — `--per-core` silently measuring only the first core
(`intel_group_id`, a shared perf-group-leader fd, bled across back-to-back `setup_counters()` calls);
`--topdown`/`--topdown2` reporting all-zero whenever any other Intel counter opened first (Intel's Perf
Metrics `slots` sub-events require a literal `slots`-led group, and `ipc`'s default-on event usually
won leadership instead). Verified against real Raptor Lake HX hardware.

### Intel counter-group budget chunking
**Intel counter-group budget chunking:** the third originally-found bug (`topdown.c`) — Intel's
single-shared-group design funneled every non-topdown counter into one perf event group with no size
limit, cascading into wholesale `EINVAL` loss once a combined group exceeded real hardware PMU capacity
or spanned two different underlying PMUs. `cache_counter_group()`/`raw_counter_group()` now chunk Intel
counters into hardware-budget-respecting groups (`is_group_leader` every `available_counters`/
`num_counters_available` counters, same as AMD/ARM already did) instead of one unbounded shared group,
while the topdown/topdown2 Perf Metrics family stays exactly one dedicated group regardless of size (a
kernel-enforced "literal `slots` leader" requirement, not a PMC-budget one). This also let
`setup_counters()` drop the `intel_group_id`/`intel_topdown_group_id` module statics entirely in favor
of the same `is_group_leader`-driven local `group_id` AMD/ARM already used — structurally removing the
class of cross-call state-leak bug the first fix above had to patch.

### x86 hybrid core-type detection
**x86 hybrid core-type detection:** `--affinity=coretype=<id>`/`--list-affinity` (`affinity.c`) now
detect Intel P-core/E-core and AMD Zen5/Zen5c core-type groups on x86 by reusing `cpu_info.c`'s
existing per-core vendor classification when the ARM-only `MIDR_EL1` pass finds nothing — previously
x86 always reported 0 core types. Verified against a real 32-thread Intel P-core/E-core host (16+16
threads, both `coretype=0|1` resolving correctly); the web UI's discovery endpoint carries the new
vendor-tagged core types too.

### Intel L2 topdown unsigned-underflow fix (`topdown.c`, Intel counter-grouping correctness track)
**Intel L2 topdown unsigned-underflow fix (`topdown.c`, Intel counter-grouping correctness track):**
`print_topdown()`'s `VENDOR_INTEL` branch computed its four L2 splits (`backend_cpu`, `speculation_pipeline`,
`frontend_bandwidth`, `retire_fastpath`) as plain `unsigned long` subtraction of two independently-read,
independently-multiplexed perf counters — the exact bug class `safe_sub()` was introduced to fix on
AMD's side (see that function's own comment), just never applied when the Intel branch was written.
When a "child" counter reads fractionally above its "parent" (routine measurement noise), the
subtraction wraps to near-`ULONG_MAX` and the resulting percentage explodes — reproduced live on
carlsbad with a branch-heavy workload (`spec_pipeline_pct=173616230410.5`, same signature as the
originally-logged `72407003082176.4`). Fix: route all four through the already-existing `safe_sub()`.
Regression test (`test_intel_topdown_l2_underflow`) confirmed to fail pre-fix and pass post-fix.

### RAPL/`energy-pkg` wrong-scope fix (`cpu_info.h`/`topdown.c`/`power.c`/`ibs.c`, Intel...
**RAPL/`energy-pkg` wrong-scope fix (`cpu_info.h`/`topdown.c`/`power.c`/`ibs.c`, Intel counter-grouping
correctness track):** `setup_counters()` special-cased system-wide (`pid=-1`) opens only for
`pe.type == PERF_TYPE_L3` specifically — an incidental type-value match, not a real marker. RAPL's
`power` PMU (whose driver sets `task_ctx_nr = perf_invalid_context`, rejecting task-scoped opens
outright) fell through to the generic per-process (`pid=0`) branch on any host where its dynamic PMU
type didn't happen to numerically collide with `PERF_TYPE_L3`'s sentinel value (14) — true on this
Intel host (`power`'s real type is 35), not Intel-specific (any AMD host without the same lucky
collision hits it too). Fix: a new explicit `requires_system_wide` bit (`struct counter_info`,
`cpu_info.h`) set by `power_counter_group()` (RAPL), `ibs_counter_group()` (AMD IBS, documented
system-wide-only), and `raw_counter_group()`'s AMD L3 entries; `setup_counters()`'s dispatch now checks
this marker directly instead of the `PERF_TYPE_L3` coincidence, which it no longer references at all.
Verified live on carlsbad: `perf_event_open()`'s failure mode changed from `EINVAL` (wrong pid/cpu args,
rejected regardless of privilege) to `EACCES` (correct args, just needs `CAP_PERFMON`/root) — exactly
the signature this bug's own diagnosis predicted.

### Per-core-type-aware Intel raw event tables
**Per-core-type-aware Intel raw event tables — Gracemont E-core support (`cpu_info.c`/`topdown.c`/
`wspy.c`, Intel counter-grouping correctness track):** `intel_raw_events[]` hardcoded `PERF_TYPE_RAW` as
every raw event's `device_type` — silently correct for P-cores only because `cpu_core`'s real dynamic
PMU type (4) happens to numerically equal `PERF_TYPE_RAW`'s own enum value; `cpu_atom`'s real type is
10, and Gracemont E-cores needed entirely different raw event *encodings*, not just a different
`device_type`. `core_is_per_core_eligible()` (`wspy.c`) excluded `CORE_INTEL_ATOM` outright to avoid
silently mismeasuring E-cores with P-core-only-correct events. Every encoding below was read directly
off carlsbad's live `cpu_atom` PMU (`/sys/devices/cpu_atom/events/`, `perf stat -e cpu_atom/<name>/ -vv`
for the resolved raw config), not guessed, and cross-checked against real counter runs. Two genuine
hardware differences from P-cores, not just a `device_type` substitution: (1) **Gracemont has no
`slots`/fixed-counter "Perf Metrics" register at all** (`cpu_core` exposes 20 named perf events
including `slots`; `cpu_atom` exposes 14, none named `slots`) — its 4 topdown events
(`topdown-retiring`/`-bad-spec`/`-fe-bound`/`-be-bound`) are ordinary counting events
(`event=0xc2/0x73/0x71/0x74`), not members of any kernel-enforced "literal slots leader" group;
`print_topdown()`'s `VENDOR_INTEL` case now synthesizes a slots denominator from `cpu-cycles * 5`
(mirroring AMD's `cpu-cycles * width` pattern) when no real `slots` counter is found — the width was
measured empirically: `(retiring+bad-spec+fe-bound+be-bound)/cpu-cycles` across 4 independent real runs
on this host measured 4.9997 every time. No L2 topdown breakdown exists on Gracemont at all (confirmed
absent from both sysfs and `perf list`) — those columns stay `0.0` for E-core rows via the same "vendor
doesn't populate this" convention ARM already relies on. (2) `l2_request.all`/`.miss` and the
`br_inst_retired.*`/`br_misp_retired.*` family exist on `cpu_atom` too, but with different umask
encodings than P-core for the same event numbers (e.g. `l2_request.all`: P-core `umask=0xff` vs.
Gracemont `umask=0x0`). Fix: `cpu_info.c`'s existing Raptor/Alder Lake hybrid-detection block now also
resolves each core's real dynamic PMU type via `/sys/devices/<pmu>/type` (reusing the same vendor-
agnostic `mark_cpus_for_pmu()` helper `discover_arm_pmu_topology()` already used for ARM) instead of
leaving `pmu_type` at its never-resolved `PERF_TYPE_RAW` default; a new `intel_atom_raw_events[]`
(`topdown.c`) carries the verified Gracemont encodings; `raw_counter_group()`/`setup_counter_groups()`
gained a `core_class` parameter to select it (`CORE_INTEL_ATOM`) instead of `intel_raw_events[]`, and
skip the P-core-only forced-single-group chunking rule for it. `core_is_per_core_eligible()` no longer
excludes `CORE_INTEL_ATOM`. `COUNTER_TOPDOWN_BE` (`--topdown-backend`'s `exe_activity.*`/
`memory_activity.*`) has no confirmed Gracemont equivalent — deliberately left as zero coverage rather
than guessed, same philosophy as the rest of this fix. Verified live on carlsbad end to end, with
`CAP_PERFMON`: `--per-core --topdown --affinity=coretype=1` on real E-cores reports sane, real values
(e.g. `retire=13.7,frontend=28.7,backend=44.2,speculate=13.3%,confidence=1.00,sanity=99.8%`,
`224/224 counters_measured`); `--per-core --topdown2` shows real non-zero L2 breakdown on P-core rows
(unchanged) alongside the correct, honest `0.0` L2 columns on E-core rows in the *same* run;
`--per-core --branch --cache2` reports real, plausible, vendor-distinct branch-miss/L2-miss rates on
both core types. `strace` additionally confirmed E-core `perf_event_open()` calls use `type=0xa` (10,
`cpu_atom`'s real dynamic type) with Gracemont's own configs
(`0xc0`/`0x3c`/`0xc2`/`0x73`/`0x71`/`0x74` for instructions/cycles/topdown,
`0xc4`/`0xc5`/`0x7ec4`/`0xebc4` for branch, `0x24`/`0x124` for L2).

### Phoronix per-test option-combination count (`ledger.c`, Tier 7)
**Phoronix per-test option-combination count (`ledger.c`, Tier 7):** `wspy-ledger --phoronix-option-combos`
scans the same `--phoronix-profiles-dir` tree `--unavailable-deps` already reads and reports each matching
workload's full option-matrix size — the product of every `<TestSettings>/<Option>`'s `<Menu>` `<Entry>`
count in its `test-definition.xml` — as a new `option_combinations`/`option_combinations_note` pair (CSV) or
a bracketed suffix (human report), surfaced up front instead of discovered partway through a long
batch-run sweep. Verified against real installed profiles: blender-1.2.1's own test-definition.xml
(`system/blender-1.2.1`) resolves to exactly the confirmed 5×2=10. An `<Option>` with no `<Menu>` at all
(a free-form input like fio's "Disk Target" or iperf's "Server Address") can't be enumerated, so it's
excluded from the product and the count is reported as a lower bound (`combo_has_freeform`) rather than
silently undercounted; if a workload name matches more than one installed profile (different suites or
kept versions) and they disagree on the count, `combo_ambiguous` flags it rather than silently picking
one. Purely static `test-definition.xml` parsing, same "deliberately not a real XML parser" convention
`scan_phoronix_dependencies()` already established for this file — no need to run anything. Independent of
`--unavailable-deps`; both share `resolve_profiles_dir()`'s profiles-dir resolution and may be given
together. Feeding this count into the (shipped) `--tree` pass timeout item's `BATCH_RUN_MULTIPLIER` —
replacing that item's blind 5.0 guess with a grounded number — remains a follow-up, not yet wired up.

### openbenchmarking.org-seeded single-test-point Phoronix suites, front-end phase...
**openbenchmarking.org-seeded single-test-point Phoronix suites, front-end phase
(`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab, Tier 7):** decomposes an already-published
OpenBenchmarking result or an installed/exported Phoronix suite into one minimal single-test-point suite
per (test, option-combination), materialized under `workload/phoronix/<test>/<options>/` and registered
with `wspy-ledger --add`. Three source methods (a result URL/ID, an XML file already on disk, or an
installed PTS suite under `~/.phoronix-test-suite/test-suites/`) all reduce to the same
`(test_id, arguments)` pair list, parsed from either of the two real Phoronix XML shapes (suite-definition
`<Execute>` elements or composite/result-file `<Result>` elements) — verified against real files on a live
PTS install, including a real fetched OpenBenchmarking result. Idempotent/additive re-runs (an existing
`<test>/<options>/` directory is left untouched and reported `exists`), and a per-test `installed` flag
(`phoronix-test-suite info`) surfaces which materialized points still need `phoronix-test-suite install`
run by hand. The directory-name slug for a long `Arguments` string (a common failure mode: two option
combinations sharing a long prefix, e.g. a model file path, differing only near the end) gets a short
content hash appended whenever it would otherwise be truncated, rather than silently truncating and
colliding two different combinations into one slug — confirmed live against a real OpenVINO result
where every one of 12 model/precision combinations' `-hint throughput`/`-hint latency` pair collided
under a plain 60-character truncation before this fix.

The Phoronix tab also shows a live inventory of already-materialized test points
(`joblib.list_materialized_phoronix_test_points()`), each with a "Use in Run tab" button that copies its
suite into `~/.phoronix-test-suite/test-suites/local/<identity>/` (`joblib.
copy_phoronix_test_point_to_local_suite()`) and prefills the Run tab's workload/suite/benchmark fields —
closing most of the gap to the "runner script" half this item originally deferred, without the invasive
change a literal `--output-root` override would have needed (the web launcher's report page, `/compare`,
and bundle export all hard-assume one fixed `--output-root`). A run launched that way still writes its
real files under the normal `--output-root` (`suite=phoronix`, `benchmark=<identity>` — every existing
route keeps working unchanged) and additionally gets symlinked at
`workload/phoronix/<test>/<options>/runs/<run-id>/` (`joblib.link_phoronix_test_point_run()`, best-effort)
purely so it's still browsable as a subdirectory of the test point directory. Because each generated
suite is exactly one test point, its wspy capture is already segmented at the source — no post-hoc
composite.xml/log-timestamp correlation needed for runs built this way (doesn't replace
`wspy-phoronix-segment` for suites run the ordinary multi-test-point way, but sidesteps the problem
entirely for anything built through this path). Longer-term, once enough `<test-name>/<options-info>`
directories accumulate this way they form a pre-profiled library keyed on real Phoronix test identity —
a natural feed for Tier 2's clustering/nearest-neighbor work once that lands, and a cheaper way to grow
`wspy-ledger`'s workload coverage than hand-authoring one-off `wspy-run` invocations per benchmark.

Also writes one `workload/phoronix/<test>/README.md` per bare test name (one directory level above the
`<options>/` subdirectories, since it describes the test regardless of option combination), rendered from
the same `phoronix-test-suite info <test_id>` call `installed` already used (`fetch_phoronix_info_fields()`/
`write_phoronix_test_readme()`, `joblib.py`) — the test's Description plus a handful of other high-level
fields (type, license, supported platforms, project/OpenBenchmarking.org links). Same idempotent/additive
convention as `suite-definition.xml`: an existing README is left untouched on re-import. Skipped (not an
error) when `--no-check-installed` disabled the `info` lookup or the lookup itself failed.

The inventory table itself is now grouped by test rather than shown as one flat, recency-ordered list of
option combinations — `list_materialized_phoronix_test_points()`'s own ordering (newest `generated_at`
first) is the right default for "what did I just import" but stopped scaling once a host accumulates
hundreds of points across many imports and a human is instead trying to find one specific test.
`joblib.group_materialized_phoronix_points_by_test()` re-buckets that flat list into
`<test>` → `<options>` groups sorted alphabetically by bare test name (each group's own points re-sorted by
options slug); `joblib.read_phoronix_test_description()` pulls each group's one-line summary straight back
out of the README.md above, so the inventory doesn't need a second `phoronix-test-suite info` round-trip
just to label a collapsed group. Each test renders as a `<details>` block (`render_phoronix_inventory_groups()`,
`web/server.py`) with a text filter above matching test name/description/option-slug substrings
(`wirePhoronixTab()`, `web/static/app.js` — client-side only, no new endpoint), plus expand-all/collapse-all
buttons.

### `cache_counter_group()`'s "instructions" entry opened at the wrong PMU type
**`cache_counter_group()`'s "instructions" entry opened at the wrong PMU type (`topdown.c`) —
vendor-agnostic, not Intel-specific, though surfaced by the same real Coremark run above.** The
synthetic `"instructions"` entry `cache_events[]` carries alongside its `PERF_TYPE_HW_CACHE` rows (a
genuine `PERF_TYPE_HARDWARE`/`PERF_COUNT_HW_INSTRUCTIONS` event, used only as `print_cache()`'s "N per
1000 inst" denominator) never had a `device_type` set, so `setup_counters()` opened it at the whole
group's fixed `PERF_TYPE_HW_CACHE` type instead — and `PERF_COUNT_HW_INSTRUCTIONS` (`1`) numerically
collides with L1I-read-access's own `PERF_TYPE_HW_CACHE` encoding (also `1`, confirmed against
`<linux/perf_event.h>`), so it silently requested a duplicate of `l1i-read` rather than real instruction
retirement, on every vendor. Fix: `cache_counter_group()` now records each `cache_events[]` entry's real
`type_id` per-counter; `setup_counters()`'s `pe.type` resolution now also honors it for
`PERF_TYPE_HW_CACHE` groups (previously that per-counter override only applied to `PERF_TYPE_RAW`
groups).

### AMD IBS sampling-mode support
**AMD IBS sampling-mode support (`ibs_sample.c`/`ibs_sample.h`, PRs #143/#144) — done for this cycle,
removed from the open backlog.** `--ibs-sample` mmaps the perf ring buffer with `PERF_SAMPLE_RAW`
(nothing in wspy read a perf mmap ring buffer before this) and decodes each sample's fixed-offset
fields into end-of-run rate estimates: op-side `dc_miss`/`dc_l1tlb_miss`/`dc_l2tlb_miss`/`op_brn_misp`
(of branch-retiring ops) plus `IbsOpData2`'s two scheme-independent signals (`dram_rate`,
`remote_node_rate`); fetch-side `ic_miss`/`l1tlb_miss`/`l2tlb_miss`. Draining only happens once, at
end-of-run (never from `timer_callback()`'s `SIGALRM` handler — ring parsing isn't async-signal-safe
and wspy has no poll loop), so `--ibs-sample` + `--interval` zeroes periodic rows and populates only
the final tail row — documented, not silent. `IbsOpData2`'s full named category breakdown (its meaning
differs between pre-Zen4 and Zen4+/`zen4_ibs_extensions` hardware, confirmed against the kernel's own
decoder) is decoded scheme-agnostically but only *named* in human-readable output, never baked into a
permanent CSV column. The remaining raw-address fields (`IbsBrTarget`/`IbsDcLinAd`/`IbsDcPhysAd`) and
`IbsOpData4` (no documented bitfield layout) are deliberately out of scope permanently, not deferred —
see the Zen5/IBS deep-dive's item 6 for the full reasoning. `--interval`-integrated periodic rates
(needs a real poll-loop architectural change) is split out and tracked in "Deferred indefinitely" below.

### `--ibs-sample` wired into a real `wspy-run` profile, plus its own `CAP_PERFMON` permission...
**`--ibs-sample` wired into a real `wspy-run` profile, plus its own `CAP_PERFMON` permission
requirement discovered and fixed.** New `ibs-sample` builtin profile (`wspy-run`: `--ibs-sample
--no-ipc`, deliberately no `--csv`/`--interval` — the named breakdown is human-readable-only and the
ring buffer only drains at end-of-run); `zen4plus-deep` now composes `deep-cpu,ibs-sample,tree-heavy`
in place of the counting-mode `ibs-memory-deep` it originally shipped with (that profile's own
composition, "Shipped since 4.2" above), with `--no-rusage` added to just its own `ibs` pass since
`deep-cpu`'s `counters` pass already captures the same elapsed/utime/stime block. Real-host testing
found `--ibs-sample` (like `--ibs-basic`/`--ibs-memory-deep`) needs root or `CAP_PERFMON` — IBS is
system-wide-only monitoring, and `perfmon_capable()` gates that identically to `--power`'s RAPL access;
confirmed live end-to-end (not just the `EACCES` symptom): granting `cap_perfmon=ep` to the `wspy`
binary took `--ibs-sample` from `EACCES` on every counter to real sample counts.
`scripts/setup_perf.sh`'s existing `CAP_PERFMON` grant (previously described as `--power`-only) now
documents covering IBS too — one grant, no script changes needed beyond the description. Also fixed a
real bug this surfaced in the web launcher's Check button: `probe_ibs()` only recognized counting-mode's
`counters_measured`/`counters_requested` CSV trailer, so a genuine `--ibs-sample` permission failure was
masked as "could not parse counter coverage" instead of the actionable `CAP_PERFMON` hint — fixed by
checking for open failures before attempting to parse coverage, and by probing with `--csv` regardless
of the real pass's own flags (that trailer is generic across every counter group, not counting-mode-
specific).

### Baselines and regression/anomaly detection
**Baselines and regression/anomaly detection (`wspy-summary --check-regression`, PR #148) — done for
this cycle, removed from the open backlog.** New standalone `wspy-summary --check-regression
<hostname>:<run_id>` mode (mutually exclusive with `--trace`) compares a run's own per-metric values
against a *rolling* baseline — every strictly-earlier run sharing the same `--group-by`/
`--group-by-option` bucket the target run itself belongs to, reusing `compute_stats()`/`compute_ci95()`/
`compute_verdict()` verbatim rather than inventing new statistics. No new store schema: "which
historical runs count as comparable" is defined identically to the existing group-by summary table, not
by a separately invented matching-key set. A metric outside the baseline's 95% CI is classified
`above`/`below` — deliberately direction-neutral, not asserted as "regression," since this codebase has
no per-metric higher-is-better/lower-is-better table anywhere and inventing one now would repeat the
same cross-scheme category-mapping risk `--ibs-sample`'s `IbsOpData2` decode already avoided (see above)
— the tool reports the CI deviation and its direction, leaving "is this actually bad" to a human or a
future per-metric semantics table. Verified against real `wspy-store` data end-to-end, including a
genuine flagged deviation from a zero-variance baseline.

### Machine/environment comparability scoring
**Machine/environment comparability scoring (`env_score`/`mixed-env`, PR #149) — done for this cycle,
removed from the open backlog.** The deferred, *scored* version of 4.2's exact-match "mixed-pmu" bucket
check, extended across `run_environment`'s fuller provenance surface (BIOS, microcode, governor, memory,
virtualization) rather than just PMU setup — no diff/score between two runs' environments existed
anywhere in the codebase before this. Extends the existing `struct bucket`/`compute_verdict()` machinery
rather than adding a new CLI mode: `env_score` is a default-on column on every bucket row, in both the
main group-by table and `--check-regression`'s baseline rows. Unweighted fraction of 8 tracked fields
(`virt_role`, `hypervisor_vendor`, `microcode_version`, `bios_vendor`/`bios_version`/`bios_date`,
`cpu_governor`, `memory_total_kb`) that agreed across a bucket's contributing runs — deliberately not a
hand-tuned per-field point scheme, the same reasoning that kept IBS sampling-mode's cross-scheme category
mapping out of a permanent CSV schema. `memory_total_kb` uses a 5% tolerance (routine firmware/DIMM
jitter); every other field is exact match; a field only counts once mutually comparable
(`hypervisor_vendor` self-excludes on host runs with zero special-casing). A bucket with zero comparable
fields gets an explicit no-data sentinel, never a fabricated 0%/100% — absence of provenance data is not
evidence of a mismatch. New `--min-env-score` flag, default 0.8. Also fixed a real pre-existing bug found
while researching this: `upsert_run_environment()` (`store.c`) silently collapsed a genuine JSON `null`
for `memory_total_kb`/`cpu_governor_uniform` to `0`/`false`, indistinguishable from a real measurement —
every text provenance field already preserved `null` correctly via `bind_text_or_null()`, these two
numeric/bool fields had no equivalent. Verified against real `wspy-store` data end-to-end.

### Distribution-first reporting: per-run IPC quantile features
**Distribution-first reporting: per-run IPC quantile features (`ipc_p10`/`ipc_p90`/`ipc_iqr`, PR #150)
— done for this cycle, removed from the open backlog.** Extends `extract_run_features()` (`store.c`) —
the feature vocabulary `wspy-archetype` already consumes and this doc calls the clustering groundwork —
with per-run distribution-shape features derived from a single run's own `--interval` tick data, rather
than today's per-metric means. Clustering-prep feature-vocabulary richness, not clustering itself (the
next Tier 1 item). Scoped to `ipc` only for this first pass (the most universally-collected metric,
already the primary signal `phase.c`/`print_topdown()` use elsewhere) via a per-metric helper
(`extract_quantile_features_for_metric()`) that's a one-line extension per additional metric later.
Linear-interpolation percentiles (R-7/numpy-default method); requires >=4 `--interval` ticks
(`QUANTILE_MIN_TICKS`) or all three features record unavailable rather than a fabricated value from too
few points. The extraction query's `core IS NULL` guard is load-bearing, not decorative:
`metric_values.row_index` means a genuine tick ordinal for `--interval` runs but one core's snapshot for
`--per-core` runs — without it, a `--per-core` run's cross-core spread would be silently quantiled as if
it were a time series, colliding with `parallelism_proxy`'s already-covered signal under a different
feature name. `FEATURE_SET_VERSION` 1.1→1.2 (informational only). Verified against a real `--interval`
run end-to-end: sorted IPC ticks `[0.15,0.15,0.17,0.23,0.87,0.93]` produced `p10=0.15`/`p90=0.9`/
`iqr=0.555`, matching hand-computed expected values exactly.

### Nearest-neighbor search
**Nearest-neighbor search (`wspy-archetype --nearest`, PR #152) — the nearest-neighbor slice of Tier 1
item 1 below; K-means clustering and cluster profile cards remain open, rescoped there.** New standalone
`wspy-archetype --nearest <host>:<run_id> [--k N]` mode: given a target run, ranks every other run in the
store by a coverage-aware distance computed only over the `run_features` both runs actually have
`measured` — root-*mean*-square (dividing by the shared-feature count, not taking a root-sum-square) over
z-score-standardized differences, so a pair sharing 6 features isn't penalized purely for sharing less
than a pair sharing 18. Population mean/stddev for standardization are computed once across every loaded
candidate, not per-pair, so every pairwise distance shares the same scale; a zero-variance feature
(identical value across every candidate) is excluded rather than causing a divide-by-zero. The
`compared_features` shared-dimension count is always reported alongside each distance — coverage
transparency, matching the `env_score`/baseline-`n` convention already established elsewhere in this
codebase, rather than silently hidden. Mirrors `--run`'s colon-parsing and not-found exit-code
conventions; `--command`/`--hostname` reused as candidate-pool filters. Clustering/cluster-profile-cards
deliberately left out of this pass: K-means with a common-subspace distance has no clean textbook answer
for how a centroid should be computed when cluster members don't all share the same available features —
real, separate design work, not wasted effort building this distance function first. Verified via 8 new
`test_archetype.c` cases (basic ranking, common-subspace RMS normalization, zero-shared-feature exclusion,
zero-variance no-crash, `--k` limiting, `--command`/`--hostname` filters, target-not-found,
target-with-no-measured-features) plus the full `run_tests.sh` matrix.

### K-means clustering + cluster profile cards
**K-means clustering + cluster profile cards (`wspy-archetype --kmeans`) — closes out Tier 1's clustering item,
the last item in Tier 1; Tier 1 is now fully shipped for 4.3.** New standalone `wspy-archetype --kmeans
<n> [--seed <n>] [--iterations <n>]` mode partitions every candidate run into `n` clusters over the same
coverage-aware z-standardized distance `--nearest` uses, then prints one row per member (grouped by
cluster, closest-to-centroid first) carrying a "profile card" of the `KMEANS_TOP_FEATURES` dimensions
where that cluster's centroid sits furthest from the population mean. Resolves the design question this
item was deferred over: a centroid, per dimension, is the *available-case mean* — averaged only over
whichever cluster members actually measured that dimension, so a dimension no member measured simply
doesn't appear in the centroid rather than requiring a fabricated value. This is the standard "partial
distance strategy" for clustering under missing data (Dixon 1979; also underlies Hathaway & Bezdek's
fuzzy-c-means-with-missing-data extension), not a novel invention — just not previously wired up here,
and it mirrors `nearest_neighbor_distance()`'s own "only shared dimensions count" idiom on the assignment
side too (`centroid_distance()`, same RMS-over-shared-dimensions/zero-variance-exclusion shape).
Initialization is k-means++ (Arthur & Vassilvitskii 2007), seeded from real data points rather than a
fabricated average point (a real point is always well-defined on its own measured dimensions, sidestepping
the missing-data question for the one spot it would otherwise bite hardest: a centroid with zero members
yet to average over); `--seed` makes a given `(data,k)` pair reproducible via `rand_r()`-based sampling
independent of any other code's RNG state. Empty clusters (a real Lloyd's-algorithm failure mode) are
healed each iteration by stealing the single worst-fit point from a cluster with more than one member,
rather than letting a cluster's centroid drift somewhere no point wants it. Verified via 5 new
`test_archetype.c` cases (well-separated groups land in distinct clusters, heterogeneous-coverage
centroid computation doesn't crash, `k` greater than candidate count returns the standard "insufficient
data" exit code, same-seed determinism, and `k` larger than the natural group count still yields `k`
non-empty clusters) plus the full `run_tests.sh` matrix.

### Phase-aware topdown
**Phase-aware topdown (`wspy-summary --phase-topdown`) — closes 4.3 Tier 2's phase-aware topdown
item.** New standalone
`wspy-summary --phase-topdown <hostname>:<run_id>` mode: breaks one run's own topdown output down by
`--interval` phase (warmup/steady/degraded, `phase.c`'s per-tick classification). store.c's
`ingest_csv_metrics()` already tagged every `metric_values` row with its tick's phase label the moment
`phase.c` shipped (the CSV's `phase` column is carried straight through, no schema change needed here)
— this is simply the first thing that actually reads that column back out and correlates it with
topdown specifically, closing the topdown deep-dive's long-deferred MVP criterion ("one benchmark run
demonstrates phase-specific topdown shifts in generated summary output"). Reports each topdown column's
per-phase mean/n (blank, never fabricated as 0, when a phase has no data for that column) plus a
`drift_pct` (largest phase-to-phase swing for that column) and a trailing note naming the single
largest drifter overall; deliberately scoped to topdown's own CSV columns
(`retire`/`frontend`/`backend`/`speculate`/`contention_pct` plus the L2 splits — the literal raw CSV
column names `metric_values.metric_name` stores, *not* the `retire_pct`-style `run_features` feature-
vocabulary names `archetype.c` reads from a separate table), not a generic phase-vs-every-metric report.
A run collected without phase data (aggregate, `--per-core`, or no `--interval`) degrades to an
explicit notice rather than a silently-empty table, same convention as `--nearest`'s "target has zero
measured features" case. Verified end-to-end against a real AMD Zen5 host run (`--interval --topdown`,
piped through `--run-index`/`wspy-store`) in addition to 6 new `test_summary.c` cases (full 3-phase
drift, largest-drift trailing note, no-phase-data graceful degradation, target-not-found, single-
phase-observed, and a metric present in only one of two observed phases) plus the full `run_tests.sh`
matrix.

### Composite attribution
**Composite attribution (`wspy-archetype`'s new `memory_attribution` axis) — the topdown+cache/TLB/IBS
cross-referencing half of 4.3 Tier 2's composite attribution item (the blocking-syscall-split modifier
that closes it out entirely is its own separate "Shipped since 4.2" entry below).**
A fifth `wspy-archetype` axis, alongside `resource_dominance`/`parallelism_shape`/`control_flow_style`/
`runtime_stability`: cross-references topdown's own `backend_pct` (the same L1 category
`resource_dominance` already ranks) against every independently-measured cache/TLB/IBS signal a run
collected — `dcache_miss_pct`/`l2_miss_pct`/`l3_miss_pct`, `itlb_miss_per1k`/`dtlb_miss_per1k` (AMD-only,
`--topdown-optlb`) and the newly-promoted `itlb_generic_miss_pct`/`dtlb_generic_miss_pct` (cross-vendor,
`--tlb` — added specifically for this item after real-hardware testing showed the AMD-only pair almost
never fires in practice, since `--topdown-optlb` is far less commonly collected than plain `--tlb`),
`smt_contention_pct`, and the newly-promoted `ibs_dc_miss_pct`/`ibs_dram_pct` (AMD IBS sampling-mode,
shipped 4.3). A "memory-bound" topdown read (`backend_pct` at/above a 20% floor) corroborated by at
least one of these independently-measured signals is materially higher-confidence than topdown alone;
one with *zero* corroboration from any signal that *was* actually collected is flagged `uncorroborated`
— genuinely new information a single-counter heuristic can't produce, since it suggests the true
bottleneck may be something not measured yet (cross-NUMA latency, lock contention masquerading as a
backend stall) rather than a straightforward capacity/miss-rate problem. `unknown` covers both
`backend_pct` never having been measured and `backend_pct` being significant but zero corroborating
signals having been collected at all (kept as two states via `.available`, even though both currently
render the same label). Deliberately does *not* attempt to rank *which* cache level a stall concentrates
in — that's a different, stall-cycle-attribution question only `--topdown-backend`'s own dedicated
`l1_bound_slots_pct`/etc. group can honestly answer (a miss *rate* and a stall-cycle *attribution* are
related but distinct measurements); conflating the two would overclaim precision this signal set doesn't
have, so this axis stays a corroboration check, not a hierarchy-level diagnosis. `memory_attribution_reasons`
lists exactly which signal(s) fired (`corroborated`) or were checked and found unremarkable
(`uncorroborated`) as `name=value`/`checked:name` pairs. Thresholds (`MEMORY_ATTRIBUTION_FLOOR_PCT=20.0`,
per-signal `*_ELEVATED_*` constants) are the same "deliberately simple v1 starting point, not derived
from a formal study" caveat `DOMINANCE_*_MARGIN`/`phase.c`'s own thresholds already carry. Verified via
11 new `test_archetype.c`/`test_store.c` cases (all four label outcomes directly, plus end-to-end through
`score_runs()`'s CSV pipeline and the two newly-promoted feature-extraction paths) and against a real AMD
Zen5 host run (`--topdown --dcache --cache2 --cache3 --tlb`, piped through `wspy-store`) — real IBS
sampling itself needs root this session couldn't grant, so the IBS corroboration path is unit-tested
only, not yet confirmed against a live IBS sample; the generic-vs-AMD-only TLB feature split above was
itself a direct finding from that real-hardware run, not anticipated in the original design.

### Core-class-aware topdown
**Core-class-aware topdown — closes 4.3 Tier 2's core-class-aware topdown item.** Two pieces, both
surfaced by re-examining what
"core-class-aware" actually requires rather than assuming the existing per-core-type raw event tables
(shipped 4.2) already covered it end to end:
1. **A real correctness gap in default (non-`--per-core`) mode on Intel hybrid hosts, now warned
   about.** Research into this item's own "weighted aggregate" framing turned up something more
   fundamental than a missing summary feature: `setup_counters()`'s default (non-`--per-core`) path
   opens raw events with `device_type` left at the literal `PERF_TYPE_RAW` constant, never patched to
   the real per-host dynamic `cpu_core`/`cpu_atom` PMU type the way `bind_core_counter_groups()`
   (`wspy.c`) already does inside the `--per-core` loop. On current hardware this happens to still bind
   correctly to the P-core PMU only because `cpu_core`'s real sysfs type coincidentally equals
   `PERF_TYPE_RAW`'s own enum value (4) — not by design. Any execution the scheduler places on an
   E-core simply can't be counted by that mismatched-PMU-type event: no error, no dropped-sample
   marker, just a silently under-counted total, with zero indication anything was missed. `topdown.c`'s
   `setup_raw_events()` now warns on `cpu_info->is_hybrid && !aflag`, mirroring the existing ARM
   `mixed_pmu_types` warning exactly (that warning turns out to guard the identical underlying
   mechanism — raw events bound to one PMU type silently not counting on a task migrated to a
   different-PMU-type core — just phrased more softly). AMD Zen5/Zen5c doesn't need the same warning:
   confirmed there's no per-core-class raw table split on that vendor yet (`raw_counter_group()`'s own
   comment), so there's no PMU-type mismatch to warn about.
2. **The weighted-aggregate/summary-presentation work itself: `wspy-core-report --weight-by <metric>`.**
   New optional flag weights each core's contribution to the existing "Cross-core stats" and "Core-class
   summary" mean/stddev/cv by that same core's own value of a chosen metric column (e.g. `ipc` or
   `cpu-cycles`) — an "activity-weighted" combination rather than counting every core equally regardless
   of how much work it actually did. min/max/hot/cold stay exactly the raw per-core values either way
   (weighting only changes how per-core values combine into one number, not which core was highest/
   lowest). A core lacking data for the weight metric is excluded from every weighted result entirely,
   never silently zero- or unity-weighted (`gather_core_values()`'s new `weight_metric` parameter).
   Population-style weighted variance (divides by `sum(weights)`, not a bias-corrected denominator) —
   same "deliberately simple, not a formal derivation" spirit `DOMINANCE_*_MARGIN`/`phase.c`'s own
   thresholds already carry. Opt-in and fully backward-compatible: default output (no `--weight-by`) is
   byte-identical to before. Verified against a real per-core `--topdown` capture on this AMD host: a
   single-busy-process workload's unweighted cross-core `ipc` mean (0.69, diluted by 31 idle cores) vs.
   `--weight-by ipc`'s activity-weighted mean (1.94, correctly dominated by the one core actually doing
   the work) — a concrete demonstration of exactly the problem this item asked to fix. 8 new
   `test_core_report.c` cases (weighted-mean correctness, raw-value min/max/hot/cold unaffected by
   weighting, zero-weights fallback, missing-weight-data exclusion) plus the full `run_tests.sh` matrix;
   the Intel hybrid warning itself has no dedicated unit test (no existing precedent for asserting on
   `warning()` text anywhere in this codebase, including the ARM warning it mirrors) but is a 3-line
   conditional gated behind a flag (`is_hybrid`) that's false on every test host today, so zero behavior
   change for any existing test.

### Composite attribution's blocking-syscall-split modifier
**Composite attribution's blocking-syscall-split modifier — closes 4.3 Tier 2's composite attribution
item entirely.** `wspy-archetype`'s `memory_attribution` axis (shipped) now folds in the
"no blocking-syscall activity" vs. "heavy blocking-syscall activity" vs. "runnable but not scheduled"
three-way split from the 4.1 critical-path work, ahead of the cache/TLB/IBS corroboration checks it
already did: a "memory-bound" topdown read on a phase/run dominated by kernel-blocking (futex/io-wait)
or scheduler run-delay isn't a genuine hardware stall at all, so asking whether cache/TLB/IBS
"corroborate" it would be misleading regardless of what they show. Two new labels, checked before the
existing `corroborated`/`uncorroborated` logic: `blocked` (heavy futex/io-wait — waiting on the kernel
is the story) and `oversubscribed` (heavy scheduler run-delay with low blocking-wait — runnable but not
given the CPU, an affinity/placement problem, not a counter-chasing one); `blocked` wins when both fire,
matching the critical-path work's own stated priority.

The real, separate plumbing work this item was waiting on: this signal lived only in `--tree`'s own raw
text output before now, never ingested into the normalized store. `store.c`'s new
`scan_tree_blocking_stats()` scans that raw `--tree` file directly for its `futex`/`io_wait`/`schedstat`
lines (topdown.c's own fixed fprintf formats, confirmed against the emission sites) and sums them into
whole-run totals — deliberately *not* reusing or shelling out to `proctree.c`'s own parser, since this
needs only a run-wide sum, not per-pid/tree-shape reconstruction (proctree.c has no such run-wide total
today either, in raw text or its `--json` export, so a new pass was needed regardless of ingestion
approach). Promoted into two new run_features, normalized by `elapsed_seconds` the same way every other
`_pct` feature is: `blocking_wait_pct` = (futex + io-wait seconds) / elapsed × 100,
`sched_rundelay_pct` = run-delay seconds / elapsed × 100. Both `unavailable` (not fabricated as 0) for
the common case of a run that never used `--tree` at all. Verified via 10 new
`test_store.c`/`test_archetype.c` cases (scanner correctness, missing-file/no-matching-lines
degradation, feature promotion end to end, all four priority-ordering outcomes including "both blocked
and oversubscribed signals fire") and against a real 32-process oversubscription capture on this host
(`--tree --tree-io-wait --tree-schedstat` against 32 busy-spin children on a 32-core host, correctly
producing `sched_rundelay_pct≈32%`) piped through `wspy-store`.

### IBS-derived memory-path bottleneck decomposition
**IBS-derived memory-path bottleneck decomposition (`wspy-archetype`'s new `memory_attribution_locus`
field) — closes 4.3 Tier 2's last remaining item.** `memory_attribution` (shipped, above) explicitly
declined to rank *which* cache level a memory-bound stall concentrates in, pointing at
`--topdown-backend`'s L1/L2/L3/DRAM stall-cycle chain (`print_topdown_be()`) as the right tool for that —
but that chain is Intel/ARM-only (`print_topdown_be()` returns immediately on `VENDOR_AMD`), and IBS is
also AMD-only, so this builds the AMD equivalent from a fundamentally different kind of data: IBS's
per-sample tags are request-outcome hits (did *this* sample miss L1D / cross to DRAM / cross a NUMA hop),
not stall-cycle attribution.

New `locus`/`locus_reasons` fields on `struct memory_attribution_result`, populated only when the axis's
existing classification already landed on `"corroborated"` *and* the newly-promoted `backend_memory_pct`
feature (the L2 memory-portion-of-backend split, a more precise anchor than the coarser `backend_pct` the
rest of the axis gates on) clears the same 20% floor `memory_attribution` itself uses. Two precision
tiers, `locus_reasons` always prefixed `tier=ibs-sample`/`tier=cache-counter` so one is never mistaken for
the other:
1. IBS tier (needs `ibs_dc_miss_pct`): `ibs_remote_node_pct` at/above 10% wins first (a cross-socket hop
   dominates regardless of where else the access resolved) → `"remote-numa"`; else `ibs_dram_pct` at/above
   10% → `"dram"`; else, only when `ibs_dram_pct` was itself measured, the residual
   `ibs_dc_miss_pct − ibs_dram_pct` at/above 10% → `"l2-l3"` (missed L1D, resolved before DRAM); else a
   plain `ibs_dc_miss_pct` at/above 10% → `"l1"`. `ibs_dc_l1tlb_miss_pct`/`ibs_dc_l2tlb_miss_pct` elevated
   appends a `tlb-cofire` tag rather than competing for the label — address translation is an independent
   hop from the data-miss chain. Falls through to tier 2 if IBS data exists but nothing in it clears its
   threshold, since `memory_attribution`'s own corroboration may have come from a signal (e.g.
   `smt_contention_pct`) with no hierarchy-position information at all.
2. Cache-counter tier (no IBS data this run): same shape over `l3_miss_pct`/`l2_miss_pct`/`dcache_miss_pct`
   at the same `CACHE_MISS_ELEVATED_PCT` cutoff `memory_attribution`'s own signal table already uses for
   these features (not a new threshold), `dtlb_generic_miss_pct` as the TLB co-firing tag.

`store.c`'s `SIMPLE_METRIC_FEATURES` table gained four new one-line rows (`ibs_dc_l1tlb_miss_pct`,
`ibs_dc_l2tlb_miss_pct`, `ibs_remote_node_pct`, `backend_memory_pct`); `FEATURE_SET_VERSION` bumped to
1.5. Deliberately out of scope for v1: fetch-side/icache decomposition (a different topdown category,
`frontend_bandwidth_pct`, with different semantics — conflating it here would repeat the exact
overclaiming-precision mistake `memory_attribution`'s own header warns against) and the raw
`op_data_src_count[13]` histogram / `IbsBrTarget`/`IbsDcLinAd`/`IbsDcPhysAd` (same scheme-ambiguity/
no-stable-bitfield reasons `ibs_sample.h` already excludes them from CSV for).

Verified via 11 new `test_archetype.c` cases (both precision tiers, the `tlb-cofire` tag, tier-2
fallthrough, the `"corroborated"`-only gate, the `backend_memory_pct` availability/floor gates) plus an
end-to-end `run_features` → CSV pipeline test, and 1 new `test_store.c` feature-promotion case, plus the
full `run_tests.sh` matrix (93 golden-output checks, 62 capability-matrix bundles including the NVIDIA
build rerun). Caught one real bug during testing: an unmeasured `ibs_dram_pct` was initially treated as
`0` when computing the L2/L3 residual, which silently misclassified a run as `"l2-l3"` when DRAM
residency was actually unmeasured rather than genuinely zero — fixed by only computing the residual when
`ibs_dram_pct` was itself actually collected, falling back to a plain `"l1"` read otherwise.

### CPU2026 workload-suite web tab
**CPU2026 workload-suite web tab (new `web/server.py`/`web/joblib.py` tab, alongside Run/Validate/
Store & Summary/Discovery/Phoronix) — closes 4.3 Tier 6's CPU2026 backlog item.** A SPEC CPU2026
counterpart to the Phoronix tab, structurally simpler: unlike a Phoronix test point, a CPU2026
benchmark and its config file both already exist locally the moment a suite is installed, so there's
nothing to fetch/materialize from a remote result — the tab's job is discovery, registration, and
action-triggering, not import. `CPU2026_BENCHMARKS` (`joblib.py`) is a static 52-benchmark catalog
(`706.stockfish_r` etc., from https://spec.org/cpu2026/docs/overview.html#benchmarks, grouped
intrate/intspeed/fprate/fpspeed) for display/grouping only — what's actually runnable is discovered live
via `discover_installed_cpu2026_benchmarks()`/`discover_cpu2026_configs()` scanning
`$SPECDIR/benchspec/CPU2026/*/` and `$SPECDIR/config/*.cfg` on disk, same "static table for labels,
filesystem for truth" split the Phoronix tab uses between `phoronix.tests.txt` and live
`phoronix-test-suite info` calls — a benchmark installed under a name the static table doesn't know
still shows up, just without suite/language labels.

Two-level inventory hierarchy (benchmark → config-tag, mirroring `workload/phoronix/<test>/<options>/`):
config tags (filename minus `.cfg`) are discovered independently of any benchmark since one SPEC config
can build any benchmark, then a benchmark×config *pairing* gets registered (not "materialized" — no
ledger, no XML) via `register_cpu2026_point()` under `workload/cpu2026/<bench>/<tag>/` (a `source.json`
provenance sidecar + one `README.md` per bench built from the static catalog's suite/language labels,
no subprocess/network round-trip needed the way Phoronix's `phoronix-test-suite info` does — idempotent,
an existing `source.json` is left untouched on re-register). `runs/<run_id>` symlinks back to real run
dirs via `link_cpu2026_point_run()`, same idiom as `link_phoronix_test_point_run()`.
`resolve_cpu2026_point_dir()` mirrors `resolve_phoronix_test_point_dir()`'s "resolve under dest_root,
require a real `source.json`" validation rather than trusting a client-supplied path. `$SPECDIR`
(default `/home/mev/cpu2026`, new `--cpu2026-dir` flag) is changeable from the tab's "Suite directory"
field via a plain GET re-render, no server restart — `cpu2026_suite_installed()` checking for
`$SPECDIR/shrc` is the install-sanity check, same role `check_phoronix_batch_config()` plays for
Phoronix. Inventory table's "Built" column (`cpu2026_benchmark_built()`) is a substring match against
`$SPECDIR/benchspec/CPU2026/<bench>/exe/*<tag>*` (SPEC's exe naming carries machine/OS suffixes like
`_base.<tag>-m64`, so it's not an exact reconstructed filename), recomputed live per inventory render
rather than cached at registration time, since a Build action changes that state afterward and
staleness here would be actively wrong rather than merely a re-check-later warning — same
cheap-check-now/re-verify-at-run-time posture `list_installed_phoronix_test_versions()` documents, but
live instead of cached given that difference.

Two actions per row: **Build** (`runcpu --config <cfg> --action=build --tune <tune> <bench>`, run as a
backgrounded subprocess via `execute_cpu2026_build()` and relayed live through a new `CPU2026_BUILDS`
SSE registry — a build isn't a wspy run at all, no report directory or manifest, so it gets its own
registry rather than reusing `RUNS`; `_stream_events()` gained a `make_report_url` parameter, always
`False` here, so this reuses the same SSE relay `execute_analyze()` uses without fabricating a bogus
report link) and **Use in Run tab** (`build_cpu2026_run_workload()` prefills the Run tab's `workload`
field, mirroring `_phoronix_use_in_run`). The one real wrinkle vs. Phoronix: `runcpu` needs SPEC's
`shrc` sourced for its environment, but neither `execute_cpu2026_build()`'s `Popen()` nor the Run tab's
`workload` field (`shlex.split()`, no shell) invoke a shell on their own, so
`build_cpu2026_shell_argv()` wraps the real command in `bash -c "source $SPECDIR/shrc && cd $SPECDIR &&
ulimit -s unlimited && runcpu ..."` — same `--action=validate --nobuild`/`ulimit -s unlimited` choices
`workload/cpu2017/run_test.sh` already makes for its own build-then-validate split. A new
`cpu2026_test_point` hidden Run-tab field (alongside the existing `phoronix_test_point` one) carries the
registry dir through `_link_cpu2026_test_point()` to a post-run symlink-back, same as
`_link_phoronix_test_point()` — deliberately *not* wired into `_enqueue_job()` yet (no
`_cpu2026_test_point_identity()` analog exists), left for a follow-up. No `wspy-ledger` integration
needed — 52 benchmarks don't need a "what's left" backlog ledger the way Phoronix's huge test catalog
does; the inventory table itself is the coverage view.

Verified via 30 new `test_joblib.py` cases across 10 new test classes (212/212 joblib tests passing:
install-sanity check, discovery, build-check substring matching, registration idempotency, path-
resolution rejection, inventory listing/grouping, run-symlinking, and argv-building for both the Build
and Use-in-Run-tab commands) plus a live smoke test against a fake SPEC install exercising discovery,
registration, inventory rendering, Use-in-Run-tab prefill, and the Build SSE stream's error handling.

### Tree viewer: cumulative time + hot-process table
**Tree viewer: cumulative time + hot-process table (`web/static/proctree_viewer.js`, PR #163) — makes
it easier to narrow to the processes where a run's user/system time actually went.** The single-tree
viewer's per-comm summary rollup (`proctree.c`'s `build_comm_table()`, already exposed as `data.summary`
in the JSON but previously only ever rendered by the diff view) is now shown as a clickable "hot
processes" table above the tree. Each node also displays cumulative (self + descendant) utime/stime and
its share of total run time, computed client-side; auto-expand now opens subtrees at/above a 5%
cumulative-time-share threshold instead of a flat depth<3 cutoff, and a new "hide branches under N%"
filter complements the existing text search. Child rendering order intentionally stays chronological
(fork order), not resorted by time. Entirely client-side — no `proctree.c`/wspy changes needed, since the
JSON already carried the required per-node/per-comm fields. Verified live via headless-Chromium
DevTools-Protocol automation against a real 403-process run.

### PID-targeted counter attachment
**PID-targeted counter attachment (`--target=comm=<name>[,cmdline=<substr>]`, PRs #164–167) — closes
4.4's "PID-targeted counter attachment for known-hot processes" item.** Once a `--tree` run's hot-process
table shows which comm dominates a run's time, `--target` attaches a second, dedicated counter group
(the same counters the run's other flags requested) to just the matching process(es), rather than only
ever reading the whole-subtree aggregate. `setup_counters()` (`topdown.c`) gained a `pid_t attach_pid`
parameter (`-1` preserves every existing call site's behavior unchanged); a `>=0` caller skips any
`requires_system_wide` counter (RAPL/AMD L3/IBS — not meaningful process-scoped) and opens the rest with
`inherit=0` against that exact pid. `target_parse_spec()`/`target_match()` implement the comm=/cmdline=
grammar (comma-separated key=value, AND semantics when both given), mirroring `affinity.c`'s spec-parsing
style. Attachment happens live inside `ptrace_loop()`'s `PTRACE_EVENT_EXEC` handling (comm/cmdline are
already readable there, same timing `--tree-cmdline` relies on) — matched processes each get their own
counter group built from whatever `counter_mask` this specific wspy invocation has active; readings are
read back and emitted as `targetcounter <group> <label> <value>` tree-file lines at that pid's own exit,
then the fds are closed immediately rather than held until end-of-run (a long matching-heavy run could
otherwise exhaust PMU slots) (PR #164). `proctree.c` parses these into an open-ended `target_counters`
array — present on both the per-process node and the per-comm rollup (`PROCTREE_JSON_SCHEMA_VERSION`
bumped 1.0.0 → 1.1.0) — since the counter set depends on whichever flags the producing run had active,
not a fixed field list the way futex/io/schedstat's extras are.

Three follow-on web changes, each surfaced by actually using the feature end-to-end: (1) the Run tab's
Process-tree card gained a matching free-text `--target` field (PR #165), which in turn surfaced a real,
unrelated latent bug in the process-tree pass's own argv builder (`web/joblib.py`): the "software
counters too" checkbox had *never* actually turned software counters on — wspy's own default
`counter_mask` is `COUNTER_IPC` only (`wspy.c`), not IPC-plus-software as the old code assumed, so the
checked case emitted no flag at all while only the unchecked case emitted a no-op `--no-software`; a
plain `--tree` run through the web UI had been collecting zero performance counters regardless of that
checkbox. (2) That single checkbox was then replaced with the same full counter-group selector the
"Performance counters" card already uses (`render_group_checkboxes()`/`counter_group_flags()`), so
`--target`'s attachment isn't limited to software-or-nothing — which also fixed `ALL_GROUPS`' `"software"`
entry (wrongly marked `default_on=True`), a bug that had silently affected the *main* "Performance
counters" card's own software checkbox too, predating `--target` entirely (PR #166). (3) The web tree
viewer's per-comm hot-process table (previous entry) renders one column per distinct counter when
`target_counters` is present; per-process detail (already in the JSON per node) now also renders in the
per-node tree view itself, toggled the same way futex/io_wait/etc. already are, and a shared
`computeDisplayCounters()` helper collapses any group's `instructions`+`cpu-cycles` pair into a single
derived `IPC` ratio (dropping the raw counters, including AMD's `ex_ret_ops`, for that group) since
those numbers are only meaningful as the ratio — no other counter-pair gets this treatment (PR #167).

This item was originally planned as two backlog entries — PID-targeting (item 10) and a later
uprobe-based argument-capture item (item 11) meant to reuse its comm/PID-match plumbing — but the
argument-capture half hadn't landed yet at the time this doc entry was written; now tracked in "Deferred
indefinitely" below (uprobe-based function-argument capture). PR #164 verified via `run_tests.sh`'s
full matrix (incl. two new `capability_matrix.sh` bundles: graceful attach, and the
`--target`-without-`--tree` fatal rejection); PRs #165/#166 verified via `web/test_joblib.py` (228/228,
6 new cases incl. a regression test for the `ALL_GROUPS` fix) plus a live web-launcher smoke test
(`/api/run-custom` → real `--target` run → `/api/tree-json` → confirmed both `ipc` and `software`
counters attach); PR #167 verified by mirroring the JS collapse logic in Python against a real run's
JSON (no JS runtime available in this environment to execute it directly), confirming both the
comm-summed and per-process cases collapse correctly while leaving other groups' raw counters intact.

### Symbol-level profiling
**Symbol-level profiling (`--symbol-sample`/`--symbol-sample-event`, `wspy-symbolize`, the tree
viewer's "Profile" drill-down, 2026-07-29/30) — closes 4.4's "Symbol-level profiling" item.** Maps
sampled events to routine/symbol names, scoped to `--target`-matched processes rather than a whole-
system `perf record`-style capture (already covered by `perf` itself; see "Symbol-level profiling
deep-dive" for the full use-case discussion). Capture: a generic `PERF_SAMPLE_IP` sampling counter,
generalized from `ibs_sample.c`'s mmap ring-buffer plumbing (now factored out into shared
`perf_ring.c`/`perf_ring.h`, with `ibs_sample.c` refactored onto it unchanged in behavior), attached
at the same `PTRACE_EVENT_EXEC` point `--target`'s counter attachment already uses; drains into
`targetsample`/`targetsamplelost`/`targetmap` tree-file lines at that pid's own `PTRACE_EVENT_EXIT`,
which `proctree.c` parses into `target_samples`/`target_samples_lost`/`target_maps` on each `tree`
JSON node (`PROCTREE_JSON_SCHEMA_VERSION` 1.1.0 → 1.2.0) — deliberately *not* rolled into the
per-`comm` `summary` totals the way `target_counters` is, since raw addresses carry per-process
ASLR/PIE load bias and summing them across pids would be actively wrong, not just imprecise.
Resolution: a new stdlib-only Python tool, `wspy-symbolize` (`wspy-analyze`/`wspy-bundle`'s own
category, not linked into wspy itself), resolves `target_samples` against `target_maps` (PIE bias:
`addr - map.start + map.file_offset`) via batched `addr2line` calls (one per binary/library, not one
per address), with five distinct non-crashing degradation reasons rather than one generic
"unresolved" bucket — see `doc/ARTIFACT_CONTRACT.md`'s "Symbol table" section for the full schema.
Web UI: `/api/symbolize/<suite>/<benchmark>/<run_id>?pid=<n>|comm=<name>` (mirrors `/api/tree-json`'s
on-demand, nothing-written-to-disk shape) plus a "▶ profile" toggle in the tree viewer on any node
carrying `target_maps`, caching its fetched result/expanded state on the node object itself so it
survives a rerender from an unrelated column toggle. Verified live end-to-end against real hardware
in this environment throughout (capture: real `--target --symbol-sample` runs against `sleep`/`yes`
workloads, including hand-checking the PIE offset arithmetic against a real `libc.so.6` map;
resolution: `wspy-symbolize` correctly dominated by `libc`'s `write`/`syscall_cancel` internals for a
`yes` busy-loop, correctly flagging this host's own stripped Rust-`coreutils` `yes` binary as
unresolved; web UI: the real, unmodified `proctree_viewer.js` executed under `gjs`
(GNOME JavaScript/SpiderMonkey) with a hand-written DOM+`fetch` shim fed real captured JSON, since no
browser was available this session — see "Symbol-level profiling deep-dive" for the full verification
narrative of each piece). This item was originally planned as two backlog entries — the capture/
resolution work itself (item 9) and a later uprobe-based argument-capture item (item 10) meant to
reuse its resolution output — but the argument-capture half hasn't landed yet; now tracked in "Deferred
indefinitely" below (uprobe-based function-argument capture).

### Tree viewer oversized-JSON handling
**Tree viewer oversized-JSON handling (`web/server.py`/`proctree_viewer.js`, 2026-07-30, PR #170) —
bugfix, found via real use rather than backlog.** A `build-linux-kernel-defconfig` run forking ~99,570
processes (a `-j`-parallel kernel build) made `_api_tree_json`'s on-demand `proctree --json` shell-out
produce a 541MB JSON blob; the browser's `fetch`/`JSON.parse` couldn't reliably complete on a response
that large, surfacing as a confusing client-side `Unexpected end of JSON input` instead of anything
actionable. Fix landed in three increments on the same branch: (1) a `TREE_VIEWER_MAX_BYTES` guard,
initially 20MB, that rejects an oversized response with a clear `413` instead of attempting to ship it;
(2) raised to 500MB once a real, smaller kernel-build run (219MB, `--tree-cmdline` only, no other
per-tick flags — same ~99.5k process count, just fewer bytes/process) needed the headroom; (3) instead
of just rejecting, `_api_tree_json` now keeps `process_count`/`summary` (comm-grouped, small regardless
of process count) and only drops the `"tree"` key, flagging `tree_omitted`/`tree_omitted_bytes` so
`proctree_viewer.js` still renders the process-count line and summary table but skips the interactive
tree, replacing it with a message pointing at `process.tree.summary.txt` (linked via the existing
`/files/` route when present) — same degrade-gracefully idiom other parts of this codebase already use
rather than hard-failing. Increment (3) also fixed a bug in the guard itself: it compared `proctree`'s
raw stdout length against the threshold, but the actual response is that data re-serialized through
`json.dumps()` afterward (which re-escapes `cmdline` strings and can inflate size several percent) —
enough that the 541MB run's 502.8MB-raw output slipped under the newly-raised 500MB limit undetected
until the check was moved to measure the real serialized size. Note this only addresses the fetch/parse
failure, not a separate, still-open render-cost concern: `proctree_viewer.js`'s `renderNode()` builds one
DOM element per process up front regardless of payload size, so a tree just under the byte limit but
still tens of thousands of processes could be slow to render even once it loads (see that file's own
header comment on deferring child-DOM construction until expand, not done here). Verified live: confirmed
the failure mode by curling `/api/tree-json` directly against the real 541MB run (valid-but-oversized
JSON, matching the reported truncation symptom); re-verified after each increment against that run plus
a smaller in-limit kernel-build run and a normal small run (`coremark-default`) to confirm non-oversized
trees stay unaffected; user confirmed the tree viewer renders correctly end-to-end in a real browser
post-fix.

### `wspy-analyze`: fixed a "thinking" model silently producing an empty `aianalysis.<model>.txt`...
**`wspy-analyze`: fixed a "thinking" model silently producing an empty `aianalysis.<model>.txt`
(`ollama_generate()`/`analyze_one_model()`).** Root-caused live (2026-08-01) against the real first
published `/workload` report (WordPress page id=35, `phoronix/coremark-default`): `gpt-oss:20b` drafts
its answer inside Ollama's separate `"thinking"` stream field before ever emitting `"response"` text,
and Ollama was running it with only a 4096-token context window (confirmed against the loaded model's
own `llama-server` process args) — for this codebase's multi-KB prompt, the model exhausted that budget
still mid-thought (`done_reason: "length"`), so `response` stayed permanently empty with no exception
anywhere. Replayed the exact real prompt directly against `/api/generate` to confirm both the failure
and the fix: request now sends `options.num_ctx` (new `--num-ctx`, default 16384) — confirmed this alone
turns `done_reason` from `"length"` into a normal `"stop"` with real output. Defense in depth: if
generation is still cut short before any visible response text exists, `analyze_one_model()` now skips
writing the file and warns instead of silently succeeding empty. `test_ai_analyze.sh` still passes;
re-ran the real failing case through the actual CLI post-fix (real 1.3KB narrative, not 0 bytes).

### New pre-computed report artifacts + a "default curation" button
**New pre-computed report artifacts + a "default curation" button** — a bundle of three small, related
additions from iterating on that same first published report, filling gaps found while trying to reuse
its curation as a starting template for future reports:
- `wspy-run` now writes `command.txt` alongside `manifest.json` — the same workload argv
  `manifest.json`'s own `"command"` array already records, as one copy-pasteable shell-quoted line
  (`printf '%q '`) rather than a JSON array a reader has to mentally reassemble; shows up in the
  curation studio's "+ add" list as "command line".
- `web/joblib.py`'s `run_proctree_besteffort()` gains two more best-effort derived views alongside the
  existing `process.tree.summary.txt`/`process.tree.simple.txt`: `process.tree.top.txt` (individual
  processes — not proctree's own per-comm rollup — ranked by actual CPU time, `utime_seconds+
  stime_seconds`) and `process.tree.top1pct.txt` (the full tree pruned to just those top processes plus
  their ancestor chain, every other subtree collapsed to a "N more process(es) omitted" line at the
  level it was hidden). "Top 1%" is `top_fraction=0.01` clamped to `[5, 50]` processes
  (`PROCESS_TREE_TOP_FRACTION`/`_MIN`/`_MAX`) so it stays readable on both a tiny run and the
  ~155K-process stress run `doc/ARTIFACT_CONTRACT.md` mentions — a starting default the author expects
  to tune, not a fixed policy. The pruning walk is a single bottom-up "does this subtree contain a kept
  pid" pass before printing, not repeated per-node subtree scans, to stay O(process_count) rather than
  O(process_count²) on that same large-tree case. Verified against a real 640-process tree (from a real
  `ollama-subset` run): the 727-line raw tree collapses to a 15-line pruned view.
- The curation studio (`render_studio()`) gained an "Apply default curation" button
  (`build_default_curation_blocks()`, `apply_studio_post()`'s new `default-curation` op) — one click
  replaces the current curation with a fixed starting set: every pass's own raw text/CSV output,
  `command.txt`, every AI narrative analysis (not prompt/critique files), every plot, all at
  `depth="full"`. A policy over `collect_run_files()`'s *kinds* (this run's own recorded passes, the
  `ai_generated` flag, `plots/` membership) rather than hardcoded filenames, since different `wspy-run`
  profiles name their passes differently — deliberately *not* a literal reproduction of page 35's exact
  hand-picked 3-plot/2-text-block selection (which would need per-profile filename knowledge this
  function doesn't have), a known, accepted gap given the author's own framing that this default will
  need tuning over time. Always replaces, never merges — confirmed working end to end through the real
  running server (`curl`-submitted the real form POST against the real `coremark-default` run:
  correctly produced all 5 pass outputs + `aianalysis.gpt-oss_20b.txt` + all 14 plots).

New tests: `web/test_studio_curation.py` (`build_default_curation_blocks()`), `web/test_joblib.py`
(`render_top_processes_text()`/`render_top1pct_tree_text()`, `classify_bundle_kind()`/
`collect_run_files()` for the two new filenames). Not done in this pass: re-publishing WordPress page
35 itself with the fixed analysis/new curation (held off deliberately — its curation will change again
once these land, no point publishing twice), and the page's sidebar/full-width layout question (a
WordPress theme/Customizer setting outside what this codebase's publishing code controls, left for the
author to check in wp-admin directly).

### Default curation moved into an editable config file
**Default curation moved into an editable config file** — the "default we'll want to change over
time" gap flagged above got hit for real on a `zen4plus-deep` run: reviewing the button's output
against a real 20-artifact run surfaced that only ~10 of them were actually wanted, in a different
order than the kind-based policy produced. `build_default_curation_blocks()` now reads
`web/default_curation.conf` (`load_default_curation_config()`) — an ordered, plain-text list of
filenames/glob patterns, one per line, `#`-prefixed for a documented exclusion (kept in the file
rather than deleted, so re-enabling something is a one-character edit) and `##`-prefixed for section
headers/prose. Config-listed artifacts are added in file order; everything the config doesn't mention
(a pass output from a profile it hasn't been tuned for, a plot template it doesn't list, additional AI
narrative variants) still falls through to the pre-config kind-based logic, appended after — nothing
disappears just because the config is incomplete for a given run. The shipped config also folds in two
artifact kinds the studio could already produce but the original policy never offered:
`process.tree.top1pct.txt`/`process.tree.top.txt` (the tree-heavy pass's derived process-list views)
and `aianalysis.*.txt` (any model's narrative, via glob — silently skipped if not generated yet, not
an error). New tests: `web/test_studio_curation.py`'s `LoadDefaultCurationConfigTest` (parser) plus
three new `BuildDefaultCurationBlocksTest` cases (config ordering/exclusion, exclusion not
reintroduced by the fallback loop, unlisted artifacts still auto-appended).

### Composite-preset process-tree auto-generation bug + manual retrigger button
**Composite-preset process-tree auto-generation bug + manual retrigger button** — found while chasing
why a real `zen4plus-deep` run's process-tree views (above) never showed up:
`execute_profile_run()` (`web/joblib.py`) only auto-ran `run_proctree_besteffort()` when the *raw*
profile string was literally `"tree-heavy"`/`"gpu-compute"`, but the web launcher only ever submits a
composite preset's own name (`"zen4plus-deep"`), never its expanded `"deep-cpu,ibs-sample,tree-heavy"`
pass list — `server.py` already had `COMPOSITE_PRESET_PROFILES`/`_expand_preset_names()` for exactly
this (its own IBS/power probe tables needed it), `joblib.py`'s post-processing step just wasn't using
it. Both moved into `joblib.py` (renamed `expand_preset_names()`, still imported back into `server.py`)
and `execute_profile_run()` now expands before checking. Also added a "Generate process-tree views"
button on the report page (`render_proctree_card()`, wherever `process.tree.txt` exists) — synchronous
`POST /api/proctree-views/<suite>/<benchmark>/<run_id>` (`_proctree_views()`), not SSE, since this is
local proctree/Python rendering, not an LLM call — so a run from before this fix (or before the button
existed at all) can get its views generated retroactively without re-running the benchmark. Requests
every proctree annotation flag unconditionally; proctree treats an unrequested-at-collection-time flag
as "absent", not an error, so the maximal request is safe without knowing which `--tree-*` flags the
original run used. Verified end-to-end against a real `process.tree.txt` (root-less `wspy --tree`)
through the live HTTP server, both the button and the 400/404 guard paths. New test:
`web/test_joblib.py`'s `ExpandPresetNamesTest`.

### wspy-analyze output rendered as real Markdown, not dumped verbatim
**wspy-analyze output rendered as real Markdown, not dumped verbatim** — the report page, HTML export,
and WordPress export all just `html.escape()`'d wspy-analyze's `.txt` output into a `<pre>` block, so a
model's `**bold**`/table syntax showed up as literal asterisks and pipes (surfaced publishing a real
report: gpt-oss:20b's narrative is markdown-flavored by default, primed by the prompt templates' own
markdown headers). Fix, in three parts:
- wspy-analyze now writes `.md` (`aianalysis.<model>.md`, `aiprompt.md`, and their `.critique`/
  `.compare.` variants) instead of `.txt` — content was always markdown, the extension now says so.
  `joblib.py`'s `AIANALYSIS_RE`/`AIPROMPT_CRITIQUE_RE`/`ai_artifact_label()` match either extension
  (no regression for an existing run's `.txt` narrative — it keeps being recognized/labeled/curated
  exactly as before, just rendered as plain text rather than parsed markdown, same as any other `.txt`
  block); `guess_kind()` maps `.md` to a new `"markdown"` kind. `default_curation.conf` lists both
  extensions for the AI-narrative slot.
- New `web/markdown_lite.py`: a small stdlib-only Markdown → HTML/WordPress-block converter (this repo
  has no non-stdlib Python dependency to reach for). Deliberately not full CommonMark — ATX headings,
  paragraphs, bold/italic/inline-code spans, single-level lists, GFM pipe tables, fenced code, hr; no
  nested lists/blockquotes/links, since none of those show up in this tool's actual prompt/response
  shape. `to_html()` for the report page/HTML export; `to_wp_blocks()` decomposes into native Gutenberg
  blocks (`wp:heading`/`wp:paragraph`/`wp:list`/`wp:table`/`wp:code`) rather than one opaque raw-HTML
  blob, matching how every other content type in `render_export_wordpress()` already works — stays
  editable in the WP block editor afterward. 26 new tests (`web/test_markdown_lite.py`).
- `prompts/perf_analysis.tmpl`/`perf_analysis2.tmpl` (versions bumped to 1.2/2.1) now explicitly ask
  for a Markdown response, including real `|`-delimited tables over ASCII-art ones. `perf_analysis2.tmpl`
  (the structured, numbered-section template) is now wspy-analyze's default (`DEFAULT_TEMPLATE`) instead
  of the short-prose `perf_analysis.tmpl` — the web UI's template `<select>` reordered to match. Verified
  against a real live Ollama call (`test_ai_analyze.sh`, `qwen2.5-coder:3b`): real `.md` output, template
  v2.1 by default. Not done: a one-time rename/backfill tool for pre-existing `.txt` narrative files —
  they keep working, just without the new rendering, per the author's own call when scoping this.

### wspy-analyze: AMD IBS counting-mode CSV data now reaches the AI narrative prompt
**wspy-analyze: AMD IBS counting-mode CSV data now reaches the AI narrative prompt** (PR #182) — a new
streaming per-column summarizer (`summarize_csv()`) feeds `ibs.csv` min/max/mean/stddev into the prompt,
closing a gap where IBS counting-mode data was invisible to the narrative. See
`doc/INVESTIGATION_ARCHIVE.md` for the full write-up.

### `wspy-testpoint`: full run-selection/aggregation/rendering pipeline for a test point
**`wspy-testpoint`: full run-selection/aggregation/rendering pipeline for a test point** (PRs #183-186)
— `select-runs` assigns each linked run a role (stats-pool/supplementary/excluded/primary) so a redo or
a differently-scoped run never pollutes statistics; `aggregate`/`render` compute and curate a
`README.md` from the resolved run set, including a cross-run archetype-stability signal. Closed out
(now-former) Tier 3 item 5. See `doc/INVESTIGATION_ARCHIVE.md` for the full design and PR-by-PR
write-up.

### Web UI "Publish test-point report" button
**Web UI "Publish test-point report" button** (PR #187) — wires the `wspy-testpoint` pipeline above
into the report page: a card runs `select-runs`/`render` in a background thread with live SSE output,
same shape as the existing AI-narrative-analysis card. Closes (now-former) Tier 3 item 6's
write-path/trigger scope (the WordPress page-hierarchy bug is a separate, still-open item).

### WordPress REST publishing primitives
**WordPress REST publishing primitives** (`web/wp_client.py`, `wspy-publish`) — authenticated page
create/update/draft/publish, media upload, Gutenberg-block content reuse from the existing exporter, and
a per-report "Publish to WordPress" button in the web UI, all verified against the real
`mvermeulen.org/workload` site. See `doc/INVESTIGATION_ARCHIVE.md` for the full 8-step build history.
Does not include the site-wide automated pipeline (walking the whole store to publish/update suite- and
cross-suite-level pages) Tier 3 item 2 originally asked for — that shipped later, see
`scripts/publish_reference_matrix.py`'s own "Shipped since 4.2" write-up below.

### cpu2026 level-3 benchmark pages + `wp_client.publish_page_at_path()`
**cpu2026 level-3 benchmark pages + `wp_client.publish_page_at_path()`** (PR #188) — materializes
`doc/REPORT_HIERARCHY.md`'s level-3 pages for all 52 real SPEC CPU2026 benchmarks, on both the
file-system report-root and the live site (`scripts/publish_cpu2026_benchmarks.py`), verified end to end
and published live. New `publish_page_at_path()` is the hierarchy-aware create-with-content primitive
that was missing (walks/auto-creates parent stubs, then sets content on the leaf) — closed the
now-former Tier 3 item 6's underlying gap in spirit; the web UI "Publish to WordPress" button has since
switched onto it too (see below), fully closing that item.

### Phoronix level-3/4 hierarchy pages
**Phoronix level-3/4 hierarchy pages** (PR #189) — `scripts/publish_phoronix_pages.py`, second use of
the cpu2026 recipe, confirming `publish_page_at_path()` generalizes: level-3 (97 materialized tests,
content reused from `write_phoronix_test_readme()`'s existing output — closing `doc/REPORT_HIERARCHY.md`'s
own flagged migration debt) and level-4 (test-points that actually have a linked run, 2 of 443
materialized — the rest are never-benchmarked option-combination noise), all published live, no changes
needed to `wp_client.py`/`report_root.py`/`wspy-publish` themselves.

### Web UI "Publish to WordPress" button now nests correctly
**Web UI "Publish to WordPress" button now nests correctly** (PR #190) — fixes the now-former Tier 3
item 6: the button's form previously always published at WordPress root (`parent_id` defaulted to `0`,
had to be typed in by hand). Replaced with a required `machine` field; the full
`suite/test/test-point/machine/run-id` path resolves automatically via `joblib.resolve_test_identity()`
(moved from `wspy-testpoint`, now shared) and publishes via `publish_page_at_path()`, nesting each run's
page as a child of its auto-created machine stub. Verified against the real site — confirmed the created
page's parent chain resolves correctly leaf-to-root via the WP REST API. `publish-page --slug`'s flat
CLI path stays a deliberately simple primitive, unchanged.

### Machine catalog pages
**Machine catalog pages** (PR #191) — `scripts/publish_machine_page.py` resolves
`doc/REPORT_HIERARCHY.md`'s two long-open machine-level questions: a `/machine/` index plus one
`/machine/<short-name>/` detail page per physical machine (run locally, since hardware detection can't
describe a machine it isn't running on), named `<vendor>-<short-model>-<ram-gib>gb` to disambiguate
machines sharing a chip but differing in memory. Auto-created machine stubs inside the suite hierarchy
now link back to their catalog entry (`publish_page_at_path()`'s new `stub_content` parameter). A new
`machine_short_name` field in `~/.config/wspy/publish.json` (`save_config()` now shared, moved out of
`wspy-publish`) lets both the CLI and the web UI's publish form remember a machine once registered,
instead of retyping it every time. Verified live against the real site.

### Phoronix test-point stub auto-populated at publish time
**Phoronix test-point stub auto-populated at publish time** (PR #192) — publishing a run already gave
its auto-created machine-level page a real catalog link (PR #191); now the test-point-level page above
it (e.g. `/phoronix/openssl/sha256/`) also gets real content (`test_id`/`arguments`) automatically,
instead of staying empty until a separate manual `scripts/publish_phoronix_pages.py` re-run. New
`joblib.find_materialized_phoronix_test_point()`/`test_point_wp_content()` (the latter moved out of the
script, now shared). Phoronix only for now — verified live against the real site.

### cpu2026 identity resolution fixed
**cpu2026 identity resolution fixed** (PR #193) — `resolve_test_identity()` never special-cased cpu2026;
publishing a cpu2026 run would have created a wrongly-named sibling page instead of nesting under the
real `cpu2026/<bench>/` page. New `joblib.find_materialized_cpu2026_point()` (mirrors the Phoronix one)
splits identity into `(bench, "tag-tune")` correctly, and `cpu2026_test_point_wp_content()` gives the
auto-created test-point stub real config content too, same parity as PR #192. Caught by the user before
trying a new SPEC benchmark; not yet verified against the real site (no such run exists yet), covered by
unit tests only for now.

### `wspy-testpoint aggregate`/`render` resolve a run's actual per-pass store run_ids
**`wspy-testpoint aggregate`/`render` resolve a run's actual per-pass store run_ids** (PR #194) —
`select-runs`/`runs.json` name a run by its `wspy-run` directory, but `wspy-store`'s `runs` table keys
each row by the underlying `wspy` invocation's own run-index `run_id`, generated independently per
collection pass and never equal to the directory name; `aggregate`/`render` were passing the directory
name straight through as `--run-id`, so every multi-pass run (the norm for anything collected via
`wspy-run --suite/--benchmark`, cpu2026 or Phoronix alike) reported "not found in store" even after
correct ingestion. New `resolve_store_pass_rows()` correlates a run directory to its real store rows by
matching `output_path`/`manifest_path` under `<suite>/<benchmark>/<run_id>/`, falling back to an exact
`(hostname, run_id)` match for a bare wspy run dropped into the layout by hand (what the existing smoke-
test fixtures already modeled); `aggregate`/`render` now expand each stats-pool run into all of its
resolved pass ids, and the archetype cross-run section picks one representative pass (the `"counters"`
configuration when present). Caught by the user trying to publish a real 707.ntest_r cpu2026 test-point
report through the pipeline PRs #183-186 built; new regression coverage in `tests/testpoint_smoke.sh`
reproduces the exact pre-fix failure. Verified live retrying that same publish attempt — which then
surfaced a second, separate identity-resolution bug this fix didn't touch (PR #196 below).

### "Publish test-point report" pre-fills Machine slug from config
**"Publish test-point report" pre-fills Machine slug from config** (PR #195) — the card's Machine field
was always blank, even though `wp_client.load_config()`'s `machine_short_name` (set once via
`wspy-publish configure`) already backs the sibling single-run "Publish to WordPress" panel's own
Machine field. Same read, same still-editable/required text input — just removes retyping a slug this
web layer already had on hand. Verified live: a real report page now renders the field pre-filled from
the real config.

### `wspy-testpoint` threads `--cpu2026-dest-root` through to identity resolution
**`wspy-testpoint` threads `--cpu2026-dest-root` through to identity resolution** (PR #196) —
`resolve_test_point_paths()` called `joblib.resolve_test_identity()` with only `--phoronix-dest-root`,
never a cpu2026 one; PR #193 fixed this exact call site in `web/server.py`'s single-run publish path,
but `wspy-testpoint`'s own CLI (what the "Publish test-point report" button shells out to) never got the
same follow-up. Every cpu2026 benchmark identity therefore hit `resolve_test_identity()`'s generic
fallback (`test=<full identity>`, `test_point="default"`), landing as a wrongly-named sibling directory
instead of nesting under its real `<bench>/` page — caught live publishing a real 707.ntest_r test-point
report. New `--cpu2026-dest-root` flag (default matches `web/server.py`'s own `CPU2026_DEST_ROOT`),
threaded through the same way `--phoronix-dest-root` already was; new regression coverage in
`tests/testpoint_smoke.sh`. Verified live: hierarchy confirmed correct on retry; the wrongly-nested tree
the pre-fix code had already committed into the real report-root was manually cleaned up (git-committed
removal, local-only, same "never push automatically" convention as every other `wspy-testpoint` commit).

### WordPress idempotent content-merge protection
**WordPress idempotent content-merge protection** (PR #197) — closes Tier 3 item 2's "idempotent content
merge" gap. `publish_page_content()` always did an
unconditional `update_page()` on an existing page, with no way to tell "nothing's changed since our last
write" from "a human hand-edited this in wp-admin since" — any repeat publish would silently clobber a
hand-edit. New fingerprint tracking (`~/.config/wspy/publish_state.json`, page id → sha256 of the
content wspy itself last confirmed live there): before overwriting, refetches the page's current raw
content and compares against the recorded fingerprint, raising `WPContentDriftError` on a mismatch
instead of overwriting (a page with no fingerprint on record yet — predating this feature — is trusted,
not retroactively blocked). `force=True` (`wspy-publish publish-path --force`, or the web UI's new
"Overwrite" checkbox) bypasses the check. Every successful write re-fetches the page's own post-write
live content (not the caller's raw input) to record the new fingerprint, so WordPress's own block-markup
normalization-on-save can't cause a false-positive drift on the very next publish. 5 new unit tests
(`web/test_wp_client.py`); not yet verified against the real site (no hand-edited page to test drift
against yet).

### Characterization badges in the curation studio
**Characterization badges in the curation studio** (PR #198) — closes the badges half of Tier 3 item 3
(similarity panels shipped separately, see PR #201 below). A "Generate characterization
badge" panel on the studio page shells `wspy-archetype --run <hostname>:<run_id>` and writes a compact
resource_dominance/confidence/etc. markdown snippet (`archetype_badge.md`) into the run directory —
deliberately not a new curation-studio block "kind" with its own rendering path in every export format;
once written it's an ordinary artifact, already offered by `collect_run_files()` and already rendered
through the existing markdown pipeline via `guess_kind()`'s `.md` rule, the same "external tool output
becomes a curatable file" precedent `wspy-analyze`'s `aianalysis.<model>.md` established. Resolving which
store row to classify hit the same directory-name-vs-store-run_id mismatch `wspy-testpoint`'s own
`aggregate`/`render` had to solve — caught live against the real store before shipping (an early draft
used the run directory's own name and silently resolved the wrong pass every time). `resolve_store_pass_rows()`/
`escape_like()`/`pick_counters_pass_id()` moved out of `wspy-testpoint` into `web/joblib.py` so both
callers share one implementation instead of two independently-drifting copies. Verified live end to end
through the actual running server (generate → file appears in "Add a block" → panel switches to
"Regenerate"); 16 new unit tests across `web/test_archetype_badge.py`/`web/test_joblib.py`.

### cpu2026 benchmark pages get sitemap navigation
**cpu2026 benchmark pages get sitemap navigation** (PR #199) — each of the 52 pages published by
`scripts/publish_cpu2026_benchmarks.py` gets a trailing `[html_sitemap child_of="CURRENT" depth="0"]`
shortcode block, closing the "how do you actually browse this hierarchy" gap `doc/REPORT_HIERARCHY.md`'s
deep suite/test/test-point/machine nesting created — plain WordPress parent/child pages have no built-in
tree browsing, and the site's Weaver theme's own breadcrumbs only solve "where am I," not "what's under
here." New addition to `wp_content()`'s existing hand-built Gutenberg block markup, using the HTML Page
Sitemap plugin newly activated on the live site. `depth="0"` (not `"-1"`) is what actually recurses into
a nested `<ul>` tree — `depth="-1"` flattens every descendant into one un-nested list, confirmed live
when first tried on the parent `cpu2026` index page (hand-edited separately, outside this repo's publish
tooling, to `[html_sitemap child_of="CURRENT" depth="1"]` for its own flat top-level benchmark listing).
Verified live against all 52 `cpu2026` pages (`706.stockfish_r` first, manually inspected, then the
remaining 51) — every page came back `found/updated (status=publish)` with no errors.

### cpu2026 registration tracks SPEC install specdir per host
**cpu2026 registration tracks SPEC install specdir per host** (PR #200) — found while double-checking
PR #199's freshly-committed `workload/cpu2026/<bench>/<tag>/<tune>/source.json` files: that directory is
this repo's own checked-in `web/server.py:CPU2026_DEST_ROOT`, shared across every host that clones it, but
`specdir` (the absolute local SPEC install path `register_cpu2026_point()` records) was a single flat
field — silently belonging to whichever host registered first, leaving every *other* host's "built"
status (`list_materialized_cpu2026_points()`) and the CPU2026 tab's Build/Use-in-Run-tab actions
(`web/server.py`) resolving a specdir that wasn't theirs (wrong path or none at all). `source.json`'s
`schema_version` bumped 1→2: the flat `specdir`/`generated_at` became a `hosts` map keyed by hostname.
`register_cpu2026_point()` stays idempotent/additive, now per host — a hostname already present is left
untouched, a new one is added without touching others (`status` gained `"host_added"` alongside
`"created"`/`"exists"`). A new shared `cpu2026_host_specdir()` helper (defaulting to
`socket.gethostname()`) is the single place that resolves "this host's own entry," used by
`list_materialized_cpu2026_points()` and by the two `web/server.py` handlers that read `source.json`
directly (Build, Use in Run tab) — both now give a clear 400 instead of trying to build/run against an
empty or wrong path. Hand-migrated the 4 already-committed `source.json` files (all written on one host)
to the new schema in the same PR, so the repo was never left in a mixed-schema state. 3 new tests in
`web/test_joblib.py` (270 total there, 405 across `web/`); `./run_tests.sh`'s full C/shell matrix (which
doesn't cover `web/`) also green.

### Similarity panels in reports
**Similarity panels in reports** (PR #201) — closes out 4.3 Tier 3 item 3's other half; badges (PR #198)
were the first half. A "Generate similarity panel" Studio button runs `wspy-archetype --nearest
<host>:<run_id> --csv` once (same store-run-id resolution `resolve_archetype_run_key()` already gave
badges), resolves each neighbor's own run directory via a new `joblib.resolve_store_run_directory()` —
the reverse of `resolve_store_pass_rows()`, reading a neighbor's `output_path`/`manifest_path` back out
of the `runs` table and taking the dirname's last 3 path components as `(suite, benchmark, run_id)` —
and writes a small markdown neighbor table (`archetype_similar.md`) into the run directory. From there
it's an ordinary curatable artifact through the existing markdown pipeline, same "external tool output
becomes a curatable file" precedent badges established, for the identical reason: several render/export
paths (e.g. `render_export_markdown()`) are also called by `wspy-testpoint` with no `wspy-archetype`
config available at all. A neighbor whose own directory can't be resolved (pruned from disk, ingested
from elsewhere) still renders as plain text rather than a dead link. Also brought
`doc/PROFILE_COOKBOOK.md`'s `--nearest`/`--kmeans` section up to date — it previously said neither
existed, though both had already shipped earlier this cycle (PRs #152/#155) — and added
`distance`/`compared_features` to `doc/METRICS.md`. Verified end to end against the real dev store via
the actual running server: a real zero-neighbor render, and a real multi-neighbor case with
correctly-resolved `/report/...` links, both added as a curated block and confirmed rendering on the
report page. 15 new tests (8 in `web/test_archetype_similar.py`, 7 in `web/test_joblib.py`; 420 total
across `web/`); `./run_tests.sh`'s full C/shell matrix also green.

### Interactive timeline viewer for --interval CSVs
**Interactive timeline viewer for --interval CSVs** (PR #202) — closes 4.3 Tier 3 item 4. Scope
correction found during scoping: item 4's "interactive tree/timeline drill-down" wording predates the
discovery that the *tree* half was already fully shipped in 4.2 (`proctree_viewer.js` — collapsible
hierarchy, search, filter, diff, linked from every report). Only the *timeline* half was actually
missing — an `--interval` run's CSV had exactly two renderings before this: a static gnuplot PNG, or raw
preformatted text under the curation studio's generic depth mechanism, neither interactive, neither ever
plotting GPU utilization alongside CPU phase. New `/interval-viewer/<suite>/<benchmark>/<run_id>/
<filename>` page (mirrors `/tree-viewer/...`) backed by `/api/interval-json/...`
(`joblib.parse_interval_csv()`, a stdlib `csv`-module parse, no subprocess), linked from the report page
next to any pass's CSV artifact with a `time` column (`joblib.csv_has_time_column()`). `interval_viewer.js`
is hand-rolled SVG, same no-charting-library precedent `proctree_viewer.js` established: percentage-shaped
columns (topdown axes plus GPU busy/activity — all naturally 0-100%) share one chart with phase-shaded
background bands; every other numeric column gets its own auto-scaled small-multiple chart — deliberately
not a single chart with a second y-axis, per the dataviz skill's own non-negotiable rule against dual
y-axis charts. All charts share one x-domain and phase shading, with a shared hover crosshair/tooltip and
click-drag zoom rescaling every chart together, so GPU/CPU/phase stay correlated regardless of which
chart the pointer is over. New `--series-1..8` CSS custom properties (the dataviz skill's validated
CVD-safe categorical order) for series colors; phase bands reuse this app's own existing
`--good`/`--warn`/`--bad` tokens (already used for `status-ok`/`status-warn`/`status-failed` elsewhere)
rather than importing a second, unrelated status palette. 9 new tests in `web/test_joblib.py`; 429 total
across `web/`, full `run_tests.sh` matrix green. Verified live against the real dev store via the actual
running server (endpoint responses, path-traversal rejection, HTML wiring) — no browser tooling was
available to drive a full interactive click-through (hover/zoom) this session; JS syntax was verified
with a hand-rolled bracket/string/comment-aware balance checker (no `node` in this environment) and
reviewed carefully by hand. **Still worth a manual click-through** before relying on the hover/zoom
interactions in anger.

### wspy-analyze: `on_cpu` explained to the AI-analysis prompt
**wspy-analyze: `on_cpu` explained to the AI-analysis prompt** (PR #203) — fixes a real misreading caught
on a published cpu2026 report: a local model's narrative claimed "30% of the cores are active" for a run
whose `on_cpu` line actually read `0.939  # 30.03 / 32 cores` (93.9% core-busy, i.e. ~30 of 32 cores). Root
cause: `on_cpu` is `print_usage()`'s human-text-only output, never a CSV column, so it fell outside
`GROUP_NOTES`' CSV-header-driven detection (`collect_present_groups()`) and got zero explanation in the
prompt — unlike every other counter group — leaving a small model to pattern-match the `30` inside the
trailing `#`-comment as a percentage instead of using the actual ratio it was told to quote verbatim.
`group_notes_text()` now also checks the raw counter text directly for `on_cpu` and, when present, adds a
note spelling out that the ratio is already the percentage and the comment just restates it in
core-equivalents, not a second finding — applies to both single-run and `--compare-rundir` prompts.
`./test_ai_analyze.sh` (structural + live Ollama calls, both modes) still passes.

### Benchmark reference-matrix database: query layer + web UI
**Benchmark reference-matrix database: query layer + web UI** (PR #204) — ships most of 4.3 Tier 3
item 5, now fully shipped (the analysis-feed hookup, drill-down links, and "by machine" view landed
separately, see PR #210/#211/#212 below). Computed on demand at web-request time, no new persistent
matrix table:
`joblib.enumerate_reference_matrix_cells()` walks materialized Phoronix/cpu2026 test points against
which `<report-root>/<suite>/<test>/<test-point>/<machine>/` directories already have a `runs.json`
(`wspy-testpoint select-runs`) — reusing that curated run selection rather than re-deriving one from
raw store rows, and sidestepping the fact that `store.c`'s own `runs` table has no
`suite`/`test`/`test-point` columns to group by at all (only reconstructable from
`output_path`/`manifest_path` parsing). `joblib.aggregate_reference_matrix_cell()` shells out to
`wspy-testpoint aggregate --csv` per cell (that tool has a hyphenated filename, not importable as a
module); `parse_summary_csv()` moved from `wspy-testpoint` into `joblib.py` so both share one copy
instead of two drifting apart.

Machine identity revised from the settled design during implementation: rather than a new
hand-maintained `<report-root>/machines.json`, discovered `scripts/publish_machine_page.py` already
maintains an equivalent per-machine catalog (`<report-root>/machine/<short-name>/machine.json`, run
once per physical machine) that only lacked a `hostname` field — added that one field instead of
building a second, parallel registry. `web/machine_registry.py` is a read-only scan of that existing
catalog (`{hostname: short_name}`); it never writes. A machine with runs in the store that has never
run `publish_machine_page.py` simply has no slug yet — no synthetic placeholder, self-heals the
first time that machine registers.

Publish status per cell: new `wp_client.list_child_pages()` (`GET /wp/v2/pages?parent=<id>`,
paginated) plus `server.resolve_reference_matrix_row_publish_status()`, which walks one row's
suite/test/test-point pages and lists its machine children in a single call — O(rows) WordPress
calls, not O(cells). Surfaces WordPress's own draft/publish status per machine rather than collapsing
to one posted/not-posted boolean.

Web UI: a new "Reference" tab (`render_reference_tab()`) lists every row with run counts and
publish-status badges per machine column, deliberately no per-metric aggregation on the overview so
it stays fast regardless of matrix size. Clicking a cell opens
`/reference/<suite>/<test>/<test-point>` (`render_reference_test_point_detail()`), which does run
real aggregation per machine and renders a metric × machine cross-machine comparison table
(verdict-driven warning highlighting).

Caught and fixed mid-session, real incidents rather than hypotheticals: (1) a smoke test of
`publish_machine_page.py` with a scratchpad `--report-root` but no `--dry-run` still used real
WordPress credentials and the real report-root git remote — it published a live bogus draft page,
overwrote the live `/machine/` index page's content, and overwrote the local `machine_short_name`
config; all three were restored/cleaned up (one stray draft page needed manual deletion in wp-admin,
since the `wspy` WP account intentionally lacks `delete_pages`). (2) An earlier commit re-annotated
every settled-design sub-bullet in place with "(built)"/"(partly built)" narrative, growing item 5 to
~65 lines in the active backlog — corrected back to the terse form above, matching this doc's own
established convention (see the item-3 correction, `90c2a3a`) of keeping implementation-diary detail
out of the live backlog entirely.

13 `web/test_*.py` files green (2 new: `test_machine_registry.py`, `test_reference_matrix.py`), full
`run_tests.sh` C matrix green (67 bundles, 0 failed).

### Recover metrics from already-published WordPress pages
**Recover metrics from already-published WordPress pages** (PR #205) — closes 4.3 item 21: a machine
that publishes reports but has no direct file/SSH access to whichever host serves the reference-
matrix web UI can still contribute real metric data, recovered straight from its own already-
published WordPress pages, instead of needing that machine reachable at all. Raised the same day as
item 5's PR #204, once real use surfaced a machine (`amd-395-96gb`) that had published reports but
whose raw run data lived on a separate host the reference-matrix-serving machine couldn't reach.

New `web/counter_text.py` parses wspy's human-readable `counters.txt`/`ibs.txt` output (`topdown.c`'s
`PRINT_NORMAL` mode, not the CSV format) into structured `{metric, value, is_percent, comment}`
records — nothing in this codebase parsed this format before (`wspy-analyze`, the only other reader,
hands it to an LLM as unstructured prose). `classify_counter_text()` tells the two formats apart by
content shape (`##### pass N #####` vs. `ibs_sample_` prefixes) since a full-depth curated block
carries no filename tag once published. `wp_client.fetch_page_raw_content()` (promoted from a private
drift-checking helper to a public read primitive) plus new `extract_preformatted_blocks()` pull a
published page's exact `<pre class="wp-block-preformatted">` text back out, HTML-unescaped — confirmed
to round-trip exactly against the encoding `render_export_wordpress()` already uses to publish it.

`server.recover_machine_metrics_from_wordpress()` ties it together: walks a machine's published
run-id pages (most recent `MAX_WORDPRESS_RECOVERED_RUNS` first), parses every full-depth counter
block, and aggregates per metric — first-occurrence-wins within one run when a label repeats (the
same primary-reading-vs-different-pass distinction `topdown.c`'s own multi-pass output can produce).
Wired into `render_reference_test_point_detail()` as an additive column for any machine with
WordPress presence but no local `runs.json` — real local aggregation always wins when both exist, and
recovered columns render visibly distinct (own CSS class, no verdict-based coloring, an explicit
"(from WordPress)" label, and a caption noting the recovered metric-name spellings may not exactly
match `wspy-summary`'s own column names for the same counter). Settled scope: compute on the fly, no
new persistent storage or synthesized `wspy-store` rows — a page later deleted from WordPress simply
stops appearing next load.

Deliberately narrow, with two follow-ups split out rather than folded in (items 22/23, still open):
this only fills a machine column for a test-point *row* already known locally, never discovers a row
with no local trace at all; and `wspy-archetype`/`wspy-analyze` don't yet know this data source
exists. Verified end to end against the real site (read-only): recovered 77 real metrics for
`amd-395-96gb` with no local `runs.json`, confirmed the detail page renders both the store-based and
WordPress-recovered columns together correctly. 18 `web/test_*.py` files green (2 new:
`test_counter_text.py`, `test_wordpress_recovery.py`), full `run_tests.sh` C matrix green.

### Full top-down WordPress discovery for the reference matrix
**Full top-down WordPress discovery for the reference matrix** (PR #206) — closes 4.3 item 22, the
other item 21 follow-up: a "Discover from WordPress" button per suite in the Reference tab finds test
points published on WordPress with no local `runs.json` anywhere, rather than item 21's per-row
recovery, which only ever fills a gap in a row already known locally.
`server.discover_wordpress_matrix_rows()` walks `suite → test → test-point → machine`
(`list_child_pages()` at each level), stopping at the machine-level page — existence only, resolving
the item's own settled scope: never lists run-id children or recovers metrics, both stay deferred to
`render_reference_test_point_detail()`'s already-existing per-row WordPress merge (item 21), triggered
lazily only if a human opens that row's detail page afterwards.

Resolved the item's other open question (cost) by running as a background thread
(`DiscoveryState`/`DISCOVERY_RUNS`, same SSE-relay shape as `execute_analyze()`/
`execute_testpoint_publish()`, keyed by suite name alone since there's no per-run identity here)
rather than on every tab load — confirmed necessary, not theoretical: a real crawl of one suite
(cpu2026, 52 test pages) took 50 seconds against the live site. New
`/api/reference-discover/<suite>` (POST, starts the job) and `/api/reference-discover/<suite>/events`
(GET, SSE stream) routes. Resolved the "how should a discovered row be presented" question more
simply than originally floated: rather than merging discovered rows into the main overview table with
row-level visual distinction, `wireReferenceDiscoverButtons()` (`app.js`) renders them as a plain
list of links straight into each row's detail page once the crawl finishes — simpler to build, and
arguably clearer than blending an unverified discovery into the primary sortable table.

Verified end to end against the real site (read-only): a live cpu2026 crawl found 15 machine-level
pages, including 8 benchmarks with zero local trace at all; the detail page correctly rendered one of
them (`706-stockfish_r`/`amd-395-96gb`) using WordPress's own already-sanitized slug form (dots become
hyphens on page creation; discovery reports the slug WordPress actually assigned, not the original
dotted name, and the detail route works correctly with either). 17 `web/test_*.py` files green (1
new: `test_wordpress_discovery.py`), full `run_tests.sh` C matrix green.

### Parse normalized ratios from counters.txt/ibs.txt comments
**Parse normalized ratios from counters.txt/ibs.txt comments** (PR #207) — closes most of 4.3 item
24, refining item 21's recovery rather than growing it further. `recover_machine_metrics_from_wordpress()`
previously discarded each line's comment, keeping only the raw operand value — but `doc/METRICS.md`
documents that wspy's own store deliberately keeps the *ratio* (IPC, topdown percentages, miss
rates), not the raw operand counts, since raw counts aren't comparable across differently-scaled or
differently-multiplexed runs. The WordPress-recovered path was the only one working from the wrong
number — exactly the values a future clustering/archetype pass (item 23's own eventual target, e.g.
"is this floating-point-heavy" / "what's the branch-miss rate" / "is this front-end-bound") would
need to be genuinely comparable.

New `counter_text.extract_derived_ratios()`, two tiers: (1) a generic
`parse_comment_ratio()` handling most comments, which are self-describing enough for one
`"<number>[%] <description>"` parse (`2.38 IPC`, `8.4% icache miss rate`, `260.598 icache per 1000
inst` → slugify the description into a metric name) — strips `topdown.c`'s own trailing "high"/"low"
classification word first so the same ratio always slugifies to the same name regardless of which
side of a threshold a given run landed on; (2) explicit, `topdown.c`-source-verified tables for two
real traps a generic parse can't handle: `retiring`/`frontend`/`backend`/`speculation` print *two*
percentages (`27.6% (47.0%)`) where only the second, parenthetical one matches the real
`retire_pct`/`frontend_pct`/`backend_pct`/`speculate_pct` CSV column (confirmed directly against
`topdown.c`'s own `PRINT_CSV` branch, not guessed from `PRINT_NORMAL` text alone); their L2 children
and `smt-contention` print one bare percentage with no description text to name a metric from at all
(`smt-contention`'s own second, parenthesized number is a hardcoded literal per `topdown.c`'s own
comment, not real data — takes the first, not second).

Verified end to end against the real site: `amd-395-96gb` now recovers real IPC (2.31), topdown
breakdown (retire 40.6%, frontend 29.6%, backend 23.2%, speculate 6.6%), branch miss rate (2.68%),
and cache/float rates, not just raw operand magnitudes. **Deliberately not a full audit:** the
generic tier's slugified names (e.g. `branch_miss`, `icache_miss_rate`) aren't guaranteed to match
`wspy-summary`'s own real CSV column spelling (`branch_mispredict_pct`, etc.) for every remaining
cache/branch/TLB comment — only the topdown L1/L2 percentages got exact-name verification against
source, since those were the specific trap worth getting right this pass; the rest stays open as
item 24's own residual entry. 16 `web/test_*.py` files green (12 new tests in `test_counter_text.py`),
full `run_tests.sh` C matrix green.

### wspy-archetype characterization for WordPress-recovered machines
**wspy-archetype characterization for WordPress-recovered machines** (PR #208) — closes 4.3 item 23.
Split, on reflection, into a pre-publish/post-publish distinction: `wspy-analyze`'s AI narrative is
generated *before* a run is curated/published, from that run's own real local data — no WordPress
angle there, no gap to close. The real opportunity was post-publish: making `wspy-testpoint render`'s
"Workload characterization" section see peers it currently couldn't at all.

Investigated `--nearest`/`--kmeans` as the entry point and ruled them out as bigger than needed —
their coverage-aware distance already tolerates partial feature overlap, but z-score standardization
is population-relative (computed once across whatever `load_feature_vectors()` loads from SQL), so an
external vector can't be compared after the fact without joining that population first, a real
`archetype.c` change. The section this item actually targets uses `--run` instead
(`trace_run_archetype()`) — a *pure* rule-based classification (`score_snapshot()`) over a small fixed
struct, no database or population statistics involved at all. Shipped the much smaller match: new
`--run-guest <json-file>` mode on `archetype.c` that skips the database lookup entirely, builds that
same struct directly from a flat JSON object (`json_reader.c`, now linked into `wspy-archetype`/
`test_archetype`), and reuses `score_snapshot()` plus the exact same key=value output
`print_scorecard_fields()` (factored out of `trace_run_archetype()`) already emits for `--run` —
`wspy-testpoint`'s existing parser needed zero changes.

`wspy-testpoint`'s `collect_wordpress_archetype_scorecards()` runs this for every machine published
on WordPress for a test point other than the one being rendered, using item 21's
`recover_machine_metrics_from_wordpress()` as the feature source, and `render_archetype_section()`
renders the result as a visibly distinct "WordPress-recovered peers" table — never merged into the
real stats-pool consistency verdict, same visible-provenance principle items 21/22 already
established.

Verified end to end against the real site: `amd-395-96gb` correctly classifies as compute-bound
(40.6%), with `control_flow_style` correctly falling back to "unknown" — a direct, visible
consequence of item 24's then-still-open residual (`branch_miss` vs. the real `branch_mispredict_pct`
name; see the very next entry below, which closed that residual the same day), not a bug in this
item. 5 new C tests (`test_archetype.c`), 12 new Python tests (`web/test_testpoint_archetype.py`,
loading `wspy-testpoint` via `importlib` since it has no `.py` suffix — no prior precedent for
unit-testing that script directly, established here). Full `run_tests.sh` C matrix green (clean
rebuild, given this touches core C code), all 17 `web/test_*.py` files green.

### Align WordPress-recovered metric names with real store.c feature names
**Align WordPress-recovered metric names with real store.c feature names** (PR #209) — closes item
24's residual: a full audit mapping every `counters.txt`/`ibs.txt` comment-derived (and a few
primary-value) metric name to its real `SIMPLE_METRIC_FEATURES` name, traced directly against
`store.c` and the exact `topdown.c` print function emitting each comment, not guessed from sample
output. Real gaps found and fixed: `branch_mispredict_pct` (the `"branch misses"` comment slugified
to `"branch_miss"`, silently unrecognized by `archetype.c`'s `run_snapshot_apply_feature()` — this is
what caused item 23's own `control_flow_style=unknown` write-up above), `smt_contention_pct` (was
targeting the raw CSV column name `"contention_pct"` instead), `dcache_miss_pct`/`icache_miss_pct`/
`l2_miss_pct` (both the ARM/Intel `"l2 miss"` and AMD `"l2 miss from l1"` labels)/`itlb_miss_per1k`/
`dtlb_miss_per1k`, `ipc_mean`, and (via a separate `canonical_metric_name()` rename step, since these
are primary values, not comment-derived) `ibs_dc_miss_pct`/`ibs_dram_pct`/`ibs_dc_l1tlb_miss_pct`/
`ibs_dc_l2tlb_miss_pct`/`ibs_remote_node_pct`. Deliberately left unrenamed, not gaps but genuine
non-issues: GHz (`doc/METRICS.md`: no real CSV column exists at all, human-only annotation),
float/AVX breakdown (not yet promoted to any `SIMPLE_METRIC_FEATURES` entry), and the L1-level
iTLB/dTLB/icache variants that aren't in `SIMPLE_METRIC_FEATURES` today — nothing to align any of
these three to unless `store.c` itself promotes a matching feature later, a `store.c` change, not
something this text-parsing layer can fix on its own.

Verified end to end against the real site: `amd-395-96gb`'s `control_flow_style` now correctly
resolves to `"branch-heavy"` (previously `"unknown"`), confidence improved from `"low"` to
`"medium"` with the extra known axis. All 17 `web/test_*.py` files green, full `run_tests.sh` C
matrix green.

### Surface archetype characterization on reference-matrix pages
**Surface archetype characterization on reference-matrix pages** (PR #210) — closes 4.3 item 5's
"analysis-feed hookup" sub-bullet. Each machine column header on a reference-matrix test-point
detail page now shows a `resource_dominance`/`confidence` badge when available (e.g. "compute-bound
(medium confidence)"). New read-only `wspy-testpoint characterize` subcommand wraps
`collect_archetype_scorecards()`/`collect_wordpress_archetype_scorecards()` (item 23) as JSON so
`web/server.py` can reuse them without a circular import; `characterize_reference_matrix_machines()`
shells out to it once per cell, summarizing a machine's own stats-pool runs to "mixed"/"n/a" when
they disagree and merging WordPress-recovered peer scorecards across cells. `--run`/`--run-guest`
based, not `--kmeans` (deferred — see item 5's own entry for why). Verified live: `amd-395-96gb`
shows "compute-bound (medium confidence)"; `amd-370-64gb` correctly shows no badge (no topdown data
for that run). All 17 `web/test_*.py` files green, full `run_tests.sh` C matrix green.

### Drill-down links from a reference-matrix cell to its individual runs
**Drill-down links from a reference-matrix cell to its individual runs** (PR #211) — closes 4.3 item
5's last open sub-bullet. Each local machine's column header on a reference-matrix test-point detail
page now links to a "(N runs)" page listing every run `wspy-testpoint select-runs` has considered for
that test point + machine — not just the stats-pool subset the aggregate actually averages, so a
human can also see which runs were excluded/supplementary and why. A per-column link, not per-cell,
since every metric in a machine's column is aggregated from that same machine's exact stats-pool run
set. New `joblib.load_reference_matrix_cell_runs()` reads a `runs.json`'s full run list; new
`server.render_reference_test_point_runs()` renders it as a table, linking each run to its own
`/report/` page only when that run's output directory still exists locally. Verified live:
`amd-370-64gb`'s header shows "(1 run)", linking correctly to its one real report page. All 17
`web/test_*.py` files green, full `run_tests.sh` C matrix green.

### "By machine" reference-matrix view
**"By machine" reference-matrix view** (PR #212) — closes the benchmark reference-matrix database's
last open piece, previously deferred as "largely redundant" with the per-test-point detail page —
built after all, sliced the other way: rows are one machine's test points within a suite, columns are
metrics. Reuses `aggregate_reference_matrix_cell()` unchanged (already keyed by suite/benchmark/
machine regardless of which axis a caller iterates over). Scoped per suite, not globally cross-suite,
matching the author's own external reference page this feature is modeled on (one table per suite —
different suites collect different metric sets). New `render_reference_by_machine_panel()` adds a "By
machine" chip list to the reference-tab overview, one chip per (suite, machine) pair, linking to the
new `render_reference_by_machine()` at `/reference/<suite>/by-machine/<machine>`. Deliberately
narrower than the cross-machine detail page: no WordPress-recovery merge or characterization badges,
both still reachable via the per-test-point detail page each row links to. Verified live:
`cpu2026/amd-370-64gb`'s page correctly renders `707.ntest_r/gcc_O3-base` as a row across the full
local metric-name union. All 17 `web/test_*.py` files green, full `run_tests.sh` C matrix green. **This
closes out the benchmark reference-matrix database item entirely** — deleted from the open backlog.

### Site-wide publishing pipeline for the reference matrix
**Site-wide publishing pipeline for the reference matrix** (`scripts/publish_reference_matrix.py`, PR
#213) — closes 4.3's static-site publishing pipeline item, the last piece of what was Tier 3. Fetching
the real `/perf/workloads/<suite>/` pages during scoping showed their actual shape: rows=test-points,
columns=metrics, one machine per page — exactly `render_reference_by_machine()`'s shape (PR #212), so
this was mostly publishing plumbing over data that already existed, not new analysis. Same two-phase
generate-then-push pattern as `publish_cpu2026_benchmarks.py`/`publish_phoronix_pages.py`: per-(suite,
machine) wide table at `<suite>/by-machine-<machine>/`, merging local `wspy-store` data with
`recover_machine_metrics_from_wordpress()` (item 21) for WordPress-only machines discovered via
`discover_wordpress_matrix_rows()` (item 22, default-on — the author noted the store mechanism is
cumbersome and wasn't always followed historically, so pulling from what's already published matters
more than a niche fallback); a per-suite index; and a root rollup. Local data always wins per test
point when both exist; WordPress-recovered cells marked `*`, non-PASS-verdict cells marked `†`. Also
fixed a real, previously-dormant bug found while building this: `joblib.aggregate_reference_matrix_cell()`
passed `--report-root-remote` to `wspy-testpoint aggregate`, which never supported that flag — silently
never hit before because every existing caller passed `None`. Verified via `--dry-run` against the real
local report-root/store and a mocked mixed local+WordPress scenario. All 17 `web/test_*.py` files
green, full `run_tests.sh` C matrix green.

### Web UI "Publish reference matrix" button
**Web UI "Publish reference matrix" button** (PR #214) — wraps the script above as a background-
thread/SSE-streamed card on the Reference tab, same shape as the existing "Publish test-point report"/
"AI narrative analysis" cards, closing the gap where publishing was CLI-only. "Preview (dry-run)"
defaults checked in the form — a web button is a much easier way to fat-finger a live publish than a
deliberately-typed terminal command, so the safe default lives here rather than in the script's own
CLI default (a real run). New `REFERENCE_PUBLISH_RUNS` registry keyed by a fresh per-click job id
(mirrors `DiscoveryState`'s single-key shape — a whole-site publish run has no natural `(suite,
benchmark, run_id)` identity — but a job id rather than suite alone, since one invocation can span
every suite). Verified live: started the dev server, confirmed the button/checkboxes render, POSTed a
real dry-run job against the real local store/report-root, and streamed its SSE events end to end —
correct command line, correct log output, `done` event fired, confirmed nothing was touched on disk.

