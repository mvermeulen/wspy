wspy 4.3 is the second-largest release yet (89 merged PRs). It closes out the clustering/nearest-
neighbor, topdown/attribution, and publishing/reporting-expansion work planned for this cycle, plus a
substantial amount of tooling that grew organically out of actually publishing real reports against a
real WordPress site: a benchmark reference matrix that works even for machines with no local database
presence, a full `wspy-testpoint` run-selection/aggregation/rendering pipeline, PID-targeted counter
attachment and symbol-level profiling, and a round of real hardware-verified Intel hybrid
counter-grouping fixes. This note groups what shipped by theme and summarizes why it matters; see
`README.md`, `CLAUDE.md`, and `INVESTIGATION.md`'s "What shipped in 4.3" for full command/flag
reference and design detail, and `doc/INVESTIGATION_ARCHIVE.md`'s "Validation narratives (4.3-era)"
for the complete validation write-up behind every item below.

## Intel hybrid counter-grouping correctness (Tier 0)
Real Intel hybrid hardware (a Raptor Lake HX host) became available for the first time this cycle and
turned up six confirmed hardware bugs, all now fixed:
- **`--per-core` silently measuring only the first core** (#129) — a shared perf-group-leader fd bled
  across back-to-back `setup_counters()` calls.
- **`--topdown`/`--topdown2` reporting all-zero** whenever any other Intel counter opened first (#129)
  — Intel's Perf Metrics `slots` sub-events require a literal `slots`-led group.
- **Unbounded Intel counter groups losing wholesale to `EINVAL`** past real hardware PMU capacity
  (#132) — now chunked into budget-respecting groups the same way AMD/ARM already were.
- **An unsigned-underflow bug in the L2 topdown split** (#134) — routed through the same `safe_sub()`
  AMD's side already used.
- **RAPL/`energy-pkg` opening at the wrong scope** (#135) on any host without a lucky PMU-type-number
  coincidence — a new `requires_system_wide` bit replaces the incidental type-value match.
- **Full Gracemont E-core raw-event support** (#136) — `intel_atom_raw_events[]`, every encoding
  verified directly against real `cpu_atom` PMU output; Gracemont has no `slots` register at all, so
  `print_topdown()` now synthesizes one from `cpu-cycles*5`.
- Also: x86 hybrid core-type detection for `--affinity=coretype=<id>` (#131), and a related
  vendor-agnostic fix where `cache_counter_group()`'s synthetic `"instructions"` entry numerically
  collided with L1I-read under `PERF_TYPE_HW_CACHE` (#133).

## AMD IBS sampling mode
- **`--ibs-sample`** (#143, #144) — mmaps the perf ring buffer with `PERF_SAMPLE_RAW` and decodes real
  per-sample fields into end-of-run rate estimates (`dc_miss`/`dc_l1tlb_miss`/`dc_l2tlb_miss`/
  `op_brn_misp`, `dram_rate`/`remote_node_rate`, fetch-side miss rates) — the first thing in this
  codebase to read a perf mmap ring buffer at all.
- A new `ibs-sample` `wspy-run` profile, and `zen4plus-deep` switched onto it (#146, #147) — needs the
  same `CAP_PERFMON` grant `--power` already documented.

## Analytics and comparability (`wspy-summary`, `wspy-archetype`)
- **`--check-regression`** (#148) — compares a run's own metrics against a rolling baseline of every
  earlier same-bucket run, reusing the existing stats/CI machinery rather than inventing new
  statistics.
- **`env_score`/`mixed-env`** (#149) — an 8-field environment-comparability score (BIOS, microcode,
  governor, memory, virtualization), extending 4.2's exact-match `mixed-pmu` check into a real scored
  metric.
- **Per-run IPC quantile features** (#150) — `ipc_p10`/`ipc_p90`/`ipc_iqr` from `--interval` tick data.
- **`--phase-topdown`** (#156) — breaks one run's topdown output down by warmup/steady/degraded phase.

## Clustering and nearest-neighbor (Tier 1, now fully shipped)
- **`wspy-archetype --nearest`** (#152) — coverage-aware, z-standardized distance computed only over
  `run_features` both runs actually share, so a pair with less overlap isn't unfairly penalized.
- **`wspy-archetype --kmeans`** (#155) — k-means++ initialization, available-case-mean centroids
  (the standard partial-distance strategy for clustering under missing data), empty-cluster healing,
  and per-cluster profile cards.

## Composite attribution (Tier 2, now fully shipped)
- **`memory_attribution` archetype axis** (#157) — cross-references topdown's `backend_pct` against
  every independently-measured cache/TLB/IBS signal a run collected, as `corroborated`/
  `uncorroborated`/`unknown`.
- **Core-class-aware topdown** (#158) — a new Intel-hybrid warning for a real correctness gap in
  non-`--per-core` mode, plus `wspy-core-report --weight-by <metric>` for activity-weighted cross-core
  aggregates.
- **Blocking-syscall-split modifier** (#159) — `blocked`/`oversubscribed` labels, checked before the
  cache/TLB/IBS corroboration logic, from `--tree` futex/io-wait/schedstat data now ingested into the
  store for the first time.
- **`memory_attribution_locus`** (#160) — an IBS-derived decomposition of *which* cache level a
  memory-bound stall concentrates in, the AMD-only equivalent of Intel/ARM's `--topdown-backend` chain.

## Phoronix suite tooling (Tier 7)
- **`wspy-ledger --phoronix-option-combos`** (#137) — reports a workload's full option-matrix size
  upfront instead of discovering it partway through a long batch-run sweep.
- **`wspy-phoronix-import` + the web Phoronix tab** (#138–#142, #153, #154) — decomposes an
  OpenBenchmarking result or installed/exported suite into minimal single-test-point suites, with
  grouped inventory, per-test README generation, and version-mismatch/filter UI.
- Smaller fixes: linking queued Phoronix runs back into their test-point directory (#145), a
  `--foreground` timeout fix to prevent orphaned background processes (#151).

## CPU2026 web tab
- **CPU2026 workload-suite tab** (#161) — discovery/registration/build-triggering for SPEC CPU2026
  benchmarks, structurally simpler than the Phoronix tab since a benchmark+config already exist
  locally once installed.
- Per-host `specdir` tracking (#200) so a shared repo checkout works correctly across multiple SPEC
  install hosts; an `shrc`-sourcing-order fix (#179); a missing benchmark added to the static catalog
  (#224).

## Process-tree and profiling depth
- **`--target=comm=<name>[,cmdline=<substr>]`** (#164–#167) — PID-targeted counter attachment: a
  second, dedicated counter group on just the matching process, surfaced in the tree viewer's
  hot-process table and per-node detail, plus a full counter-group selector on the Run tab (which also
  fixed a dead "software counters" checkbox that had never actually worked).
- **`--symbol-sample`/`wspy-symbolize`** (#169) — routine/symbol-level profiling scoped to
  `--target`-matched processes, via a generalized `perf_ring.c` shared with IBS sampling; a
  tree-viewer "▶ profile" drill-down resolves samples against `addr2line`.
- Tree-viewer trims/collapses (#168) for a more readable counter-group picker.

## `wspy-analyze`
- Fixed a "thinking" model (`gpt-oss:20b`) silently producing empty output from context-window
  exhaustion, plus new pre-computed report artifacts and a "default curation" button, later made
  configurable via `web/default_curation.conf` (#178).
- Output is now real Markdown (`.md`, a new stdlib `markdown_lite.py` converter) instead of
  HTML-escaped `<pre>` text, reaching both the report page and WordPress export as native Gutenberg
  blocks.
- AMD IBS counting-mode CSV data (#182) and `on_cpu`'s already-a-percentage meaning (#203) now reach
  the AI prompt, fixing two real narrative misreadings caught on published reports.

## Report-page artifacts and curation studio
- **Characterization badges** (#198) and **similarity panels** (#201) — one-click `wspy-archetype
  --run`/`--nearest` output written as a curatable markdown artifact, the same "external tool output
  becomes a curatable file" precedent used throughout.
- **Interactive timeline viewer for `--interval` CSVs** (#202) — hand-rolled SVG, small-multiple
  charts sharing one phase-shaded x-domain with a synchronized hover crosshair and zoom.

## `wspy-testpoint` pipeline
- **`select-runs` → `aggregate`/`render`** (#183–#186) — role-assignment (stats-pool/supplementary/
  excluded/primary) so a redo never pollutes statistics, then a curated `README.md` with cross-run
  archetype-stability, wired into a web "Publish test-point report" button (#187).
- Fixed two real identity-resolution bugs found publishing actual reports: resolving a run's actual
  per-pass store `run_id`s instead of its directory name (#194), and threading `--cpu2026-dest-root`
  through to identity resolution (#196); the button's Machine field now pre-fills from config (#195).

## WordPress publishing
- **REST publishing primitives** (#173–#177) — `web/wp_client.py`/`wspy-publish`: page
  create/update/draft/publish, media upload, Gutenberg-block content reuse.
- **Hierarchy-aware `publish_page_at_path()`** — walks/auto-creates parent stubs, used for cpu2026
  (#188) and Phoronix (#189) level-3/4 benchmark pages, machine catalog pages (#191), and the
  single-run "Publish to WordPress" button's now-correct page nesting (#190); Phoronix/cpu2026
  test-point stubs auto-populate at publish time (#192, #193).
- **Idempotent content-merge protection** (#197) — a fingerprint check refuses to silently overwrite a
  page a human hand-edited in wp-admin since wspy last wrote it.
- Sitemap navigation on cpu2026 pages (#199) for browsing the deep suite/test/test-point/machine
  hierarchy.

## Benchmark reference matrix
- **Reference matrix database: query layer + web UI** (#204) — computed on demand from materialized
  test points' `runs.json`, no new persistent matrix table; a per-test-point cross-machine detail view
  and a "by machine" view (#212), with drill-down links to individual runs (#211) and archetype
  characterization badges per machine column (#210).
- **Machines with no local data can still contribute:** metrics and topdown ratios recovered directly
  from their own already-published WordPress pages (#205, #207, `web/counter_text.py`), full-suite
  WordPress discovery for rows with no local trace at all (#206), and characterization scoring for
  those WordPress-recovered machines too (#208, #209).
- **Site-wide static-publishing pipeline** (#213, `scripts/publish_reference_matrix.py`) plus a web
  "Publish reference matrix" button (#214), and a round of usability polish — search/sort/column-toggle
  and metric tooltips, grouping fixes, cache-column merges (#215–#223).

## Release engineering
- `scripts/release_prep.sh` gained an outstanding-open-issue review alongside its existing open-PR
  check.
- `doc/CONTRIBUTOR_GUIDE.md` — a walkthrough for adding a collector/metric/manifest-or-run-index
  field/store schema bump without breaking CSV shape, doc drift, or schema-migration checks.

---
89 merged PRs since v4.2. See `INVESTIGATION.md`'s "What shipped in 4.3" for the complete pointer list
and `doc/INVESTIGATION_ARCHIVE.md` for full design/validation detail on every item above.
