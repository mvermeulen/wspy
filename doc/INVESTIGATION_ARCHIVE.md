# wspy Investigation Archive

This file holds full design write-ups and validation narratives for work that has **shipped and is
done** — moved out of `INVESTIGATION.md` so that document stays focused on what's still open. Nothing
here is active backlog; if an archived item needs revisiting (a follow-up, a v2, a newly-discovered
gap), open a fresh entry in `INVESTIGATION.md` itself rather than editing history here.

`INVESTIGATION.md`'s "Shipped since 4.1" section links back to specific entries in this file by name.
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

**V1 scope deliberately excludes `power_core` (per-core energy):** unlike `energy-pkg`, `power_core`'s
own `cpumask` means getting a real per-core breakdown requires opening N events, one pinned per
primary-thread CPU, and aggregating into `--per-core`'s existing per-core row shape — a separate unit of
work, not a bigger version of the same call. `power_core` is still probed for `--capabilities` discovery
but never opened as a real counter.

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
