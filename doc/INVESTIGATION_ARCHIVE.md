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

## Validation narratives (4.2-era)

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
