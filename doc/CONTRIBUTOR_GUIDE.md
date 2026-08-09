# Contributor guide: adding a collector, metric, or schema bump safely

This document is for someone touching wspy's data-producing code for the first time — a new raw perf
counter, a new counter group, a new system-wide metric, a new manifest/run-index/store field, or any
change that adds, removes, or reorders a CSV column. It exists because several of these edits look like
a one-line change but have a second, easy-to-miss step (a schema version bump, a doc update, a test
extension) that the codebase has silently regressed on before — see `doc/INVESTIGATION_ARCHIVE.md`'s
"Non-obvious implementation traps" for two real examples. `CLAUDE.md`'s "Common edits" section is the
terse, canonical checklist; this document is the walkthrough version — same steps, with the *why* and
the exact gotchas spelled out. Read `CLAUDE.md`'s Architecture table first if you haven't already; it
names which file owns which responsibility and this guide assumes you know that map.

Not a tutorial on onboarding a new *benchmark suite* — that's `doc/NEW_WORKLOAD_COOKBOOK.md`. This is
about changing wspy's own code.

## Before you start

- Create a feature branch: `git checkout master && git pull && git checkout -b feature/<slug>`. See
  `CLAUDE.md`'s "Development workflow" — actual behavior changes don't go straight to `master`.
- Know which of the two output paths your change touches, because each has its own downstream
  consumers and tests:
  - **Live counter/system output** (`wspy`'s own CSV/human-readable columns) — read by
    `wspy-validate`, `wspy-store`'s CSV ingest, `tests/golden_output.sh`, `tests/capability_matrix.sh`.
  - **Metadata** (the JSON manifest, the run-index JSONL record) — read by `wspy-validate`,
    `wspy-store`'s `enrich_from_manifest()`, `wspy-ledger`.
  - A single logical addition (e.g. a new hardware feature) can touch both — decide up front whether
    the new data belongs in the per-tick CSV stream, the once-per-run manifest, or both.
- Every scenario below ends the same way: build, run `./run_tests.sh`, update `doc/METRICS.md` if you
  added a metric. Don't skip the doc update to "do it later" — `tests/doc_version_check.sh` (part of
  `run_tests.sh`) only warns on a whole missing formatter, not a column added to an existing one, so
  nothing else catches it for you.

## Scenario: new raw perf counter

1. Add an entry to `intel_raw_events[]` or `amd_raw_events[]` in `topdown.c`: `event=...,umask=...`
   plus a `COUNTER_*` mask bit from `wspy.h` (add a new bit there if none of the existing ones fit).
   `setup_raw_events()` converts the table entry into the `perf_event_open()` config at run time.
2. If this is an Intel E-core (Gracemont) equivalent of a P-core event, add it to
   `intel_atom_raw_events[]` **instead of, and separately from,** `intel_raw_events[]` — the encodings
   routinely differ even for what looks like the same named event (confirmed live: `l2_request.all` is
   `umask=0xff` on P-core, `umask=0x0` on Gracemont). Verify the real encoding against hardware —
   `/sys/devices/cpu_atom/events/<name>` if the event has a name there, else
   `perf stat -e cpu_atom/<name>/ -vv` to read the resolved `config` — rather than assuming the P-core
   row is right for both.
3. If the counter should be user-toggleable rather than always-on when its group is active, add a
   `--foo`/`--no-foo` flag pair in `wspy.c:parse_options()`. Watch for `getopt_long`'s `val` field —
   the "silently misrouted bad flags" trap in `doc/INVESTIGATION_ARCHIVE.md` came from a `val`
   collision on a new long-only flag.
4. **Verify:** `make && sudo ./wspy --csv --topdown -- true` (or whatever flag exercises the new
   counter) and confirm the value looks sane. Run `./run_tests.sh`.

## Scenario: new counter group

1. Write a constructor in `topdown.c` following the pattern of `software_counter_group()` or
   `cache_counter_group()` — it builds and returns a `struct counter_group*` (or `NULL` if the group
   isn't available on this hardware).
2. Link it into `setup_counter_groups()`, gated on the relevant `counter_mask` bit(s). Two things that
   have shipped as real bugs here:
   - Pass only the bit(s) this group actually owns, not the raw `counter_mask`. `cgroup->mask` is later
     tested against `COUNTER_IPC`/`COUNTER_TOPDOWN`/etc. in `print_metrics()`'s dispatch chain in bit
     order — leaking an unrelated bit that's on by default (e.g. `COUNTER_IPC`) misroutes the whole
     group to the wrong `print_*()` formatter and produces a duplicate/wrong column instead of the one
     you added.
   - Each `if` block **prepends** to `*counter_group_list` (`cgroup->next = *counter_group_list;
     *counter_group_list = cgroup;`), so groups print in the *reverse* order their `if` blocks run in.
     If output column order matters for your change, control it by reordering the `if` blocks, not by
     reordering fields inside one group.
3. Add the header/value cases to whichever `print_*()` formatter your group dispatches to (see the CSV
   pitfalls below — this is the step they apply to).
4. **Verify:** `./run_tests.sh`, and manually eyeball `sudo ./wspy --csv --<your-flag> -- true`'s header
   against its value row.

## Scenario: new system-wide metric

1. Parse the raw source in `system.c:read_system()`, store the parsed value in `struct system_state`.
2. Add header/value/normal-mode cases in `system.c:print_system()`.
3. If the metric should be independently toggleable rather than tied to an existing `SYSTEM_*` bit, add
   a new `SYSTEM_*` bit and a CLI flag for it (`system_mask` is otherwise always populated regardless of
   `-s`/`--system`, which only controls whether `print_system()` runs at all — see `CLAUDE.md`'s
   "Counter mask bits" note).
4. **Verify:** `./run_tests.sh`; if the new metric is CSV-visible, extend `tests/golden_output.sh`
   (see below).

## The two CSV pitfalls (applies to any of the above)

Every `print_*()` function switches on `enum output_format` (`PRINT_NORMAL`, `PRINT_CSV`,
`PRINT_CSV_HEADER`) — add the header case and the value case **together, in the same order**;
`tests/golden_output.sh` pins exact CSV column ordering, so an out-of-order addition fails there, not
silently. Two specific mistakes have shipped as real bugs before:

1. **Every field must be comma-terminated in the value row.** A bare `fprintf(fp,"%f",val)` with no
   trailing comma silently fuses into the next field instead of producing an extra column — this reads
   as "the column is just missing" rather than an obvious parse error, and downstream CSV consumers
   (`wspy-store`'s header-driven ingest, `wspy-plot`) may or may not notice depending on the value's
   shape.
2. **The value row must be emitted unconditionally.** Gate it on vendor/group applicability only — never
   on whether the counter actually produced a usable value. Gating the print behind a
   "did this counter read successfully" check means a permission-denied or unsupported-hardware run
   silently drops columns the header still claims exist, desynchronizing the row from its own header.

**Verify:** extend `tests/golden_output.sh`'s exact header/column-order check and/or
`tests/capability_matrix.sh`'s graceful-degradation sweep when your change adds, reorders, or removes a
CSV column — don't rely on manual eyeballing alone; these are exactly the scripts meant to catch a
regression here later. Then update `doc/METRICS.md` (see below).

## Scenario: new manifest field

1. Add the field to `struct manifest_info` (`manifest.h`).
2. Populate it in `wspy.c:main()`.
3. Emit it in `manifest.c:write_manifest()`.
4. **Bump `MANIFEST_SCHEMA_VERSION`** (`manifest.h`): MINOR for an additive field (existing readers keep
   working, they just don't know about the new key), MAJOR for anything removed or renamed.
5. `wspy-validate` checks the manifest's major version against its own `MANIFEST_SCHEMA_VERSION`
   (`validate.c`) — a MAJOR bump means old manifests written before your change may start failing
   validation; that's expected, but make sure it's *intentional* before you bump MAJOR instead of MINOR.
6. **Verify:** `./run_tests.sh` (covers `test_wspy.c`'s manifest-content assertions,
   `tests/doc_version_check.sh`'s cross-check that `doc/ARTIFACT_CONTRACT.md`'s quoted example version,
   if it quotes one for this constant, matches the real `#define`).
7. If the field is genuinely new information (not just a restructuring), also consider whether it
   belongs in `doc/METRICS.md` as `[manifest-only]`, and whether it's a `wspy-store` candidate (next
   scenario).

## Scenario: new run-index field

1. If the field already exists in `struct manifest_info`, just emit it in
   `run_index.c:append_run_index()` — no new struct needed.
2. If it's index-only (not in the manifest at all), it needs its own plumbing analogous to the manifest
   field steps above, scoped to `run_index.c`/`run_index.h`.
3. **Bump `RUN_INDEX_SCHEMA_VERSION`** (`run_index.h`) — deliberately independent of
   `MANIFEST_SCHEMA_VERSION`; the two documents serve different readers (`wspy-ledger`'s workload
   coverage vs. `wspy-validate`'s per-run checks) and change on different schedules.
4. `wspy-ledger` warns (doesn't fail) on a run-index record whose `schema_version` doesn't match its own
   `RUN_INDEX_SCHEMA_VERSION` — expected and fine for a MINOR bump; just confirm the warning path still
   makes sense for your change.
5. **Verify:** `./run_tests.sh` (`test_ledger.c`, `test_store.c`, `tests/doc_version_check.sh`).

## Scenario: new normalized-store field / schema bump

This is the one with the most moving parts. `wspy-store`'s SQLite schema is versioned via
`PRAGMA user_version`, checked against `STORE_SCHEMA_VERSION` (`store.c`) by `ensure_schema()` on every
open.

1. Add the column to `SCHEMA_DDL` (the fresh-database path — what a brand-new store gets today).
2. Bind/populate it in whichever of `upsert_*()`, `enrich_from_manifest()`, or `ingest_csv_metrics()`
   actually produces the value.
3. **Bump `STORE_SCHEMA_VERSION`.**
4. Add a `MIGRATION_V<N-1>_TO_V<N>` DDL string (see the existing `MIGRATION_V1_TO_V2` /
   `MIGRATION_V2_TO_V3` / `MIGRATION_V3_TO_V4` in `store.c` for the pattern) — typically one or more
   `ALTER TABLE ... ADD COLUMN` statements, or a whole new `CREATE TABLE IF NOT EXISTS` for a new child
   table.
5. Add the dispatch branch in `ensure_schema()`'s `user_version == <N-1>` chain. Existing databases
   opened at an *older* version than `N-1` cascade through every intermediate `MIGRATION_Vx_TO_Vy`
   string in sequence up to `N` (see the `user_version == 1` branch in `store.c`, which runs
   `MIGRATION_V1_TO_V2` then `_V2_TO_V3` then `_V3_TO_V4` back to back) — a version-0 (brand-new)
   database instead runs fresh `SCHEMA_DDL` once and never touches the migration strings at all. Either
   way, a given `ALTER TABLE ADD COLUMN` statement runs at most once against any one database, which is
   what keeps it from erroring on a column that's already there — don't add a migration step to a
   branch that could re-run against a database that already has the column.
6. `PRAGMA user_version` is written to the target version only after every applicable migration
   succeeds — if you're testing this by hand, a half-migrated database (a crash mid-migration) is not a
   state the code otherwise has to explicitly handle, so don't rely on partial-migration recovery.
7. **Verify:** `./run_tests.sh` — `test_store.c` exercises the migration end to end (open an old-version
   fixture database, confirm both the schema and the final `user_version` land where expected); add a
   fixture/assertion there for your new migration step the same way. Also check `summary.c`'s
   `METRIC_VALUES_MIN_SCHEMA_VERSION` and `archetype.c`'s `ARCHETYPE_MIN_SCHEMA_VERSION` — these are
   separate "minimum version I can read" floors, not tied 1:1 to `STORE_SCHEMA_VERSION`; bump them only
   if your change actually invalidates older data for that specific reader, not automatically.
8. If the new field is a metric worth surfacing as a first-class per-run summary value (rather than
   staying in long-format `metric_values`), see `store.c:extract_run_features()`'s
   `SIMPLE_METRIC_FEATURES[]` pattern and `doc/METRICS.md`'s `[feature]` status tag.

## Scenario: new `wspy-summary --group-by` column

Add a case to `group_by_column()`, a case to `parse_group_by()`, and a new `enum group_by` value
(`e.<column>` naming convention for a `run_environment` column). This mechanism is deliberately a small,
closed whitelist — the whitelist itself is what makes interpolating the resolved column name into raw
SQL safe. An open-ended, user-supplied grouping key belongs in `--group-by-option`'s
`run_config_options` join instead; don't extend the whitelist just to avoid using that join.

## Scenario: new `wspy-validate` sanity bound

Add an entry to `sanity_bounds[]` (column name + `{min,max}`) only when the generic finite/non-negative/
not-implausible default rule isn't tight enough for this specific column. Use `PERCENT_SANITY_MAX` for
`%`-suffixed columns rather than a hardcoded 100.

## Scenario: new AMD IBS filter

Don't hardcode a register bit offset. Look the field up via `ibs_pmu_format(pmu,"<field>")` in
`ibs_build_fetch_event()`/`ibs_build_op_event()`. Set the corresponding `*_requested` flag
unconditionally (the user asked for it), but only set `*_applied` if the sysfs format lookup actually
succeeds — this is what lets unsupported hardware degrade to "ran unfiltered" instead of failing the
whole run. If the filter changes what counts as a sample, reflect that in `print_ibs()` too.

## Updating `doc/METRICS.md`

Any of the scenarios above that adds a genuinely new metric (a CSV column, a `run_features` entry, an
IBS-sample field, a `wspy-core-report` column) needs an entry in `doc/METRICS.md`: name (use the CSV/
text name verbatim, including its existing casing/spacing conventions — don't "fix" `branch miss`'s
space or `retire_pct`'s lack of one to match), a one-line derivation, the source function, and the
correct database-status tag (`[raw]`/`[feature]`/`[environment]`/`[manifest-only]`/`[human-only]`/
`[categorical]` — see that file's own legend for the exact meaning of each). Only add a high/low
guidance note where it's genuinely meaningful, per that file's own intro — not for every row.
`tests/doc_version_check.sh` WARNs if a whole new metric-*formatter function* has no mention at all in
`doc/METRICS.md`; it cannot catch a column quietly added to a formatter that already has some mention
there, so this step is on you, not the test suite.

## Final checklist before opening the PR

1. `./run_tests.sh` passes (and `./test_amd_smi.sh`/`./test_nvidia_nvml.sh` if the change touches GPU
   code).
2. `tests/golden_output.sh`/`tests/capability_matrix.sh` extended if you added/reordered/removed a CSV
   column.
3. `doc/METRICS.md` updated if you added a metric.
4. Any `*_SCHEMA_VERSION` bump is deliberate (MINOR vs. MAJOR reasoned about, not defaulted) and
   `doc/ARTIFACT_CONTRACT.md`'s example, if it quotes one for that constant, matches.
5. `make compile_commands.json` regenerated if you touched the `Makefile` itself (editor tooling only,
   not part of the build).
6. Push the branch, `gh pr create`, merge through GitHub — see `CLAUDE.md`'s "Development workflow".
