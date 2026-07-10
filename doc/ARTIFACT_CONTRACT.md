# wspy artifact contract & troubleshooting runbook

This document is the reference for what `wspy` writes to disk (manifest, run index, CSV, tree
file), what stability guarantees each format carries, and how to diagnose the most common ways a
run comes back incomplete or wrong. It exists so external tooling (report generators,
`workload/*/run_test.sh` migrations, anything reading `--manifest`/`--run-index` output) can depend
on the documented shape below instead of on today's field ordering by convention.

If you're extending one of these formats, see `CLAUDE.md`'s "Common edits" section first — it has
the mechanical steps (which schema version to bump, where to wire a new field in) for each artifact
type below.

## Artifacts at a glance

| Artifact | Written by | Format | Schema version macro |
| --- | --- | --- | --- |
| Manifest | `--manifest <file>` | one JSON object | `MANIFEST_SCHEMA_VERSION` (`manifest.h`) |
| Run index | `--run-index <file>` | JSON Lines, one compact object appended per run | `RUN_INDEX_SCHEMA_VERSION` (`run_index.h`) |
| Main output | `-o <file>` (or stdout) | CSV (`--csv`) or human-readable text | not versioned — see "CSV output" below |
| Tree file | `--tree <file>` | line-oriented, 4 line kinds | not versioned — grammar is small and append-only |

A single `wspy` run can produce any subset of these: `--manifest` and `--run-index` are independent
(a run can use either, neither, or both), and the main output always exists (stdout if `-o` isn't
given). Only `--tree` produces the tree file.

All three JSON/JSONL-adjacent formats (manifest, run index) are hand-emitted (`json_util.c`'s
`json_write_string()`/`format_iso8601()`), not built with a JSON library — the field order shown
below is what the writer emits, but readers should parse as JSON (object member order is not
semantically meaningful) rather than depending on textual position.

## Versioning contract

Both JSON artifacts carry a SemVer `schema_version` string describing the *document shape*, decoupled
from `wspy --version` (which is just `WSPY_VERSION_MAJOR.MINOR`, the tool's own version):

- **MAJOR** bumps on a field removed or renamed in a way that breaks an existing reader. A reader
  should refuse to trust an unrecognized MAJOR (warn, don't silently misparse) — this is exactly
  what `wspy-validate`'s `check_schema_version()` does (`validate.c`) and what `wspy-ledger` does
  for run-index records it ingests.
- **MINOR** bumps when a field is added in a backward-compatible way. A reader written against an
  older MINOR should still parse a newer-MINOR document correctly (it just won't know about the new
  field).
- **PATCH** bumps for fixes that don't change the shape at all (e.g. correcting how a value is
  computed, not what fields exist).

The manifest and run index are versioned **independently** (`MANIFEST_SCHEMA_VERSION` vs.
`RUN_INDEX_SCHEMA_VERSION`) — the run index is a leaner, line-oriented projection of a run, not the
manifest itself, and its shape evolves on its own schedule. Current versions as of this writing:
manifest `1.2.0`, run index `1.2.0` (check `manifest.h`/`run_index.h` for the authoritative current
values — this doc is not the source of truth for the version number itself, only for the contract
around how it's used).

**What "stable" means in practice:** a field that exists in a given schema version will keep its
name, type, and meaning for the rest of that MAJOR version. New fields get added at the end of their
containing object as MINOR bumps. Don't assume a field is present — `null` is a legitimate value for
almost every optional/best-effort field (see "Degrade, don't fail" below), and a reader should treat
a missing key the same as `null` rather than erroring.

## Manifest (`--manifest <file>`)

One JSON object, written once at the end of a run (`manifest.c:write_manifest()`). Top-level shape:

```
{
  "schema_version": "1.2.0",
  "wspy_version": "4.0",
  "generated_at": "<ISO-8601 timestamp>",
  "command": { "argv": ["<workload argv[0]>", "..."] },
  "timing": { "start_time": "...", "finish_time": "...", "elapsed_seconds": 12.345 },
  "exit_status": { "known": true, "exited": true, "exit_code": 0, "signaled": false, "term_signal": null },
  "host": { "hostname": "...", "kernel_release": "...", "cpu_vendor": "...",
            "cpu_family": 25, "cpu_model": 97, "num_cores": 32,
            "num_cores_available": 32, "is_hybrid": false },
  "environment": { "virt_role": "host", "hypervisor_vendor": null, "microcode_version": "...",
                    "bios_vendor": "...", "bios_version": "...", "bios_date": "...",
                    "cpu_governor": "...", "cpu_scaling_driver": "...",
                    "cpu_governor_uniform": true, "memory_total_kb": 65894680,
                    "compiler_version": "...", "libc_version": "..." },
  "environment_coverage": { "captured": 9, "probed": 9, "unavailable": [] },
  "options": { "counter_mask": "0x3", "per_core": false, "system": false,
               "csv": true, "tree": false, "interval_seconds": 0 },
  "counter_coverage": { "requested": 4, "measured": 4, "unavailable": [] },
  "output_files": [
    { "kind": "output", "path": "results/run.csv" },
    { "kind": "tree", "path": "results/run.tree" },
    { "kind": "manifest", "path": "results/run.manifest.json" }
  ]
}
```

Field notes:

- `command.argv` is the **workload's** command line (what came after `--`), not `wspy`'s own
  argv. It's the only place a manifest records what was actually run — `wspy`'s own flags are in
  `options`, which is a summary (`counter_mask` as a hex bitmask, not a decoded flag list), not a
  full reconstruction of the CLI invocation.
- `exit_status.known` is `false` (with `exited`/`exit_code`/`signaled`/`term_signal` all `null`)
  whenever the workload's exit status wasn't observed this run. This always happens in `--tree`
  mode, since `ptrace_loop()` reaps children itself rather than going through the `wait4()` path
  `wspy.c:main()` otherwise uses to populate these fields — **don't treat `known: false` as an
  error**, it's an expected consequence of `--tree`. `wspy-validate` reflects this as a `WARN`, not
  a `FAIL` (see "Validation & coverage semantics" below).
- `host` is always fully populated (it comes from `cpu_info_init()`, which already has to succeed
  for `wspy` to run at all) — unlike `environment`, none of its fields are ever `null`.
- `environment.*` fields are each independently best-effort (see "Degrade, don't fail" below); a
  `null` value there means that specific field wasn't available on this host, not that provenance
  capture failed outright. `environment_coverage.unavailable` lists which of the
  `PROVENANCE_TRACKED_FIELD_COUNT` (currently 9) tracked fields came back `null` and why
  (`{ "field": ..., "reason": ... }`); `hypervisor_vendor` and `cpu_scaling_driver` exist as fields
  but aren't in the tracked/coverage count (see `provenance.h`'s comment on
  `PROVENANCE_TRACKED_FIELD_COUNT`).
- `counter_coverage.unavailable` lists individual `perf_event_open` failures as
  `{ "group": ..., "counter": ..., "errno": N, "reason": "<strerror(N)>" }`. A nonzero list here
  does **not** by itself mean the run is bad — it means some counters degraded gracefully (see
  "Degrade, don't fail"). Common causes are covered in "Troubleshooting" below.
- `output_files` only lists files that were actually requested this run (an entry is present only
  if the corresponding path was given: `-o`, `--tree <file>`, or the manifest's own path). A run
  with no `-o` (stdout output) and no `--tree` produces an `output_files` array with just the
  `manifest` entry — this is normal, not a gap.
- Numbers that are logically integers (`counter_mask`, coverage counts, exit codes) are emitted as
  JSON numbers except `counter_mask`, which is a hex **string** (`"0x3"`) rather than a number —
  match on the string, don't `parseInt`/`atoi` and compare against a decimal mask you computed by
  hand.

## Run index (`--run-index <file>`)

JSON Lines: one compact, self-contained JSON object per line, newline-terminated, no enclosing
array (`run_index.c:append_run_index()`). Safe for multiple concurrent `wspy` processes sharing one
index file — appends are serialized with `flock(LOCK_EX)`, so records from different processes never
interleave mid-line. Read it by parsing one JSON value per line, not as a single JSON document.

Per-record shape (same information as the manifest, projected leaner — no `output_files` path
details beyond the three path strings, no per-field environment gap list, just counts):

```
{"schema_version":"1.2.0","run_id":"20260710T153000.123-48213","wspy_version":"4.0",
 "hostname":"...","cpu_vendor":"...","cpu_family":25,"cpu_model":97,
 "environment":{...same field set as manifest's "environment"...},
 "environment_coverage":{"captured":9,"probed":9},
 "start_time":"...","finish_time":"...","elapsed_seconds":12.345,
 "command":["<argv0>","..."],
 "exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false,"term_signal":null},
 "options":{"counter_mask":"0x3","per_core":false,"system":false,"csv":true,"tree":false,"interval_seconds":0},
 "counter_coverage":{"requested":4,"measured":4},
 "output_files":{"output_path":"results/run.csv","tree_output_path":null,"manifest_path":"results/run.manifest.json"}}
```

- `run_id` is `<start-time-to-millisecond>-<pid>` (e.g. `20260710T153000.123-48213`) —
  sortable and unique-enough-per-host (two runs starting in the same millisecond on the same host
  still get distinct ids via pid), but **not** guaranteed globally unique across hosts. Combine with
  `hostname` if indexing runs from multiple machines into one place.
- `command` is matched as a **substring** search target by `wspy-ledger` when it maps a workload
  name to run-index records — if you're writing a similar consumer, replicate that (substring, not
  exact-match) rather than assuming `command[0]` alone identifies the workload.
- Consumers that read a whole run-index file (like `wspy-ledger`) should skip/warn on individual
  lines with an unrecognized `schema_version` MAJOR rather than aborting the whole scan — one stale
  record from an old `wspy` build shouldn't block reading the rest of the file. `ledger.c` already
  does this; follow the same pattern in new tooling.

## CSV output (`-o <file> --csv` or `--csv` to stdout)

Not schema-versioned (it's driven directly by `counter_mask`/`aflag`/`sflag`/etc., not a separate
document format), so the contract here is behavioral rather than a version number:

- **Header and value row must have identical column counts**, and value rows are emitted
  unconditionally (not gated behind "did this group actually produce a value" — a permission-denied
  counter still emits its column, just with whatever `read_counters()` left in it, typically `0`).
  This is `tests/golden_output.sh`'s core check; see "New CSV column" below before adding one.
- Column **order** is meaningful and pinned by the golden-output tests (e.g.
  `elapsed,utime,stime,gpu_busy,ipc,...`) — a new column must go through the same "header case and
  value case added together, in the same position" discipline `CLAUDE.md` documents under "CSV vs.
  human output", or the golden tests will fail.
- `--per-core` is a **known, documented gap**: combined with any counter group, the CSV header shows
  only the base/coverage columns while each per-core data row still appends that group's values —
  i.e. header and row column counts intentionally mismatch in this one case today. Don't build a
  strict "header count == row count" validator against `--per-core` output without special-casing
  it; `tests/capability_matrix.sh`'s `per-core-topdown` bundle comment has the concrete example, and
  `wspy-validate`'s CSV-row-count check (`validate.c`) will legitimately flag this combination as a
  `FAIL` today — that's expected, not a bug in `wspy-validate`.
- Percent-valued cells are plain text with a trailing `%` (e.g. `"26.61%"`), not a normalized 0–1
  fraction, and system-wide load/CPU percentages are allowed to exceed 100% when aggregated across
  cores — don't assume `<= 100` when parsing.

### New CSV column checklist

1. Add the header text (`PRINT_CSV_HEADER` case) and the value (`PRINT_CSV` case) in the same
   `print_*()` function, at the same position, unconditionally.
2. Every value field must be comma-terminated, including the last one before a `\n` — a missing
   trailing comma silently fuses two columns together instead of erroring at compile or run time.
3. Extend `tests/golden_output.sh`'s column tables (and `tests/capability_matrix.sh`'s flag-bundle
   list if the new column is behind a new flag) so the golden tests catch a regression here
   automatically, per `CLAUDE.md`'s "Before opening the PR" guidance.
4. If the column deserves a tighter-than-generic sanity bound in `wspy-validate`, add it to
   `sanity_bounds[]` in `validate.c` (see `CLAUDE.md`'s "Common edits").

## Tree file (`--tree <file>`)

Line-oriented text, one of four line kinds per line (documented at the top of `proctree.c`, which is
the reference reader):

```
<time> root <pid>
<time> start <pid> <ppid>
<time> exit <pid> <stat-fields...>
<time> comm <pid> <name>
```

- `<time>` is a monotonic-ish timestamp relative to the run (seconds, floating point).
- `root` appears exactly once, naming the top-level traced pid (the workload itself).
- `start`/`exit`/`comm` can repeat any number of times as the traced process tree forks, execs, and
  exits — a workload that spawns many short-lived children produces one `start`+`exit` pair per
  child, plus a `comm` line whenever a process's command name is (re)captured (e.g. after `execve`).
- `exit`'s trailing fields come from that pid's `/proc/<pid>/stat` at the time of the exit event —
  `proctree.c`'s `struct process_info` (`utime`, `stime`, `starttime`, `vsize`, ...) documents which
  fields it parses out of that line; treat the exact field list there, not this doc, as
  authoritative if you're writing a second reader.
- The file is append-only and unbuffered-ish in practice (written incrementally by `ptrace_loop()`
  as events happen), so a tree file from a run that was killed mid-flight is a valid **prefix** of
  what a completed run would have produced — a partial file (ending mid-tree, without matching
  `exit` lines for every `start`) is a legitimate signal that the run didn't finish cleanly, not a
  malformed file.
- `--tree-cmdline` adds full command-line capture and `--tree-vmsize` adds virtual memory size to
  the fields collected — both are additive to the base grammar above, not a different line format.
- No schema version exists for this format today (unlike the JSON artifacts) because the grammar is
  small, additive-only so far, and consumed by exactly one first-party reader (`proctree`). If a
  breaking change to the line grammar is ever needed, that would be the point to add one.

## Validation & coverage semantics

Three related-but-distinct "how much of what we asked for did we get" signals exist; don't conflate
them:

- **`wspy-validate`** (`validate.c`) checks one manifest (plus the output files it references) for
  publish-readiness: schema version recognized, referenced files exist, output CSV well-formed
  (header/row column counts match, per-cell sanity bounds), exit status clean, counter coverage
  full, elapsed time positive. Reports `[PASS]`/`[WARN]`/`[FAIL]` per check; exits 0 only if nothing
  failed (`--strict` also fails on any `WARN`). This is a **per-run** gate — run it before folding a
  run's output into a published result set.
- **`counter_coverage`** (in the manifest/run index, populated by `coverage.c`) is "how many
  requested hardware counters actually opened this run" — a **run-level, counter-scoped** signal.
  Partial coverage (`measured < requested`) is `wspy-validate`'s design intent working as expected
  (graceful degradation instead of a hard failure), so it's a `WARN`, never a `FAIL`, in
  `wspy-validate`'s own checks.
- **`environment_coverage`** (also in the manifest/run index, populated by `provenance.c`) is the
  same "measured vs. unavailable" pattern applied to provenance fields instead of counters — a
  `null` environment field is expected on some hosts (e.g. no `/sys/class/dmi/id` in a
  minimal-firmware VM), not a defect. `wspy-validate` doesn't currently check this field at all
  (see `check_*` functions in `validate.c`), so a run with poor environment coverage still passes
  validation — only counter coverage and exit status feed into its verdict today.
- **`wspy-ledger`**'s status (`done`/`skipped`/`needs-tool-support`/`unsupported`) is a **suite-level,
  workload-scoped** signal built by scanning many run-index records: did *any* run of this named
  workload exist, and did at least one of them exit cleanly? It answers "is this workload covered at
  all in this run index," which is a different question from either of the above two.

None of these three is a superset of the others — a run can pass `wspy-validate` with a `WARN` on
partial counter coverage, and still count as `done` in `wspy-ledger` (a clean exit is what
`wspy-ledger` checks, not counter coverage).

## Degrade, don't fail

A recurring design pattern across all of these artifacts, worth naming explicitly since it explains
a lot of "why is this field null / this list nonempty" questions: **a single missing capability
degrades that one field/counter to unavailable, it doesn't fail the run.** This applies to:

- Individual `perf_event_open` failures (`coverage.c`) — logged into `counter_coverage.unavailable`,
  run continues with the counters that did open.
- Individual provenance fields (`provenance.c`) — logged into `environment_coverage.unavailable`
  with a reason, rest of provenance still captured.
- GPU flags on a non-`AMDGPU=1` build — a warning ("GPU support not built (rebuild with AMDGPU=1):
  ... ignored"), not an error; the run proceeds without GPU data.
- Missing AMD L3 events (no `/sys/devices/amd_l3/type`) — `--cache3` is silently skipped rather than
  aborting the run.

If you're writing a consumer of these artifacts, mirror this: treat individual gaps as data (surface
them), not as reasons to discard the whole run's output.

## Troubleshooting runbook

Symptom-first; each entry names the underlying cause and where to look/what to run.

### "counter_coverage measured < requested" / `wspy-validate` warns about partial coverage

- **Cause:** one or more `perf_event_open()` calls failed. Check
  `counter_coverage.unavailable[].reason` in the manifest (or the gap list `--capabilities`/normal
  output prints) — it's `strerror(errno)` from the actual failed open, so the message tells you
  which syscall error occurred.
- **`EACCES`/`EPERM`:** `perf_event_paranoid` is too restrictive for the current user, or you're not
  root. Run `scripts/setup_perf.sh` to check/adjust it (needs `perf_event_paranoid <= 1` for
  unprivileged use with `CAP_SYS_PTRACE`, or run `wspy` as root).
- **One fewer counter slot than expected, no explicit error:** check
  `/proc/sys/kernel/nmi_watchdog` — if active, it reserves one hardware counter system-wide.
  `check_nmi_watchdog()` (`topdown.c`) detects and warns about this at startup
  ("`/proc/sys/kernel/nmi_watchdog is running, missing performance counters`"); `setup_perf.sh` can
  disable it too.
- **AMD-only, `--cache3` silently absent from output:** expected if
  `/sys/devices/amd_l3/type` doesn't exist on this CPU (not all AMD parts expose an L3 PMU this
  way) — this is a documented skip, not a bug.
- **This is a WARN, not necessarily a problem:** partial coverage doesn't make a run unpublishable
  by itself — see "Validation & coverage semantics" above. Only treat it as blocking if the specific
  counters you actually need for the analysis are the ones missing.

### `exit_status.known` is `false` in the manifest / run index

- **Cause:** almost always `--tree` — `ptrace_loop()` reaps the child itself, so `wspy.c:main()`'s
  normal `wait4()`-based exit-status capture never runs. This is expected, not a bug.
- **If you need the workload's real exit code with `--tree`:** there isn't currently a way to get
  it from the manifest in that mode; instrument the workload command itself (e.g. wrap it in a
  shell snippet that writes its own exit code to a file) if you need this combination.
- **Not `--tree`, still `known: false`:** check whether `--exit-with-child` was expected to matter
  here — it doesn't affect whether the status is *captured* (that happens either way outside
  `--tree`), only whether `wspy`'s own process exit code reflects it. If capture itself failed
  outside `--tree` mode, that's unexpected — worth filing as a bug rather than working around.

### `--exit-with-child` gives exit code 0 when the workload actually failed

- **Cause:** check for a logged warning `"--exit-with-child: child exit status not observed,
  exiting 0"` — this fires whenever `child_exit_known` is false (see above; typically `--tree`
  mode). `--exit-with-child` and `--tree` don't combine usefully today.
- **Signal death:** a signaled child maps to exit code `128 + signal_number` (conventional shell
  encoding), not the raw signal number or a nonzero-but-otherwise-arbitrary code.

### GPU flags produce no data / a "GPU support not built" warning

- **Cause:** the binary wasn't built with `AMDGPU=1`. Flags are still accepted by the option parser
  (so scripts don't break switching between builds) but each prints
  `"GPU support not built (rebuild with AMDGPU=1): --gpu-<flag> ignored"` and contributes no data.
  Rebuild with `make AMDGPU=1` (see `CLAUDE.md`'s "Build & Test").
- **Built with `AMDGPU=1`, still no data / wrong GPU:** `amd_sysfs_initialize()` scans
  `/sys/class/drm/card*/device/vendor` for the lowest-numbered AMD (`0x1002`) card. On a multi-GPU
  machine, this could be the wrong GPU if you have more than one AMD card — per-device selection
  (`--gpu-device=<idx>`) isn't built yet (see `INVESTIGATION_4.0.md`'s AMD GPU track). If no AMD
  card is found at all, `--gpu-busy`/`--gpu-metrics` warn and read zero rather than erroring.
- **`--gpu-smi` specifically fails to build/link:** confirm `ROCM_DIR/include/amd_smi/amdsmi.h` and
  `-lamd_smi` actually exist under the ROCm install the Makefile auto-detected (`/opt/rocm` preferred
  over `/usr`) — pass `ROCM_DIR=<path>` explicitly if auto-detection picked the wrong one.

### CSV output looks corrupted (fields fused together, wrong column count)

- **First check:** is this `--per-core` combined with a counter group? That's the one documented,
  known mismatch (see "CSV output" above) — not a new bug.
- **Otherwise:** this is exactly the class of bug `tests/golden_output.sh` exists to catch (missing
  trailing comma on a value field, or a value row gated behind a condition the header isn't gated
  behind). Run `./run_tests.sh` (or `tests/golden_output.sh` standalone) against the flags that
  produced the bad output — if it doesn't already catch it, that's a gap in the golden tests worth
  closing (extend the column tables there), not something to hand-patch around in a downstream
  consumer.

### `wspy-validate` fails a manifest that "looks fine"

- **`schema_version` MAJOR mismatch:** the manifest was written by a `wspy` build with a
  MAJOR-incompatible manifest schema. Rebuild `wspy-validate` from the same tree/version that
  produced the manifest, or check whether the MAJOR bump genuinely changed a field you depend on
  (see "Versioning contract" above).
- **"required file missing":** an entry in `output_files` no longer exists on disk — most often
  because the manifest (or the run's output directory) was moved/archived without keeping the
  referenced paths in sync (`output_files[].path` stores whatever path was given on the command
  line, absolute or relative — relative paths break if you move the manifest without moving the CWD
  context it was written from).
- **`--per-core` CSV row/column mismatch:** expected today, see "CSV output" above — not a
  real defect, and there's no per-run flag to suppress this specific check yet.
- **Sanity-bound failure on a real, correct extreme value:** the generic bound (`0` to `1e12`,
  finite) or the specific `ipc` bound (`0`–`32`) may be genuinely too tight for an unusual workload
  or measurement. Extend `sanity_bounds[]` in `validate.c` rather than treating the manifest as bad,
  if the value is legitimately outside today's bounds (see `CLAUDE.md`'s "Common edits").

### `wspy-ledger` shows a workload as `skipped` even though I ran it

- **Cause:** `wspy-ledger` matches a workload name as a **substring** against each run-index
  record's `command` array. If the workload name in your list doesn't appear verbatim as a substring
  of the actual command line used (different casing, a path prefix, an abbreviated vs. full
  benchmark name), it won't match. Check the exact `command` array in the run-index record you
  expect to match against, and check whether that run actually went into the index file you passed
  (`--run-index` has to have been given on the `wspy`/`wspy-run` invocation itself).
- **Shows `needs-tool-support` instead of `done`:** at least one matching record exists, but none
  exited cleanly — check `exit_status` on the matching record(s), same as the `wspy-validate`
  guidance above for a nonzero/signaled exit.

### Concurrent `wspy` runs sharing a `--run-index` file produce garbled/interleaved lines

- This shouldn't happen — appends are serialized with `flock(LOCK_EX)`
  (`run_index.c:append_run_index()`). If you do see interleaving, check whether the index file lives
  on a filesystem where `flock()` semantics are weak/unsupported (some network filesystems don't
  honor advisory locks correctly) — that would be a real gap worth reporting, not expected behavior.

## Related reading

- `CLAUDE.md` — "Architecture", "Common edits", and "Notable runtime behavior" sections cover the
  same ground from the code-structure side rather than the artifact-contract side.
- `tests/golden_output.sh` / `tests/capability_matrix.sh` — the executable form of the CSV/coverage
  contracts described here; run them (via `./run_tests.sh`) before relying on a format detail not
  written down in this doc.
- `INVESTIGATION_4.0.md` — "Run artifact foundation" and "Testing and documentation" tracks have the
  design rationale and history behind why these formats look the way they do.
