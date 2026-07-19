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
| Run-directory manifest | `wspy-run --suite/--benchmark` | one JSON object, one per run directory | `layout_version` (`wspy-run`'s own generator, not a C header) |
| Normalized store | `wspy-store --db <path>` | SQLite database (derived, not written by `wspy` itself) | `PRAGMA user_version` (`store.c`'s `STORE_SCHEMA_VERSION`) |

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
manifest `1.5.0`, run index `1.5.0` (check `manifest.h`/`run_index.h` for the authoritative current
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
  "schema_version": "1.5.0",
  "collector": "wspy",
  "wspy_version": "4.1",
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
  "configuration_provenance": { "preset": null, "configuration": null, "options": [] },
  "options": { "counter_mask": "0x3", "per_core": false, "system": false,
               "csv": true, "tree": false, "interval_seconds": 0 },
  "counter_coverage": { "requested": 4, "measured": 4, "unavailable": [] },
  "passes": [],
  "output_files": [
    { "kind": "output", "path": "results/run.csv" },
    { "kind": "tree", "path": "results/run.tree" },
    { "kind": "manifest", "path": "results/run.manifest.json" }
  ]
}
```

Field notes:

- `collector` identifies which tool produced this record; always `"wspy"` today — it's a schema seam
  for a future non-`wspy` collector (e.g. wrapping `perf stat` or a GPU-specific tool behind the same
  manifest/run-index shape), not a currently-configurable field. A reader can safely ignore it until
  a non-`"wspy"` value actually appears.
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
- `passes` is populated only for a native multi-pass counter execution run (`wspy --passes=<list>`,
  `multipass.h`/`multipass.c`, `INVESTIGATION.md`'s "What shipped in 4.1", "Native multi-pass
  counter execution") — `[]` for a normal run, otherwise one entry per automatically-sized pass:
  ```
  "passes": [
    { "counter_mask": "0x1", "counters_requested": 3, "counters_measured": 3,
      "start_time": "...", "finish_time": "...",
      "exit_status": { "known": true, "exited": true, "exit_code": 0, "signaled": false, "term_signal": null } },
    { "counter_mask": "0x400", "counters_requested": 6, "counters_measured": 6, ... }
  ]
  ```
  `--passes` re-launches the workload once per pass (a genuinely separate re-execution each time),
  so there's no single canonical elapsed time/rusage/exit status across all of them the way a normal
  run has — the top-level `timing`/`exit_status` fields instead reflect pass 0 ("primary") only,
  exactly as if it were the only pass, while every pass's own timing and exit status stay here for
  full audit. A pass whose exit status differs from pass 0's isn't itself a failure (`wspy.c` logs a
  warning, not an error) — real non-determinism across re-executions of "the same" command is a
  legitimate signal worth surfacing, not a reason to discard an otherwise-useful run. The top-level
  `options.counter_mask` for a `--passes` run is the **union** of every pass's mask (still truthful
  to its normal "what this run collected" meaning); top-level `counter_coverage.requested`/`measured`
  are still the running totals across all passes, since `coverage_reset()` runs once before the pass
  loop begins, not per pass — this array's own `counters_requested`/`counters_measured` are each
  pass's individual delta, for per-pass audit, not a second way to recompute those top-level totals.
- `configuration_provenance` (INVESTIGATION.md's "What shipped in 4.1", "structured configuration provenance")
  records which named preset (if any) and/or launcher-vocabulary configuration category and options
  produced this run -- `wspy` itself has no notion of presets/configurations (that vocabulary
  belongs to a front end: `wspy-run`'s builtin profiles, the web launcher's preset picker/checklist),
  so these three fields are populated purely from `--preset-name`/`--config-name`/`--config-option`
  metadata flags rather than derived from `counter_mask`/`aflag`/etc. `preset`/`configuration` are
  `null` and `options` is `[]` for a plain direct `wspy` invocation with none of those flags given --
  this is the common case, not a gap. `options` is an array of `{ "name": ..., "value": ... }` pairs
  in launcher vocabulary (e.g. `"groups"`/`"topdown,cache2"`, `"interval_secs"`/`"1"`), a different
  and lower-level thing than the flat `options` block below (`counter_mask` as a hex bitmask) --
  read both together to answer "how was this run launched" (`configuration_provenance`) vs. "what did
  it actually collect" (`options`/`counter_coverage`). See `wspy-run`'s `run_pass()` and
  `web/joblib.py`'s `build_pass_argv()`/`build_configuration_passes()` for the two front ends that
  populate it today.
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
{"schema_version":"1.5.0","run_id":"20260710T153000.123-48213","collector":"wspy","wspy_version":"4.1",
 "hostname":"...","cpu_vendor":"...","cpu_family":25,"cpu_model":97,
 "environment":{...same field set as manifest's "environment"...},
 "environment_coverage":{"captured":9,"probed":9},
 "start_time":"...","finish_time":"...","elapsed_seconds":12.345,
 "command":["<argv0>","..."],
 "exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false,"term_signal":null},
 "options":{"counter_mask":"0x3","per_core":false,"system":false,"csv":true,"tree":false,"interval_seconds":0},
 "configuration_provenance":{"preset":null,"configuration":null,"options":[]},
 "counter_coverage":{"requested":4,"measured":4},
 "passes":[],
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
- `passes` mirrors the manifest's own `passes` array (see above) but leaner — counts only
  (`counter_mask`, `counters_requested`, `counters_measured`), no per-pass timing/exit status — the
  same "counts here, detail in the manifest" split already used for `counter_coverage`/
  `environment_coverage`. `[]` for a normal (non-`--passes`) run.
- `configuration_provenance` is the same structured configuration provenance as the manifest's field
  of the same name (see "Manifest" above) -- identical shape, just compact JSON instead of
  pretty-printed.

## Normalized store (`wspy-store --db <path>`)

`wspy-store` is not something `wspy` itself writes — it's a separate ingestion tool that reads one
or more `--run-index` files, and best-effort the manifest and CSV output each record points at,
into a SQLite database. This is `INVESTIGATION.md`'s "What shipped in 4.1", "canonical metrics
schema + normalized store": a queryable index of *which runs exist and what they are* (identity,
timing, exit status, coverage counts, provenance — the `runs` table and its child tables), **plus**
a queryable long/tall table of the actual per-run *metric values* (IPC, topdown, cache numbers —
`metric_values`) that a run's CSV output contains. The metric-value layer only exists for runs that
were collected with `--csv` and whose output file is still readable from wherever `wspy-store` runs
— see "Manifest enrichment and metric ingestion are best-effort and path-based" below for the same
caveat that already applies to manifest enrichment.

**Why SQLite, not Parquet:** the backlog item names both. SQLite is what `wspy-store` writes
because it's a single well-supported C library (fits this codebase's "avoid heavy dependencies"
posture — ROCm is the only other external dependency, and only behind `AMDGPU=1`) and the write
pattern (`wspy-store` upserting one run at a time) is OLTP-shaped, not the batch-columnar-scan shape
Parquet is built for. Parquet has no practical C writer without adopting Apache Arrow/Parquet-C++,
so it isn't something `wspy-store` produces directly — if you need a Parquet copy for pandas/Arrow
tooling, export it downstream (e.g. `duckdb store.db -c "COPY runs TO 'runs.parquet'"`; DuckDB reads
SQLite natively).

**Schema** (`store.c`'s `SCHEMA_DDL`):

- `runs` — one row per run, surrogate `id` PK, natural key `UNIQUE(hostname, run_id)`. Most columns
  are a direct projection of the run-index record's fields (same names, `snake_case`); `counter_mask`
  is kept both as the original hex string (`"0x3"`) and pre-parsed into `counter_mask_int` (plain
  integer) so SQL bit-test queries (`counter_mask_int & 0x4`) work without a hex-parsing UDF.
  `kernel_release`/`num_cores`/`num_cores_available`/`is_hybrid` are populated only via manifest
  enrichment (see below) — they're `NULL` until `manifest_ingested = 1`.
- `run_command_args` — one row per `argv` element, `(run_id, arg_index)` PK, so the full command
  line is queryable/joinable rather than only the denormalized `runs.command` (argv[0] only, for
  display).
- `run_environment` — one row per run, all 12 provenance fields plus `environment_coverage`'s
  `captured`/`probed` counts. Populated straight from the run-index record's own `environment`/
  `environment_coverage` objects — **not** manifest enrichment, since the run index already carries
  this in full.
- `metric_values` — one row per (CSV data row × non-dimension column), the long/tall metric-value
  fact table: `run_id` (FK), `row_index` (ordinal of the CSV data row), `tick_time`/`core`/`phase`
  (dimensions — see below), `metric_name` (the CSV header cell verbatim, e.g. `"ipc"`, `"retire"`,
  `"net eth0"`), `value` (parsed number, `NULL` if non-finite or unparseable), `is_percent` (cell had
  a trailing `%`), `raw_text` (the original cell text, always kept — auditable even when `value` is
  `NULL`). Indexed on `(run_id, metric_name)` for "give me this metric's series for this run" queries.
  `runs.metrics_ingested`/`runs.metrics_row_count` record whether/how much this landed for a given run.
- `store_meta` — small descriptive key/value table (e.g. `created_at`); **not** the schema
  compatibility gate (see below).
- `ingest_sources` — one row per ingested file path, tracking the last byte offset/size
  successfully ingested, so re-running `wspy-store` against a growing (or unchanged) run-index file
  only does work proportional to what's new, not O(n²) over the file's lifetime. If a file's size
  drops below its last-recorded size (rotated/truncated), the next ingest rescans it from byte 0.
  (This offset tracking applies to `--run-index` files only — each run's CSV output is small and
  bounded, so it's simply fully re-parsed, and `metric_values` rows for that run replaced, on every
  ingest of that run's record; see "Idempotency" below.)

**CSV column classification, and why one code path covers every wspy CSV shape:** `ingest_csv_metrics()`
never infers column identity from `--interval`/`--per-core`/etc. flags — it reads the CSV file's
actual header row. A column named exactly `time` becomes `tick_time` (present only for `--interval`
runs), `core` becomes `core` (present only for `--per-core` runs), `phase` becomes `phase` (present
only when phase detection was active); every other non-empty-named header column becomes a metric.
This is what lets the aggregate, `--interval`, `--per-core`, and `--interval`+`--per-core`
(well-formed rows) shapes all ingest through the same logic with no per-mode special-casing.

A data row whose column count doesn't match the header's is skipped **individually** (counted in
the `wspy-store` summary line, not fatal), and parsing continues with the next row — deliberately
more robust than stopping at the first mismatch, against two confirmed pre-existing `wspy` CSV bugs
this ingester has to tolerate rather than get fooled by:
- `--interval --gpu-smi`: every periodic tick row is short by the 4 `gpu_smi_*` columns
  (`timer_callback()` never emits them — only the tail row, built by a separate code path, does).
  Skipping bad rows individually still lets that correctly-shaped tail row through; stopping at the
  first mismatch would have lost it too.
- `--per-core --interval`: after the well-formed interval rows, `wspy.c` appends a separate,
  unheaded block (one raw line per core, no header, a completely different column count) — each of
  those lines individually mismatches the interval header and is skipped, exactly as if it were
  unrelated garbage, never misattributed to a metric name it doesn't belong to.

`nan`/`-nan`/`inf` cells — which `topdown.c`'s ratio/percent `print_*` functions (`print_ipc()`,
`print_topdown_be()`, etc.) can legitimately emit on a zero divisor, confirmed empirically — parse
fine via `strtod()` but are stored as `value = NULL` (so they're invisible to `AVG()`/`MIN()`/
`MAX()` rather than poisoning them) while `raw_text` still preserves the literal `"-nan"` for audit.

**Natural key and collision handling:** `run_id` is documented (`run_index.c`'s own comment, and
above) as unique only *per host* — the natural key is `(hostname, run_id)`. If two records ever
present the same `(hostname, run_id)` with a materially different `start_time`/`command` (a real
risk for containers sharing a host's UTS namespace with easily-duplicated low-numbered PIDs), that's
a **collision**, not a re-ingest of the same run: `wspy-store` detects this by comparing against the
existing row before applying an upsert, logs a warning, leaves the existing row untouched, and
counts it (`--strict` turns this into a nonzero exit). It does not attempt to merge or pick a
"winner."

**Idempotency:** ingesting the same run-index file (or a file that has only grown since the last
ingest) any number of times leaves the `runs` table with exactly one row per distinct
`(hostname, run_id)` — records are upserted (`ON CONFLICT(hostname,run_id) DO UPDATE`), not
appended. `metric_values` rows for a run are deleted and reinserted on every (re-)ingest of that
run's record, so re-ingesting never duplicates metric rows either.

**Manifest enrichment and metric ingestion are best-effort and path-based:** each record's
`output_files.manifest_path`/`output_files.output_path` (if non-null) is only read if the file
exists on disk at ingest time; metric ingestion additionally requires `options.csv` to have been
true for that run (trusted from the run-index record itself, not sniffed from file content) and the
file to be non-empty. Before trusting the manifest, `wspy-store` cross-checks its
`command.argv[0]`/`timing.start_time` against the run-index record's own `command[0]`/`start_time`
— a fixed or reused output filename could otherwise point at a *different* run's manifest by the
time it's ingested. A mismatch (or a missing/unreadable file) leaves `manifest_ingested = 0` and the
host-detail columns `NULL`; a missing/unreadable/empty/non-CSV output file leaves
`metrics_ingested = 0` and no `metric_values` rows. Neither is ever fatal. In the deployment this
tool is named for — aggregating run-index files copied in from many hosts into one central `--db`
— both `manifest_path` and `output_path` are frequently relative paths from the originating host and
won't resolve on the ingesting machine, so expect `manifest_ingested = 0`/`metrics_ingested = 0` for
most rows in that setup. That's expected degradation (matching this codebase's "measured vs
unavailable" idiom used throughout `coverage.c`/`provenance.c`), not a bug to chase.

**Concurrency:** `wspy-store` opens the database with `PRAGMA journal_mode=WAL`, a 30-second
`sqlite3_busy_timeout`, and `PRAGMA foreign_keys=ON`, so multiple concurrent `wspy-store`
invocations against the same `--db` (e.g. from a cron job or CI matrix) serialize rather than fail
outright. **WAL mode is unsafe over NFS** (per SQLite's own documentation) — if `--db` will ever
point at a network filesystem for multi-host aggregation, keep the database on local disk and sync
it out-of-band instead, don't point `--db` directly at the network mount.

**Schema versioning:** gated by `PRAGMA user_version`, not a table — checked immediately after
`sqlite3_open()`, before any DDL/DML. A `wspy-store` build refuses to write to a database whose
`user_version` is newer than the schema version it understands (exit 2), rather than silently
misparsing it. A database at an older recognized version is migrated in place: `ensure_schema()`
runs either the fresh-database `SCHEMA_DDL` (`user_version == 0`) or exactly one version-specific
`ALTER TABLE`/`CREATE TABLE IF NOT EXISTS` migration step per older version found (e.g.
`MIGRATION_V1_TO_V2` adds `metric_values` and the two `runs.metrics_*` columns to a database created
before this table existed), never both — see `CLAUDE.md`'s "New normalized-store field" entry for
the pattern to follow when adding the next one. This is `wspy-store`'s first real migration;
`test_store.c` covers its idempotency/migration behavior end to end (see `INVESTIGATION.md`'s
"Shipped since 4.1", "Testing").

## Summary tables (`wspy-summary --db <path>`)

`wspy-summary` is a read-only query tool over the normalized store above — it never writes to
`--db`, and it produces nothing persisted; running it again is exactly how a summary table is meant
to be "regenerated from data only" (`INVESTIGATION.md`'s "What shipped in 4.1", "summary table
generator", closing the "a summary page can be regenerated from data only" criterion deferred from 4.0).

**What it computes:** for each contributing run, a metric's `metric_values` rows are first averaged
(`AVG(value)`) down to one number for that run — a no-op for the common single-row aggregate CSV
shape, and the right collapse for a `--interval` run's ticks or a `--per-core` run's cores, without
the tool needing to know which shape produced the data. Those per-run numbers are then grouped into
`(group, metric)` buckets — `group` is workload command by default, or `hostname`/`cpu_vendor` via
`--group-by` — and each bucket gets min/max/mean/median/stddev (sample, `n-1` denominator; 0 for
`n<2` rather than undefined) plus a z-score outlier flag per run (`--outlier-stddev`, default 2.0;
only evaluated for buckets with `n>=3` and nonzero stddev, since flagging is meaningless with fewer
samples). `--command`/`--hostname` filter which runs contribute; `--metric` (repeatable) filters
which metrics are reported. A bucket with fewer contributing runs than `--min-runs` is skipped
(counted, not fatal — the usual degrade-don't-fail idiom); `--strict` exits 1 if any bucket was
skipped this way, or if nothing matched at all.

**Schema compatibility:** opens the database read-only and requires `PRAGMA user_version >= 2` (the
schema version `metric_values` was introduced at — see "Normalized store" above); an older database
has nothing to summarize and is refused with a clear message rather than silently reporting zero
rows. A *newer* schema version than this tool was built against is still accepted — `store.c`'s own
MINOR-vs-MAJOR versioning convention never removes or renames a column on a MINOR bump, only adds
one, so the columns `wspy-summary` reads keep working forward.

**Output:** a flat table (human or `--csv`), one row per `(group, metric)` bucket, with the
statistics above plus an `outlier_run_ids` column (semicolon-separated) naming which runs were
flagged, so a surprising number in the table is traceable back to a specific run.

**Traceability (`--show-runs`/`--trace`):** `INVESTIGATION.md`'s "What shipped in 4.1",
"Traceability links (summary row → manifest → raw CSV → plots → tree artifacts)" — closing the
other criterion deferred from 4.0 alongside the summary generator itself (see "Success criteria for
a 4.0 kickoff" above). `--show-runs` appends every contributing run's `hostname:run_id` to a bucket
(a `contributing_runs`/trailing column, all of them — not just `outlier_run_ids`' flagged subset),
giving any row in the table a concrete set of run identities to chase, not just the outliers.
`--trace <hostname>:<run_id>` is a separate standalone mode (needs only `--db`, ignores every
grouping/filter option) that resolves one of those identities straight out of the `runs` table's own
`manifest_path`/`output_path`/`tree_output_path` columns — checking with `stat()`/`opendir()` whether
each still exists on disk, and deriving a `<output_path's directory>/plots` path (`wspy-plot`'s output
location, not a column the store tracks anywhere) — completing the chain from a summary row through
to the manifest (command line + environment), raw CSV, tree artifact, and plots. Every field degrades
independently (`exists=0`, not a failure) rather than aborting the lookup when a path no longer
resolves, matching the same-section note above about cross-host paths frequently not resolving on the
ingesting machine. Output is stable `key=value` lines (not `--csv`'s table shape, and not a bespoke
JSON encoding) so a caller — a script, or `web/server.py`'s `/api/discovery/trace` endpoint, which
further resolves a `/report`/`/files` link whenever the paths happen to fall under its own
`--output-root` — can parse a single resolved run without needing a JSON library on either side.
Exit status is 1 (not 2) when the `(hostname,run_id)` pair isn't recorded in the store at all,
mirroring `--strict`'s "still needs more data" convention rather than treating it as a usage error.

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
- `--per-core` produces **one CSV row per active core** (a leading `core` column identifies which
  one), not the single aggregate row every other flag combination produces — `aflag` routes every
  counter group named in the run's flags onto each core's own counters rather than a single
  systemwide set, so a single row has no natural place to put them. The base/rusage/system/gpu
  columns and the coverage counts repeat identically on every core's row (they're process/system
  scalars, not per-core), same idiom as `--interval`'s repeated per-tick columns. Header and row
  column counts match like any other combination now (`tests/golden_output.sh`'s
  `per-core-topdown`/`per-core-software` `assert_csv_columns_match` cases). One exception:
  `--per-core` combined with `--interval` keeps the *old* single-row-per-tick shape, column-count
  mismatch included — `timer_callback()` (the periodic-tick reader) only ever reads
  `cpu_info->systemwide_counters`, never per-core counters, so it can't produce the new shape either;
  see `wspy.c`'s `per_core_csv` comment and `tests/capability_matrix.sh`'s `interval-per-core` bundle
  comment. `wspy-validate`'s CSV-row-count check (`validate.c`) will legitimately flag *that*
  combination as a `FAIL` — not a bug in `wspy-validate`, and not fixed by this item.
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

## Unified output layout (`wspy-run --suite <name> --benchmark <name>`)

This is a `wspy-run` feature, not a `wspy` one — `wspy` itself has no concept of a suite or
benchmark name. Passing `--suite` and `--benchmark` together (`INVESTIGATION.md`'s "Run
artifact foundation" track, "Unified output layout" item) switches `wspy-run` from its flat
`<outdir>/<prefix><pass-name>.<ext>` naming to one directory per run:

```
<outdir>/<suite>/<benchmark>/<run-id>/
  <pass-name>.<csv|txt>        one file per pass, same content as flat mode
  <pass-name>.manifest.json    per-pass manifest (--manifest-dir defaults to this directory)
  process.tree.txt             written directly by a --tree pass (e.g. the tree-heavy profile)
  summary.txt                  concatenation of every non-CSV, non---tree pass's output
  manifest.json                run-level index (not a per-process wspy manifest -- see below)
  plots/                       reserved, empty -- for a future report generator (4.1/4.2)
```

`<run-id>` is `wspy-run`'s own `<timestamp-ms>-<pid>` (same shape as `run_index.c`'s per-process
`run_id`, computed independently — it identifies the whole `wspy-run` invocation, which may launch
several `wspy` processes, not one of them) unless overridden with `--run-id`. `--prefix` is
rejected together with `--suite`/`--benchmark`, since the directory already namespaces output.

**`manifest.json`** (the run-level one, generated by `wspy-run` itself — distinct from each pass's
own `--manifest` output next to it) is a small, unversioned-by-C-header JSON object:

```json
{
  "layout_version": "1.0.0",
  "suite": "cpu2017",
  "benchmark": "503.bwaves_r",
  "run_id": "20260710T153000.123-48213",
  "generated_at": "2026-07-10T15:30:05.000Z",
  "command": ["runcpu", "--config", "...", "503.bwaves_r"],
  "passes": [
    {"name": "systemtime", "output": "systemtime.csv",
     "manifest": "systemtime.manifest.json", "status": "ok"}
  ]
}
```

- `passes[].status` is `"ok"` or `"wspy-error"` — this is `wspy-run`'s own per-pass launch status
  (did `wspy` itself run and exit 0), the same distinction `wspy-run`'s own stdout already makes
  (`[name] done` vs. `[name] wspy error`). It is **not** the workload command's exit status — that
  lives in the pass's own `--manifest` file's `exit_status` (see "Manifest" above), one file open
  away via `passes[].manifest`.
- `passes[].manifest` is `null` for a pass that ran without `--manifest-dir` (not the default when
  `--suite`/`--benchmark` are given, but possible if a config file or future option disables it).
- `layout_version` describes this document's own shape, following the same MAJOR/MINOR/PATCH
  convention as `MANIFEST_SCHEMA_VERSION`/`RUN_INDEX_SCHEMA_VERSION` (see "Versioning contract"
  above), but it isn't tied to a C header — bump it by hand in `wspy-run` if the shape changes.
- One `wspy-run` invocation writes a fresh `manifest.json`/`summary.txt` from only the passes *that
  invocation* ran — running `wspy-run` a second time into the same run directory (same explicit
  `--run-id`) overwrites both rather than merging in the new invocation's passes. Compose multiple
  builtin profiles into one invocation instead (a comma-separated profile argument, e.g.
  `deep-cpu,tree-heavy`) if a suite needs passes from more than one profile in the same run
  directory — this is how `workload/cpu2017`, `workload/phoronix`, and `workload/pbbsbench` do it.

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
- GPU flags on a build missing the matching `AMDGPU=1`/`NVIDIA=1` — a warning ("GPU support not built
  (rebuild with AMDGPU=1/NVIDIA=1): ... ignored"), not an error; the run proceeds without GPU data.
- `--gpu-nvidia` on an `NVIDIA=1` build with no NVIDIA driver installed — `nvidia_nvml.c` dlopen()s
  `libnvidia-ml.so.1` at run time rather than linking it at build time, so a missing driver is a
  first-class runtime condition here (unlike AMD's build-time link failure): logged, the run proceeds
  with zero-valued `nv_*` columns.
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
- **Built with `AMDGPU=1`, still no data / wrong GPU:** `amd_sysfs_initialize()` defaults to the
  lowest-numbered AMD (`0x1002`) card under `/sys/class/drm/card*/device/vendor` when no device is
  selected. On a multi-GPU machine this could be the wrong GPU if you have more than one AMD card —
  pass `--gpu-device=<idx>` to select a specific card/device by index (see the device list printed by
  `wspy --capabilities` on multi-GPU hosts, and `CLAUDE.md`'s GPU support notes). If no AMD card is
  found at all, `--gpu-busy`/`--gpu-metrics` warn and read zero rather than erroring.
- **`--gpu-smi` specifically fails to build/link:** confirm `ROCM_DIR/include/amd_smi/amdsmi.h` and
  `-lamd_smi` actually exist under the ROCm install the Makefile auto-detected (`/opt/rocm` preferred
  over `/usr`) — pass `ROCM_DIR=<path>` explicitly if auto-detection picked the wrong one.

### `--gpu-nvidia` produces no data / `nv_*` columns are all zero

- **Cause:** the binary wasn't built with `NVIDIA=1`. Same "recognized but warns" convention as the
  AMD GPU flags: `"GPU support not built (rebuild with NVIDIA=1): --gpu-nvidia ignored"`. Rebuild with
  `make NVIDIA=1` (see `CLAUDE.md`'s "Build & Test") — no CUDA toolkit/nvidia-dev package is needed to
  build this, unlike AMD's ROCm dependency.
- **Built with `NVIDIA=1`, still no data:** unlike AMD, this isn't a build-time link — `nvidia_nvml.c`
  `dlopen()`s `libnvidia-ml.so.1` at *run* time, so the NVIDIA driver has to actually be installed on
  the machine running `wspy` (not just the machine it was built on). Check `wspy --capabilities`'s
  NVML section: it explains which stage failed ("library not found" vs. "`nvmlInit_v2()` failed").
- **Wrong GPU on a multi-GPU machine:** `nvidia_nvml_initialize()` defaults to NVML device 0 when no
  device is selected. Pass `--gpu-nvidia-device=<idx>` to select a specific device by index (see the
  device list printed by `wspy --capabilities` on multi-GPU hosts). An out-of-range index degrades to
  no NVIDIA data (logged) rather than erroring, same as `--gpu-device`.
- **`--interval` ticks are missing the `nv_*` columns:** these are populated by `topdown.c`'s
  `timer_callback()`, a separate print call site from the aggregate/tail row — a genuine regression
  here (not a documented gap, unlike `--gpu-smi`'s interval gap noted above) since both are meant to
  stay in sync; `tests/golden_output.sh`'s `gpu-nvidia` interval check exists specifically to catch it.

### CSV output looks corrupted (fields fused together, wrong column count)

- **First check:** is this `--per-core` combined with `--interval`? That's the one remaining
  documented mismatch (see "CSV output" above) — not a new bug. Plain `--per-core` plus a counter
  group (no `--interval`) now produces one matched-column row per core; a mismatch there is a new
  bug, not the known gap.
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
- **`--per-core` + `--interval` CSV row/column mismatch:** expected today, see "CSV output" above —
  not a real defect, and there's no per-run flag to suppress this specific check yet. Plain
  `--per-core` without `--interval` no longer mismatches.
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
- `INVESTIGATION.md` — "Run artifact foundation" and "Testing and documentation" tracks have the
  design rationale and history behind why these formats look the way they do.
