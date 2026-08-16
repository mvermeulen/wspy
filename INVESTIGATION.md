# wspy Investigation

A rolling roadmap, kept deliberately focused on **forward-looking backlog** — what's actively planned
and the design reasoning behind it. Formerly `INVESTIGATION_4.0.md` — renamed once its content outgrew
a single release. Past releases are recorded only as terse pointer lists (below); their real interfaces
live in `README.md`/`CLAUDE.md`/`doc/METRICS.md`, and the design write-ups/validation narratives behind
every decision live in `doc/INVESTIGATION_ARCHIVE.md` — this document doesn't restate either.

Status (2026-08-09): **4.0, 4.1, 4.2, 4.3, and 4.3.1 are all tagged and released** (v4.2 and v4.3
published as GitHub releases with `wspy-release-notes.4.2.md`/`wspy-release-notes.4.3.md` as their
bodies, v4.3.1's release body written directly — see `scripts/release_prep.sh`). **4.4 is next.** The
open backlog (below) is sorted into three buckets: **"4.4 priorities"** (three named goals —
ease-of-use/one-click web UI flows, GPU support, and Phoronix suite build-out), **"4.5 priorities"**
(lower priority, still wanted, loosely grouped by topic), and **"Deferred indefinitely"** (explicitly
not planned for any numbered release; revisit only on a concrete trigger). A fresh "`<N>` release
closure" bucket gets added back once a cycle's own priorities empty out and it's time to prep that
tag — see `scripts/release_prep.sh`'s own checklist for what that housekeeping covers.

## Purpose
This document captures ideas for improvements focused on making benchmark collection, organization,
and publication easier and more repeatable.

## How to use this document
- **This document is the open backlog, not a changelog.** "What shipped in 4.0"–"4.3" below exist only
  so a name/PR/flag can be traced back to *which release* it landed in — each is a terse pointer list,
  not a feature description. `README.md`/`CLAUDE.md`/`doc/METRICS.md` document current interfaces and
  mechanism; `doc/INVESTIGATION_ARCHIVE.md` holds every design write-up and validation narrative. Don't
  restate either here — link to it.
- **When an item ships:** delete it from its priority bucket and add one line (name the file/tool/flag,
  not the mechanism) to that release's "What shipped in `<N>`" list — creating that section the first
  time a cycle's first item ships. If its design merited a multi-paragraph write-up while it was being
  built, move that write-up to `doc/INVESTIGATION_ARCHIVE.md` rather than leaving it inline — the open
  backlog should only ever contain open work. A rolling "Shipped since `<prior release>`" section is the
  intra-cycle staging area for this (present only while a cycle is actively shipping); fold it into the
  terse "What shipped in `<N>`" form at release-prep time, same as every past cycle.
- **Cross-references are by name, not number.** Item numbers inside a single tier list are fine as a
  local index, but don't reference an item elsewhere in this file (or from `CLAUDE.md`/commit
  messages) as "4.2 #27" — describe it by name instead ("AMD IBS sampling-mode support"). Numbers shift
  every time a tier is reorganized; names don't.
- The open backlog is sorted into three buckets — see the Status line above for what each holds. Add a
  new item to whichever bucket fits, or move an existing one, rather than inventing a parallel table.
- "Track deep-dives" hold reasoning that doesn't fit a single backlog line (Zen5/IBS, Intel hybrid/
  counter-grouping, topdown, the preset/configuration/option vocabulary). Each points back at the
  priority-list items it informs, and keeps only its still-open thread — historical background that fed
  an item that's since shipped moves to `doc/INVESTIGATION_ARCHIVE.md`, same rule as everywhere else in
  this document.
- "Open questions" carry a recommendation each; re-open one by editing its entry, not by appending new
  prose elsewhere in the file.

## What shipped in 4.0
Terse pointer list, not a feature log — see `CLAUDE.md` for current interface/mechanism of every named
file/tool, `doc/ARTIFACT_CONTRACT.md` for the manifest/run-index/CSV schema contracts, and
`doc/INVESTIGATION_ARCHIVE.md` for design history and validation narratives.

- Run artifact foundation: manifest + run-index schemas, `wspy-run` (profile-driven launcher),
  `wspy-validate`, `wspy-ledger`, the unified per-run output layout.
- Reproducibility/provenance capture: `coverage.c` (counter capability discovery), `provenance.c`
  (virt role, microcode, BIOS, governor, memory, toolchain).
- Topdown confidence envelope + decomposition sanity checks.
- Zen5/IBS capability-driven probing (`ibs.c`), `ibs-basic`/`ibs-memory-deep` profiles.
- Expanded `getrusage`/`/proc` process telemetry.
- Counter-fit preflight (`wspy --preflight`); interval automatic phase-boundary detection (`phase.c`).
- Portability: arch-neutral ptrace register access (`ptrace_arch.h`, x86_64 + aarch64); collector-plugin
  schema seam (`collector` field, no non-wspy implementation yet).
- AMD GPU dynamic device scan; NVIDIA GPU monitoring (`--gpu-nvidia`). GPU *kernel*-level instrumentation
  (CUDA/Vulkan) was scoped out of this project — see `doc/INVESTIGATION_ARCHIVE.md`.
- Golden-output/capability-matrix testing (`tests/golden_output.sh`/`tests/capability_matrix.sh`).

## What shipped in 4.1
- Normalized SQLite store + reporting: `wspy-store`, `wspy-summary`, `wspy-plot`.
- Multi-pass counter execution: `--passes`/`--multiplex` (`multipass.c`).
- Web launcher & report browser (`web/server.py`): Run/Validate/Store & Summary/Discovery tabs,
  curation studio, multi-run compare view, publish-ready export, `/history`.
- Structured configuration provenance: `--preset-name`/`--config-name`/`--config-option`.
- Job queue: `wspy-queue`, `web/joblib.py`.

## What shipped in 4.2
- Critical-path/synchronization-latency instrumentation: `--tree-futex`/`-io`/`-io-wait`/`-connect`/
  `-nanosleep`/`-wait`/`-poll`/`-schedstat`/`-vmsize`.
- Core/thread affinity control: `--affinity=...` (`affinity.c`), `--list-affinity`.
- AMD IBS hardening + Zen-family preset packs (`zen-portable`/`zen4plus-deep`).
- CPU energy/power: `--power` (`power.c`), package + per-core.
- Hierarchical L1→L2→L3 topdown schema.
- GPU support: ROCm SMI + sysfs fusion layer (`--gpu-metrics`), `gpu-compute` profile, NVIDIA parity.
- PMU-capability-aware comparability warnings (`mixed-pmu`).
- System-wide metrics: `SYSTEM_DISK`/`SYSTEM_MEM`, cgroup v2 identity/limits/throttling (`cgroup.c`).
- Per-core diagnostics: `wspy-core-report`, AMD Zen5/Zen5c core detection, `--per-core-freq`.
- `proctree --json`/`--diff` + interactive web tree viewer.
- ARM64: real topology/topdown/ptrace support, validated on real hardware.
- Local-LLM narrative analysis: `wspy-analyze` (Ollama).
- Feature normalization + archetype scorecard: `run_features`, `wspy-archetype`.

## What shipped in 4.3
- Intel hybrid counter-grouping correctness (Tier 0): six hardware-verified bugs fixed on real Intel
  hybrid hardware, plus full Gracemont E-core raw-event support.
- AMD IBS sampling mode: `--ibs-sample` (`ibs_sample.c`).
- `wspy-summary`/`wspy-archetype` analytics: `--check-regression`, `env_score`/`mixed-env`, per-run IPC
  quantile features, `--phase-topdown`.
- Clustering/nearest-neighbor (Tier 1, now fully shipped): `wspy-archetype --nearest`/`--kmeans`.
- Composite attribution (Tier 2, now fully shipped): `memory_attribution`/`memory_attribution_locus`
  archetype axes, core-class-aware topdown, `wspy-core-report --weight-by`.
- Phoronix suite tooling (Tier 7): `wspy-ledger --phoronix-option-combos`, `wspy-phoronix-import`, web
  Phoronix tab.
- CPU2026 web tab: discovery/registration/build-triggering for SPEC CPU2026 benchmarks.
- Process-tree/profiling depth: `--target=comm=<name>[,cmdline=<substr>]` PID-targeted counter
  attachment, `--symbol-sample`/`wspy-symbolize`.
- `wspy-analyze`: Markdown output (`markdown_lite.py`), IBS-CSV/`on_cpu` prompt fixes.
- Report-page curation: characterization badges, similarity panels, interactive `--interval` timeline
  viewer.
- `wspy-testpoint` pipeline: `select-runs`/`aggregate`/`render`, `wspy-archetype --run-guest`.
- WordPress publishing: `web/wp_client.py`, `wspy-publish`, hierarchy-aware page publishing.
- Benchmark reference matrix: Reference web tab, WordPress-recovered metrics for machines with no local
  database presence, site-wide static-publishing pipeline (`scripts/publish_reference_matrix.py`).
- Release engineering: `scripts/release_prep.sh` outstanding-open-issue review, `doc/CONTRIBUTOR_GUIDE.md`.

Release-prep housekeeping is folded into the list above. `scripts/release_prep.sh --version 4.3`'s full
checklist ran clean; `wspy-release-notes.4.3.md` is the published GitHub release body. v4.3 is tagged
and released.

## What shipped in 4.3.1
- Correctness fix: `web/server.py` never imported the `report_root` module, crashing every default
  `python3 web/server.py` startup (no `--report-root` flag) on first page load — `NameError: name
  'report_root' is not defined` (#226). Regression test: `ResolveReportRootForWebTest` in
  `web/test_reference_matrix.py`.

`scripts/release_prep.sh --version 4.3.1`'s full checklist ran clean. v4.3.1 is tagged and released;
its GitHub release body is the notes at `https://github.com/mvermeulen/wspy/releases/tag/v4.3.1`.

## Shipped since 4.3.1
Intra-cycle staging area for 4.4 — fold into "What shipped in 4.4" at release-prep time.

- Quickstart guide / guided onboarding path. New "Quickstart" section in `README.md` (right after
  "Building", before the 16-tools-each-in-their-own-section "Usage" reference material begins): a
  verified, copy-pasteable three-command loop (`make` → `sudo ./wspy-run --suite demo --benchmark
  hello -o web/runs --run-index web/runs/index.jsonl quick -- sleep 2` → `python3 web/server.py`)
  taking a fresh checkout from nothing to a viewable report page, followed by short pointers (not a
  full second walkthrough) to the next real steps: `wspy-store`/`wspy-summary` for queryable/
  summarized-across-many-runs data (with the exact `wspy-store` caveat this needed — `quick`'s pass
  has no `--csv`, so ingesting it alone shows `0 metric-set(s) ingested`, confirmed live), and
  `wspy-publish` for actually posting to WordPress. Same sequence surfaced as a first-run hint in the
  web UI too: `render_index()`'s "Recent reports" table, previously a bare "No runs yet." with no clue
  whether that's expected or a misconfigured `--output-root`, now shows the same command plus a
  README pointer whenever `discover_reports()` finds nothing — gone again the moment a real run
  exists. Verified end-to-end against a real running server (the exact quickstart commands run for
  real, not just read: build already done, a real `wspy-run` invocation, the resulting run showing up
  under "Recent reports" and its `/report` page loading correctly, `wspy-store` producing the exact
  "0 metric-set(s) ingested, 1 skipped" line the README's own caveat describes) plus new
  `web/test_quickstart_hint.py` (3 tests, one of which drives the real `wspy-run --dry-run` to guard
  against the hint's own command drifting from what `wspy-run` actually accepts).
- Report-page guided flow / progress indicator. A lightweight Run/Curate/Characterize/Publish
  summary (`render_progress_indicator()`, `web/server.py`) now sits at the top of all three report
  shapes (`render_wspy_run_report()`, `render_fixed_report()`, `render_incomplete_run_report()`),
  above the ~10 existing cards/buttons, so a reader sees "where in the workflow is this run" at a
  glance instead of discovering that stack only by scrolling. Presentation/sequencing only, per the
  item's own scope note — no action gets merged or reordered, every stage reuses state a caller
  already has or a cheap local check: **Run** from the run status each report shape already
  computes (`ok`/`skipped` → done, `failed` → failed, `incomplete` → incomplete, matching the
  `.status-*` palette item 6's job-state badges already established); **Curate** from
  `curation.json` content (new `_curation_has_content()`, factored out of `render_curated_section()`
  so both ask the identical question — "not started"/"started" (has the file but nothing included
  yet)/"curated"); **Characterize** from whether `archetype_badge.md`/`archetype_similar.md` exist
  in the run directory. **Publish** deliberately never makes a live WordPress call to check
  "already published" — that lookup is real, slower, and network/credential-dependent (this
  codebase already has one, `resolve_reference_matrix_row_publish_status()`, for the reference-matrix
  page a human navigates to deliberately), inappropriate to run unconditionally on every single
  report page view. Instead it shows only whether publishing is set up at all (one local
  `wp_client.load_config()` file read) — "actually published, and where" stays the export/publish
  cards' own job, which already show it live once a human clicks in. Verified against a real running
  server with five synthetic run fixtures (complete/uncurated, complete/curated-with-badge,
  workload-exit-nonzero, legacy fixed-config shape, interrupted/incomplete) covering every stage
  combination including a fake WordPress config proving the Publish check never calls
  `wp_client.find_page()`/`list_child_pages()`; plus new `web/test_progress_indicator.py` (17 tests).
- Preset/Configuration/Option vocabulary refactor — closing slice, item now fully shipped and
  removed from the open 4.4(a) list. Two threads closed it out:
  - **Client-side `data-key` wiring.** The checklist's 15 pure 1:1 checkbox-to-flag options
    (`profiles/checklist-flags.conf`, the earlier checklist-boolean-flag-table slice above) were
    mechanized server-side but still hardcoded a second time in `app.js`'s `buildChecklist()` — one
    `getChecked("tree_cmdline")`-style call per option, a real drift risk (add a checkbox to the
    HTML, forget the matching JS line, and that option silently never reaches the server). Each such
    checkbox (`render_run_tab()`, `web/server.py`) now carries `data-key="<option-key>"` inside its
    card's existing `data-config="<checklist-key>"` scope; a new `readDataKeyCheckboxes()` (`app.js`)
    reads them generically by querying that scope, so a future mechanical option needs a
    `checklist-flags.conf` line plus one HTML checkbox with its `data-key` — no third edit. Fields
    with real logic behind them (groups, interval/timeout seconds, target, power, IBS
    profile/thresholds) stay explicit, matching `build_configuration_passes()`'s own split between
    its data-driven table and its hand-written non-mechanical logic. Verified via a jsdom smoke test
    driving the actual shipped `app.js` against a real `render_run_tab()`-rendered page (prefilled
    checklist state round-tripping correctly through the generic reader into the `/api/preview`
    request body); no browser available this session.
  - **Explicit scope closure for the rest.** The checklist's genuinely non-mechanical logic
    (`counters`/`system`/`ibs` sections' `--passes` bin-packing choice, `--power` folding, legacy
    `"amdtopdown"`/`"systemtime"` pass-name heuristics) and the Run tab's checkbox *labels/grouping*
    (human-authored UI copy, not vocabulary `wspy-run`'s own data-driven profiles have any equivalent
    of) are declared a deliberate stopping point, not deferred work — forcing multi-input conditional
    logic into a flat table would obscure it, not simplify it (same reasoning already applied to
    `--power`'s cross-section folding when the boolean-flag table shipped), and there's no
    `profiles/*.conf`-side analog for UI presentation content to unify against in the first place. The
    "Preset / Configuration / Option hierarchy deep-dive" below is updated to match: its own
    cross-cutting goal is now resolved as far as any open backlog item drives it, and the one thing it
    once floated but never turned into a real item (restructuring `wspy.c`'s own CLI flag parsing
    around this vocabulary) is recorded as dropped, not pending.
- Job-browsing view in the web UI. A queued job (`wspy-queue add`, or the Run tab's "Queue instead
  of running it now" checkbox) was visible before this only via `wspy-queue list`/`show`, not from
  the web UI at all. New `/jobs` page (linked from the homepage, alongside `/history`): every job
  under `<jobs-dir>` across its pending/running/done/failed lifecycle, with a state filter, and a
  done job's row/detail page linking straight to its `/report`. `/jobs/<job-id>` detail page adds
  the "bundle in sharing structured configuration provenance with the job format" half of the item:
  new `format_job_configuration()` (`web/server.py`) reuses `format_config_provenance()` -- the
  exact same one-liner a completed run's per-pass `configuration_provenance` already renders with --
  rather than inventing a second formatting convention just for jobs; a `mode="custom"` job's
  checklist has no single configuration (several sections can be enabled at once, each becoming its
  own wspy pass), so this shows one line per enabled section via the newly-public
  `joblib.config_options()` (promoted from `_config_options()`, now a cross-module caller) and a
  new `_CHECKLIST_KEY_TO_CATEGORY` reverse lookup -- literally the same values a run launched from
  that job would go on to record, not a re-derivation. Backing shared logic
  (`state_dir`/`ensure_jobs_dirs`/`job_path`/`find_job_file`/`load_job`/`save_job`) moved from
  `wspy-queue` into `web/joblib.py` so the web view locates/reads job files the exact same way
  `wspy-queue` itself does, rather than a third, independently-drifting copy; `wspy-queue` now
  imports them instead of defining its own (its `list`/`show`/`requeue` subcommands' own output is
  otherwise untouched -- no CLI behavior change). New `joblib.list_jobs()` is the shared newest-
  first scan both `wspy-queue` (available for a future rewrite of its own `list` subcommand, not
  applied here to avoid an unforced CLI-output change) and the new page use. Deliberately read-only
  browsing -- no requeue/delete button from the web UI yet, `wspy-queue requeue`/`run` remain the
  only way to act on a job, matching `joblib.py`'s own "execution only ever happens via
  `wspy-queue run`, headless" design (a natural fast-follow, not bundled into this slice). Verified
  against a real running server with synthetic jobs across all four states (list, state filter,
  done-job report link, failed-job error display, unreadable-job-file degradation, 404 for an
  unknown job id) plus new `web/test_jobs_view.py` (16 tests) and `ListJobsTest`
  (`web/test_joblib.py`, 5 tests).
- Detect and resume interrupted `wspy-run` profiles, Phase B — resume, skipping completed
  passes (item now fully shipped, both phases; see the Phase A entry below). New `wspy-run --resume
  <existing-run-dir>` flag: derives `--suite`/`--benchmark`/`--run-id`/`--outdir` from the resumed
  directory's own unified-layout path (rejects an explicit `--suite`/`--benchmark`/`--run-id`/
  `--prefix` alongside it, and a directory that already has a top-level `manifest.json` -- nothing
  to resume), then runs the given profile/config exactly as a fresh invocation would, except each
  pass is skipped when it already has a clean-exit manifest recording the *exact* configuration this
  invocation would run now. Exact-match via a new `pass_flags_hash` (`compute_pass_flags_hash()`,
  `wspy-run`) -- a hash of that pass's own flags + `--affinity` + the workload argv (deliberately
  *not* `--config-option`'s own caller-supplied metadata tags, which "never affect what a pass
  actually does" per that flag's own `--help` text) -- now recorded via
  `--config-option pass_flags_hash=<hash>` on *every* pass, not just resumed ones, so a later
  `--resume` always has something on disk to compare against even for a run that completes normally
  the first time. The actual JSON comparison (`exit_status` clean *and* `pass_flags_hash` match)
  lives in new `scripts/pass_resume_check.py` rather than hand-rolled bash JSON parsing -- same
  "shell out to python3 for anything that needs a real parse" posture `estimate_tree_timeout.py`
  already established for this script; `python3` unavailable degrades to "always rerun", never
  blocking a run over a missing capability. A skipped pass gets a new `PASS_STATUS` value,
  `"skipped"` (`doc/ARTIFACT_CONTRACT.md`'s `passes[].status` enum updated), treated identically to
  `"ok"` by `run_status_from_passes()` (`web/server.py`) and the report page's per-pass status
  styling -- a resumed run isn't a failure just because some passes weren't re-executed. Never
  resumes a pass that was itself interrupted mid-execution: no manifest at all (`--manifest` is the
  last thing a wspy invocation writes) falls straight through to a real rerun, covered by the same
  missing-manifest path as any other skip-ineligible pass, no separate check needed. Verified via a
  new `tests/wspy_run_resume_smoke.sh` (14 checks, wired into `run_tests.sh` alongside
  `wspy_queue_smoke.sh`, same fake-`wspy`-binary approach, no build/GPU axis or root/perf access
  needed) covering: full skip on an identical resume, full rerun on a changed workload command
  (hash mismatch), mixed skip+rerun when only one pass has its own manifest, `--dry-run --resume`
  reporting skip decisions without touching the filesystem, and rejection of an already-finished
  directory -- plus `scripts/test_pass_resume_check.py` (9 tests) and
  `RunStatusFromPassesTest` (`web/test_incomplete_run.py`, 5 tests). Deliberately CLI-only for now,
  no web-UI "resume" button -- a reasonable fast-follow once this mechanism has seen real use, not
  bundled into this slice.
- Detect and resume interrupted `wspy-run` profiles, Phase A — surface incompleteness
  (Phase B, actually resuming, shipped separately above): `generate_manifest()` (`wspy-run`) only
  writes the run-level `manifest.json` after every pass finishes, so a mid-loop crash (the motivating case: a
  real host crash mid-batch) leaves per-pass `*.manifest.json` artifacts with no top-level manifest
  to tie them together — and, until now, `/report` silently mis-rendered such a directory as if it
  were an item-6 fixed-config single-pass report (`render_fixed_report()`, which only ever looks for
  the fixed `amdtopdown.*` names), and `/history`/the homepage's recent list didn't even discover the
  directory at all unless one of its files happened to match the fixed `TOPLEVEL_MARKER_FILES` set —
  neither `launch.log` (web-launcher-only, never written by a CLI-launched batch) nor `summary.txt`/
  `manifest.json` (both only ever written at the very end, by definition never reached) exist for a
  genuinely interrupted run. New `detect_incomplete_wspy_run()` (`web/server.py`) recognizes this
  shape from the completed passes' own per-process wspy manifests alone (each pass's `--manifest` is
  the last thing that pass's own wspy invocation writes, so its presence is solid evidence that pass
  finished) — deliberately leaves the one ambiguous case alone (exactly one manifest file, matching
  item 6's own legacy `amdtopdown.manifest.json` name, indistinguishable from a genuine item-6 report
  by filename alone; real profile pass ordering makes this case effectively unreachable in practice,
  see the function's own docstring). `render_report()` gained a third dispatch case
  (`render_incomplete_run_report()`) showing an "⚠ Incomplete run — N of M passes ran" banner (M via
  new `joblib.expected_pass_count_for_profile()`, recovered from whichever completed pass recorded
  `--preset-name`; `None`/"expected total unknown" for a `-c`/`--config` custom pass list, which never
  sets that flag) plus whatever real artifacts exist (reusing `collect_run_files()`'s own generic
  listing rather than a second one). `/history` gained a new `"incomplete"` status value and filter
  option (distinct from `"failed"`: every pass ran but the workload itself failed, vs. never got the
  chance to finish or fail at all). The discovery gate itself (`_looks_like_a_run_directory()`,
  shared by `discover_reports()`/`discover_run_history()`) now also accepts any `*.manifest.json`
  presence, not just the fixed marker set, so a CLI-launched interrupted batch is actually
  *findable* in the web UI at all, not just correctly labeled once you already have its direct URL.
  Verified against a real running server with synthetic interrupted-run fixtures (correct banner,
  artifact listing, `/history` row, homepage discovery, and the legacy-ambiguous case still falling
  through to `render_fixed_report()` unchanged) plus new `web/test_incomplete_run.py` (18 tests) and
  `ExpectedPassCountForProfileTest`/regression coverage in `web/test_joblib.py`.
- Preset/Configuration/Option vocabulary refactor, checklist-boolean-flag-table slice:
  the Run tab checklist's `tree`/`gpu` sections had 15 pure 1:1 "checkbox checked -> emit this one
  flag" mappings (`--tree-cmdline`/`--tree-open`/.../`--tree-nanosleep`, `--gpu-busy`/`--gpu-metrics`/
  `--gpu-smi`/`--gpu-nvidia`) hardcoded as one `if section.get(<key>): flags.append(<flag>)` block
  per option in `build_configuration_passes()` (`web/joblib.py`) — an independent, hand-written model
  next to `wspy-run`'s own now-data-driven `profiles/*.conf`. New `profiles/checklist-flags.conf`
  (`<checklist-key> <option-key> <flag>` lines, `#`-comments, read the same tolerant way
  `_load_profile_conf_passes()` already reads `profiles/*.conf`) replaces those 15 if-blocks with a
  small table lookup (`CHECKLIST_BOOLEAN_FLAGS`, loaded eagerly at import — a plain-text read, no
  built `wspy` binary needed, same posture as `ALL_GROUPS`). Deliberately a sibling data source, not
  literally the same grammar `wspy-run` reads: a fixed preset pass-list has no checkbox concept for a
  boolean-toggle table to join onto, so "unify" here means "same declarative shape/mechanization",
  not "one shared file". The `counters`/`system`/`ibs` sections' genuinely conditional logic (interval/
  `--passes` bin-packing choice, `--power` folding, legacy pass-name heuristics) stays hand-written —
  forcing it into a flat table would obscure it, not simplify it — and the preset/checklist boundary
  stays exactly as it was (see item 1's own updated text above for why decomposing presets into
  checklist state remains explicitly out of scope). Verified byte-identical flag order against the
  original if-chain for both sections' full-on cases; also closed a real, pre-existing test-coverage
  gap found while doing this — these 15 flags had no test coverage at all before (`web/test_joblib.py`
  gained `LoadChecklistFlagTableTest` plus tree/gpu exact-order regression tests). `CLAUDE.md`'s
  "Common edits" gained a matching entry so a future boolean checklist option lands in the data file,
  not a new Python branch.
- Linked navigation between the tree and interval/timeline views, feature (c) — the item's last
  remaining scope, now fully shipped: `interval_viewer.js`'s full-column timeline page
  (`render_interval_viewer()`, `web/server.py`) gains an optional "Tree + timeline (combined)" link,
  same place and wording as the report page's own reciprocal link. The `/interval-viewer/...` route
  handler resolves it via the same `joblib.find_combined_timeline_csv()` the report page already
  uses, deliberately looked up against the *run* rather than requiring the currently-viewed CSV to be
  the eligible one — a reader viewing a plain `--interval` pass's every-column output can still jump to
  a *different*, same-run `--tree`+`--interval` pass's combined view for process context, since that's
  the actual "relate them" need (a run can have more than one `--interval` CSV, only one of which is
  ever tree-eligible). `render_interval_viewer()` takes the already-resolved URL as a plain optional
  argument rather than re-deriving it itself, so the thin-HTML-shell function still needs no
  rundir/manifest access of its own. Verified with a real running server against a synthetic two-pass
  run directory (a plain `counters` pass plus a `tree-heavy` `--tree`+`--interval` pass): the link
  appears when viewing either pass's CSV, points at the correct `/timeline-viewer/...` URL, that URL
  actually 200s, and the link is correctly absent for a run with no `--tree` pass at all. Also covered
  by a new `web/test_interval_viewer_link.py` unit test (`render_interval_viewer()`'s own rendering
  contract; `find_combined_timeline_csv()` itself is `web/test_joblib.py`'s job, not re-tested here).
- Linked navigation between the tree and interval/timeline views, features (a)/(b):
  `web/static/timeline_viewer.js`'s combined tree+timeline viewer already shared one crosshair by time
  across the pct chart and the swimlane, and a bar's hover tooltip already included its
  `target_counters` — but going from a bump in the chart to *which process* caused
  it, or from a process to *its own* topdown breakdown, took eyeballing/re-deriving both ways. **(a)
  timeline → process:** hovering the pct chart now looks up every swimlane row whose `[start,finish]`
  covers that instant (`activeRowsAtTime()`), outlines those bars (`.tlv-bar-hot`) via
  `highlightActiveRows()`, and names them in the hover tooltip's new "active: ..." line — symmetric
  hovering the swimlane itself does the same, since a wide parent bar can still overlap a narrower
  still-running child elsewhere in the tree. **(b) process → topdown:** clicking a swimlane bar
  (`wireZoom()` gained an `onClick` callback, threaded through the existing click-vs-drag-zoom distance
  check) selects that process and renders a persistent "Process detail" panel below the swimlane —
  duration, `cmdline`, and its topdown/counter breakdown, computed by `computeDisplayCounters()`/
  `topdownRawCategories()` ported verbatim from `proctree_viewer.js` (kept as a duplicate, not a shared
  module, matching that file's own small-pure-helper-per-page precedent) so the breakdown always agrees
  with what the tree viewer's own Timeline mode would show for the same node. Selection is tracked by
  node identity (`selectedNode`, the same object reference `treeData.tree` already holds) rather than by
  row index, so it survives a `renderAll()` (zoom, threshold change, series-checkbox toggle) even if the
  selected process scrolls out of the currently-filtered row set. Verified via a hand-rolled jsdom
  DOM/event shim driving the actual shipped JS file against a fake tree+interval payload shaped like a
  real two-process SPEC CPU2026 `801.xz_s` run (44%/56% split) — hover-highlight, tooltip naming,
  click-to-select, topdown-category collapsing, and selection surviving a re-render all exercised against
  real dispatched `mousemove`/`mousedown`/`mouseup` events, not just read by eye; no browser available
  this session.
- `float_pct` `run_features` promotion (`store.c`'s `SIMPLE_METRIC_FEATURES`, `FEATURE_SET_VERSION`
  1.5→1.6): filed as #227 from a `compiler-flag-miner` design discussion (its GCC vector-width flag
  catalog couldn't distinguish an integer memory-bound workload from a genuinely FP-heavy one using
  `resource_dominance`/`memory_attribution` alone). The `float` metric (AMD-only FP-op density,
  `topdown.c:print_float()`) already reached `metric_values` as `[raw]`; this is just the promotion to
  a normalized per-run feature, justified by a reference-matrix data review (SPEC CPU2026 by-machine
  pages, cross-checked against `workload/cpu2026/spec_benchmarks.json`'s intrate/fprate ground truth)
  showing a clean bimodal split with no overlap between the two categories. See "Known gaps" below for
  what that review wasn't enough to justify yet.
- Vision-based topdown-chart analysis: `wspy-analyze --image` narrates a run's `plots/*.topdown.png`
  via a vision-capable Ollama model (default `gemma4:26b`), grounded in a real numeric summary of the
  same CSV data rather than the model's own pixel-reading. CLI and web UI both, wired into the curation
  studio (new `aivision.*`/`AIVISION_RE` naming) the same way the existing text narrative already is.
  See `doc/INVESTIGATION_ARCHIVE.md`'s "Vision-based topdown-chart analysis: live model comparison and
  design" for the live model comparison behind the design and how its open questions resolved.
- `wspy-run` builtin-profile vocabulary refactor (first slice): the 11 builtin profiles
  (`quick` through `zen4plus-deep`) moved from a hardcoded bash `case` statement
  (`PASS_NAMES`/`PASS_FLAGS` arrays) to data files under `profiles/*.conf` (plain pass lists, reusing
  `wspy-run`'s existing `-c <file>` grammar) and `profiles/*.spec` (composites) — `BUILTIN_PROFILES` is
  now discovered from the file set rather than hand-listed a second time. `web/joblib.py`'s
  `COMPOSITE_PRESET_PROFILES` now reads the same `*.spec` files instead of hand-mirroring `wspy-run`'s
  composition (a real, audit-found drift risk — see 4.4(a)'s own intro). New `wspy --list-groups`
  (`multipass.c`) makes the `--counters=`/`--passes=` group-name vocabulary machine-readable;
  `tests/group_vocab_check.sh` checks `web/joblib.py`'s `ALL_GROUPS` against it so that duplication
  can't silently drift either. `--list`/`--dry-run` output is unchanged (verified byte-identical per
  profile) except `--list`'s own enumeration order, now alphabetical (file-glob-derived) rather than
  hand-authored. Remaining scope (the web UI's own checklist/argv engine) stays open under item 1
  above — this was deliberately scoped as the mechanically-safe slice, not the full three-way
  unification.
- `wspy-run` builtin-profile vocabulary refactor (ARM group exposure slice): the 3
  ARM-only counter groups (`arm-dcache-mem`/`arm-icache-tlb`/`arm-mem-align-tlb`) are now real
  `web/joblib.py` `ALL_GROUPS` entries, so the web checklist's "Performance counters"/preflight cards can
  select them like any other group — `ALL_GROUPS` is now a full mirror of `multipass.c`'s
  `multipass_group_names[]` rather than missing this one carve-out (`tests/group_vocab_check.sh` stays a
  subset check on principle, not because this gap is still open). `COLUMN_TO_GROUP` also gained the 11
  raw-event CSV columns these 3 groups produce, so a custom plot asking for one of them now correctly
  auto-enables its group (`autofit_checklist_for_custom_plots()`/`build_supplementary_plot_passes()`)
  instead of silently warning. Remaining scope (the web UI's own checklist/argv engine unification onto
  `profiles/*.conf`/`*.spec`) stays open under item 1.
- Default curation: AI vision analysis output (`aivision.*.md`) is now included by the Studio's "Apply
  default curation" button when present, mirroring the existing narrative-analysis treatment —
  previously only `ai_artifact_label()`'s "AI narrative analysis" labels were swept into the fallback
  pass, so `wspy-analyze --image` output was silently skipped even when it existed in the run directory.
  `default_curation.conf` gained an "AI vision analysis" section (rendered-prompt/critique variants
  documented-excluded, same as narrative's); `build_default_curation_blocks()`'s fallback sweep now also
  matches the `"AI vision analysis ("` label prefix (the real per-image model output specifically, not
  its rendered-prompt/critique siblings, which share the same leading phrase).
- Vision analysis: per-plot-type prompt templates + multi-select web UI. `wspy-analyze --image` used to
  always render `prompts/vision_topdown.tmpl` regardless of which plot it was pointed at — fine for
  topdown (the only registered type), wrong wording for any other chart. Now
  `VISION_TEMPLATE_BY_PLOT_TEMPLATE` resolves a dedicated template from the image's own wspy-plot
  template name (`joblib.VISION_PLOT_KINDS`, shared with the web UI so the two vocabularies can't drift);
  two new templates registered — `prompts/vision_system_cpu.tmpl` (CPU busy/idle duty-cycle phases) and
  `prompts/vision_power_vs_frequency.tmpl` (power/frequency coupling, plateaus, wobble during
  transitions) — alongside the original `vision_topdown.tmpl`. An unregistered plot template is a hard
  `ap.error()` (never a silent fallback to mismatched wording); `load_template()`'s version-marker regex
  generalized from one hardcoded `VISION_TOPDOWN_TEMPLATE_VERSION` alternative to any
  `VISION_<NAME>_TEMPLATE_VERSION`, so a future template needs no matching code edit. The report page's
  "AI vision analysis" card replaced its single plot `<select>` with one checkbox per registered plot
  actually present in the run (topdown checked by default for continuity, the two new ones opt-in), still
  one launch button and one shared model selection — `execute_multi_vision_analyze()` runs each checked
  plot as its own `wspy-analyze --image` invocation, sequentially, in one combined SSE log, "done" only
  if every one succeeded. `default_curation.conf` places each plot's own `aivision.*` block immediately
  after that plot (previously the vision-analysis section was grouped near the top, ahead of every plot
  it might narrate) — general policy now, not just for topdown.
- `wspy --list-columns` + mechanized `PROFILE_PLOTTABLE_COLUMNS` ("extend `wspy.c`'s CLI
  flags"/"revisit `PROFILE_PLOTTABLE_COLUMNS`" slice): new standalone probe, `wspy --list-columns
  [<flags>]` (`wspy.c`, same "no separate workload launch" standing as `--capabilities`/`--preflight`/
  `--list-groups`) — prints the CSV header the given flag combination would produce and exits, by falling
  through the exact single-pass/`--passes` group-construction and header-printing code a real run uses
  (an early return right after each path's own CSV-header block, not a second hand-maintained copy of
  it) rather than dispatching to its own probe function the way the other three do. `web/joblib.py`'s
  hand-maintained `PROFILE_PLOTTABLE_COLUMNS` table (used by `build_supplementary_plot_passes()` to know
  which columns a preset's own passes already cover) is replaced by `profile_plottable_columns()`, a
  lazy, memoized query against this new flag: reads a builtin profile's `profiles/*.conf` (expanding
  `*.spec` composites via the already-shipped `expand_preset_names()`), finds its `--interval` pass(es),
  and asks the real `wspy --list-columns` what CSV header they produce — real per-host device names
  (`net enp2s0`, `disk nvme0n1 ...`) come back as literal columns, so the old `"net *"`/`"disk *"`
  wildcard special-case in `build_supplementary_plot_passes()` is gone too. Deliberately *not* a
  module-level constant computed at import time, unlike `ALL_GROUPS` — `joblib.py`'s own docstring
  requires importing it to never need a built `wspy` binary on PATH, so the query is cached per
  `(wspy_bin, profile token)` the first time it's actually needed instead, with an empty-set degrade on
  any subprocess failure (missing binary, non-zero exit, timeout) matching the old table's own
  "absent entry" default. Found and fixed a real, live drift bug in the process: the old hand-authored
  table claimed `ibs-basic`/`zen-portable` had zero plottable columns, when `ibs-basic.conf`'s pass has
  run `--interval 1 --csv` (producing `ibs_fetch`/`ibs_op`) all along — exactly the class of silent
  drift this item called out as the reason to mechanize it. Remaining scope under item 1: the web UI's
  own checklist/argv engine unification onto `profiles/*.conf`/`*.spec`.
- Correctness fix (#239): the Phoronix tab's inventory Installed column trusted a per-point `installed`
  flag frozen into `source.json` at materialize time, even though `list_installed_phoronix_test_versions()`
  (added for the version-mismatch/Re-pin badge) already had a live answer available. A point pinned to a
  version that *was* actually installed could still show a flat "no" with no Re-pin option if it had been
  materialized before that install happened — confirmed live against `workload/phoronix/openfoam/`'s two
  test points (pinned `1.2.0`/`1.3.0`, only `1.3.0` installed). `render_phoronix_inventory_groups()`
  (`web/server.py`) now treats the live scan as authoritative whenever it has any signal for that bare
  test name — exact match shows "yes" outright, a real mismatch keeps the badge + Re-pin — and only falls
  back to the stale flag when nothing of that test name is installed at all.
- Correctness fix (#240): `repin_phoronix_test_point()` only ever rewrote `<Execute><Test>` on a
  version re-pin, never `<Execute><Arguments>` — but a version bump can rename the argument itself.
  Confirmed live against the same `workload/phoronix/openfoam/` test point #239 above used: OpenFOAM
  1.2.0 → 1.3.0 renamed the drivaerFastback tutorial path from `incompressible/simpleFoam/
  drivaerFastback/` to `incompressibleFluid/drivaerFastback/`, so a suite re-pinned to
  `pts/openfoam-1.3.0` kept the old, now-nonexistent path and failed at run time even though the
  version-mismatch badge was clear. `repin_phoronix_test_point()` (`web/joblib.py`) now checks the new
  version's own `test-definition.xml` menu after rewriting `<Test>`: a still-valid `Arguments` is left
  alone (`"verified"`); a stale one is rewritten to the menu entry whose `Name` matches
  `<Execute><Description>`'s `"Input: <Name> - ..."` prefix, if one matches (`"updated"`, old/new
  values recorded, `source.json` gets `previous_arguments`); otherwise it's left untouched rather than
  guessed at (`"stale"`) and `web/static/app.js`'s repin button now surfaces that as a warning instead
  of reporting silent success.
- Combined process-tree + `--interval`-timeline viewer: `web/static/timeline_viewer.js`,
  `/timeline-viewer/<suite>/<benchmark>/<run_id>/<filename>`. Renders the process tree as swimlanes
  (fork→exit horizontal spans, DFS-flattened, colored by `comm`) under the same shared time axis as a
  percentage-metric chart (topdown/GPU columns only — the full-column interval viewer stays linked
  for everything else), so a reader can correlate a phase shift against which process was running.
  The load-bearing fact making this safe: `--tree` events and `--interval` CSV rows both derive from
  `topdown.c`'s single module-global `start_time`, so they only share one clock when produced by the
  *same* wspy invocation — a `--tree` pass and a separate `--interval` pass in the same `wspy-run`
  profile are two independent child-process launches with two independent clocks and must never be
  plotted together. New `joblib.find_combined_timeline_csv()`/`pass_manifest_wants_combined_timeline()`
  detect this from each pass's own wspy manifest (`options.tree`/`options.interval_seconds`, not
  structured configuration provenance, which doesn't record raw flags) — the report page only ever
  links to the combined view when a qualifying pass exists (e.g. `gpu-compute.conf`'s single pass,
  which already runs both together), and the route itself re-checks the same gate against a
  hand-edited URL rather than trusting the filename alone. No new JSON endpoint — reuses
  `/api/tree-json`/`/api/interval-json` in parallel. Row count is capped (`MAX_RENDERED_LANES`, 150)
  as a hard backstop independent of the min-duration filter, since a fork-heavy workload (hundreds of
  children starting/exiting within the same millisecond, e.g. a parallel build) makes the
  percentile-based default threshold degenerate to 0% — caught by an end-to-end smoke test against a
  real 203-process `wspy --tree --interval` run before landing, not just by inspection. Per-process
  topdown data reuses `--target`'s existing tree-JSON `target_counters` (a one-shot lifetime-total
  read at that process's exit, not a real time series yet — see `doc/INVESTIGATION_ARCHIVE.md`'s
  eventual Perfetto-export item for what a genuine per-process-over-time overlay would need). Verified
  against the real running server end to end (real `wspy --tree --interval` output, both small and
  203-process trees, report-page link wiring, 404 on a mismatched-pass URL); no browser available this
  session, so hover/zoom/tooltip interactions were exercised via a hand-rolled DOM/event shim driving
  the actual shipped JS file against real captured API responses, not just read by eye. Follow-up found
  during review: the Run tab's checklist path (as opposed to picking a builtin preset like
  `gpu-compute`) could never actually reach this feature — its "process tree" and "performance
  counters" checklist sections always become two separate, independently-clocked `wspy` invocations, so
  checking both boxes silently produced two passes that could never satisfy the same-invocation gate
  above, with no explanation on the page. Fixed by adding an "Interval seconds" field directly to the
  "Process tree" checklist card (`build_configuration_passes()`, `web/joblib.py`) — set alongside a
  counter group there, it folds `--interval <n> --csv` into that same tree pass rather than starting a
  second one; blank leaves the previous tree-only behavior unchanged. Verified against the real running
  server via `/api/run-custom` end to end (both the combined case and the still-plain-tree-only
  regression case), not just by inspection.
- Tree viewer's own "Timeline" view mode (`proctree_viewer.js`, `/tree-viewer/...`, single-tree mode
  only): a second rendering of the exact same `/api/tree-json` data as swimlanes by process
  start/finish, alongside the existing default "Tree" hierarchy rendering — no server-side change at
  all. Distinct from the combined tree+timeline viewer above in one load-bearing way: it has no
  `--interval` dependency whatsoever, since the x-axis is each process's own start/finish (present in
  every `--tree` run) rather than a periodic sample tick — works on any `--tree` run, not just the
  narrow same-invocation `--tree`+`--interval` combination that page requires. A `--target`-matched
  process whose counters resolve to the topdown 4-category breakdown (`computeDisplayCounters()`,
  already used by the tree hierarchy view's own per-node columns) is colored by its own Retiring %
  (good/warn/bad, this app's existing verdict-bucket convention) instead of the plain comm-based
  categorical color everything else gets — promoting `--target`'s per-process counters from a
  hover-tooltip afterthought to the actual visual encoding. Ported (not re-derived) the row-count hard
  cap (`TIMELINE_MAX_LANES`, 150) from the combined viewer's own fix above, same fork-heavy-workload
  reasoning. Verified via a hand-rolled DOM/event shim against real captured `/api/tree-json` responses
  from a real `--target=comm=sleep` run (confirmed matched processes render `--bad`-colored per their
  actual ~9.5% Retiring reading) and a real 203-process tree (confirmed the 150-row cap holds, and
  switching modes back and forth doesn't change the tree-mode row count) — no browser available this
  session.
- Phoronix single-iteration option (#243): `workload/phoronix/run_test.sh` gained an `ITERATIONS`
  env var (e.g. `ITERATIONS=1 TESTNAME=coremark ./run_test.sh`), exporting phoronix-test-suite's own
  `FORCE_TIMES_TO_RUN` (`pts-core/objects/pts_env.php`) so `batch-run` executes each test exactly that
  many times instead of PTS's normal dynamic repeat count (often 3+) — `batch-run` itself takes no CLI
  flag for this, only an env var. Most useful as `ITERATIONS=1` paired with a `--tree` pass, where 3
  near-identical copies of the same subtree just add noise to browse. Wired the same override through
  the web launcher as a Run tab "Single iteration" checkbox: `joblib.phoronix_single_iteration_env()`
  is applied to every `Popen` that launches the workload in both `execute_profile_run()`/
  `execute_custom_run()`; the live preview surfaces an advisory note since an env var never shows up
  in an argv preview line (including a no-op warning when checked against a non-phoronix workload);
  queued jobs carry a `single_iteration` field (`JOB_SCHEMA_VERSION` 1.1.0 → 1.2.0, additive), and
  `wspy-queue add --single-iteration` gives the CLI the same knob.
- Correctness fix (#244): `_build_phoronix_suite_xml()` (`web/joblib.py`) already fell back to the
  `arguments` string for `<Execute><Description>` when the source XML had no real `Description`, but a
  test point imported with *both* fields empty (no option ever pinned for that test) still produced an
  empty `<Description>` element. A real PTS install treats "no non-empty Description" as license to
  batch-run every option in the test's menu rather than just the imported one, so this silently turned a
  single-option import into a much bigger, unintended run. Fixed by falling back to the literal
  `"default"` when both `description` and `arguments` are empty. Confirmed live 2026-08-15: imported a
  `build-linux-kernel` test point with both fields empty and ran it through the web queue — `launch.log`
  showed `Test 1 of 1` against `[default]` only across all three passes (counters, tree, timeout
  estimator), exit code 0 throughout, where the unpatched code would have re-run the full option menu.

## Known gaps (still open)
Real-hardware/real-scale validation this project's hand-testing hasn't covered yet. Not release
blockers — just don't assume these are confirmed:
- **AMD IBS `fetchlat` threshold minimum unverified:** unlike `ibs_op`'s `ldlat` field (see above,
  now fixed), no host available during that validation exposed a working `fetchlat` sysfs format
  field on `ibs_fetch` to test whether it has an analogous hardware-enforced minimum below
  `IBS_DEFAULT_FETCHLAT_THRESHOLD`'s current value of 120. Worth the same bit-sweep treatment once
  hardware with a live `fetchlat` field is available.
- **Sub-~10ms target processes can read back all-zero/`-nan` counters** — a real perf-subsystem timing
  limitation (the kernel never finishes scheduling the counter onto real hardware before a very
  short-lived child exits), not a wspy bug. Essentially absent on realistic (>0.3s) workloads; no fix
  planned. Root-cause investigation archived in `doc/INVESTIGATION_ARCHIVE.md`.
- **`vectorization_density` archetype axis thresholds not yet set (#227):** `float_pct` (above) is now a
  `run_features` value, and `archetype.c`'s header comment already sketches the one-`SIMPLE_AXES`-entry
  extension path, but the reference-matrix data behind its promotion is single-machine, `n=1` per test,
  one compiler config (`gcc_o3-base`) — a clean intrate/fprate separation (≈0.6–5.6% vs. ≈14.5–34.0%,
  zero overlap, corroborated on a second machine for the tests it has so far), but too thin to commit to
  `low`/`moderate`/`high` boundaries yet, per this doc's own no-thresholds-without-real-data rule. Revisit
  once more machines/compiler configs/repeat runs are in the reference-matrix corpus.

## Track deep-dives
Reasoning that doesn't fit a single backlog line, for tracks with genuinely open work. Deep-dives for
work that's since fully shipped (blocking I/O, schedstat, vmsize, connect/wait/poll/nanosleep, power,
the LLM narrative-analysis design, the vision-based topdown-chart analysis design, the full
critical-path-instrumentation candidate rationale, repeatability policy/confidence metadata, comparison
matrix mode, and symbol-level profiling) have moved to `doc/INVESTIGATION_ARCHIVE.md`. The Zen5/IBS and
Topdown deep-dives below have also each been trimmed to just their one remaining open thread — the
confirmed-platform-behavior background that fed their now-shipped items moved to the archive too.

### Zen5/IBS deep-dive
The confirmed-platform-behavior background that fed 4.2's "Zen-family preset packs"/"PMU-capability-
aware comparability warnings" and 4.3's AMD IBS sampling-mode support (all shipped) has moved to
`doc/INVESTIGATION_ARCHIVE.md`'s "Zen5/IBS platform-behavior findings" now that everything it informed
has landed. One thread remains open:

- Zen5's topdown dispatch baseline shifted from Zen4's 6 slots/cycle to 8 — already implemented
  (`topdown.c`'s `CORE_AMD_ZEN`/`CORE_AMD_ZEN5` slot-multiplier branch). But the finer per-scheduler
  breakdown events AMD introduced alongside that width change aren't in `amd_raw_events[]` yet: split
  ALU/AGU scheduler-stall counters, and op-cache/execution-queue events that would separate `Frontend
  Latency` from `Frontend Bandwidth`. `IBS_LD_L1_DTLB_REFILL_LAT` also isn't named anywhere in the IBS
  capability-probing rows. Tracked as 4.5's "Zen5 fine-grained scheduler-stall counters" item — both are
  also candidate inputs for 4.5's "Platform formula registry" item once Zen5-specific formulas are
  actually versioned there.

Caveat: if upstream kernel/perf exposes new Zen5-specific generic mappings or PMU caps, update
presets and coverage logic without changing the report schema.

→ Now unblocks 4.3's "IBS-derived memory-path bottleneck decomposition" (shipped, see "What shipped in
4.3"). `--interval`-integrated periodic IBS sampling rates (needs a real poll-loop architectural change)
was deliberately deferred out of that item's scope — see "Deferred indefinitely" above.

### Intel hybrid / counter-grouping deep-dive
Real Intel hybrid hardware became available for the first time this cycle (a Raptor Lake HX host,
codenamed "carlsbad", 2026-07-22) and immediately surfaced a cluster of confirmed, hardware-verified
counter-grouping bugs in `topdown.c` — these predate this investigation entirely (the shared-group
design dates to commit `273e9af`, Dec 2023) and were never caught before because no Intel hardware
existed in this environment to exercise them. All five original findings plus the Gracemont E-core
raw-event gap are now resolved (four fully shipped, one split — the corrupt-percentage half shipped, the
all-zero/`-nan` half is a documented non-actionable perf-subsystem limitation, see "Known gaps" below) —
full root-cause/fix/verification detail moved to `doc/INVESTIGATION_ARCHIVE.md`'s "Intel hybrid /
counter-grouping real-hardware findings and fixes" now that nothing here remains open backlog.

Additional Intel counters worth adding, grounded in the same real-hardware pass (`/sys/bus/
event_source/devices/` enumerated live, not from documentation alone). The GPU item (`i915` GPU PMU) is
tracked in 4.4(b); everything else here is tracked as 4.5's "Intel counter expansion" item:
- **Real DRAM bandwidth** (`COUNTER_MEMORY`, nonexistent for Intel today). `uncore_imc_free_running_0`/
  `_1` expose `data_read`/`data_write`/`data_total` with their own `.scale`/`.unit` sysfs files — the
  exact shape `power.c` already knows how to parse; comparatively low-effort riding on existing code.
- **True LLC/L3 counters.** Today's `l2_request.all`/`.miss` (`COUNTER_L2CACHE`) is genuinely L2, not
  L3 — Intel has no L3-layer entry at all, unlike AMD's `COUNTER_L3CACHE`. `uncore_cbox_0`..`_11` (12
  CBox/LLC-slice PMUs on this host) would give real chip-wide LLC hit/miss/occupancy.
- **Generic `PERF_TYPE_HW_CACHE` coverage is incomplete on this microarchitecture.** Confirmed live on a
  real Coremark run (Raptor Lake HX, 2026-07-22): `l1i-read` (L1I read-access), `iTLB-loads` (ITLB
  read-access), and `dTLB-load-misses` all failed `perf_event_open()` with `EINVAL` while their sibling
  events in the same request (`l1d-read`, `l1i-read-miss`, `dTLB-loads`, `iTLB-load-misses`) succeeded —
  consistent with the well-known Linux-perf reality that several generic `PERF_COUNT_HW_CACHE_*`
  (cache,op,result) combinations simply aren't wired up in the kernel's per-microarchitecture Intel PMU
  mapping table, not a wspy grouping/request bug (ruled out separately: this reproduced with the 4.3 Tier
  0 counter-group budget fix already in place, each event opening as its own correctly-sized group).
  Reinforces the **True LLC/L3 counters**/**Real DRAM bandwidth** items above rather than adding new
  scope: the fix for any specific missing combo is the same one — a real Intel raw/uncore MSR event
  replacing the generic `PERF_TYPE_HW_CACHE` abstraction for that slot, not a wspy-side workaround (there
  isn't one; the kernel refuses the open before wspy sees anything to work around).
- **Intel per-core-domain and iGPU RAPL energy** — a genuinely different discovery shape than AMD's.
  This host has no `power_core` PMU; instead the *same* `power` PMU exposes `energy-cores`/`energy-gpu`
  as additional named events alongside `energy-pkg`. `energy-gpu` is notable on its own: real iGPU
  energy with no GPU vendor build flag needed.
- **`i915` GPU PMU** — an Intel-native busy/frequency alternative to the current AMD-sysfs/NVML-only GPU
  support, `perf_event_open()`-based rather than a vendor SMI/sysfs scrape.
- **C-state residency** (`cstate_core`/`cstate_pkg` PMUs) — idle-state breakdown, useful context for
  `--power`'s energy numbers; AMD has no direct equivalent PMU.
- **PEBS-based precise memory-latency sampling** (`MEM_TRANS_RETIRED.LOAD_LATENCY`-style events) — the
  natural Intel counterpart to AMD IBS sampling-mode support (shipped, see "What shipped in 4.3"),
  comparable in spirit to IBS's `IbsOpData`/`DcMiss`/`NbIbsReqSrc` tag bits. Not investigated in depth
  this pass; worth scoping now that IBS sampling-mode's mmap-ring-buffer/per-sample decode
  infrastructure exists to model this against.

### Topdown deep-dive
Everything this originally tracked has shipped — multiplex-aware confidence/decomposition sanity checks
(4.0), the hierarchical L1→L2→L3 schema (4.2), and phase-aware topdown/cross-signal attribution/hybrid
core-class summaries (4.3) — except one item:

- Platform formula registry — versioned event/formula mapping per CPU family/model, for auditability.
  Tracked as 4.5's "Platform formula registry" item; see the Zen5/IBS deep-dive above for concrete
  candidate inputs once this exists.

### Preset / Configuration / Option hierarchy deep-dive
A three-level vocabulary for describing what wspy can be asked to do, surfaced while iterating the
4.1 web-interface mockup (2026-07-11) — the goal is for the CLI, `wspy-run`, and the web UI to
describe the same thing the same way, rather than each inventing its own mental model.

- **Configuration** — one thing wspy can be asked to measure, typically corresponding to a single
  wspy run/operation (though a run can combine more than one). Examples: a process tree, an interval
  measurement of performance counters, an interval measurement of other system metrics, an overall
  (non-interval) system measurement, an overall performance-counters measurement. Each configuration
  has a natural output representation — a table, a gnuplot, a tree diagram — which is part of what
  makes it a distinct configuration rather than a variant of another one.
- **Option** — a way to customize a configuration. Options apply to specific configurations, not
  universally: an interval-seconds option only makes sense on an interval configuration;
  `--tree-cmdline` only makes sense on a process-tree configuration.
- **Preset** — a configuration, or combination of configurations, common enough to deserve a name.
  Presets are exactly the things that showed up hand-rolled in `workload/*/run_test.sh` before 4.0 and
  are now `wspy-run --profile` entries. Presets can be hierarchical — a preset can itself specify
  particular configurations at particular option values (`deep-cpu` selects several configurations at
  fixed options). Not every reachable configuration/option combination has, or needs, a preset name;
  presets are the well-worn paths, not the full space.

The load-bearing rule this implies: **a preset names a configuration+option combination `wspy-run`
already knows how to run; the moment a preset's options are customized away from what it names, the
result has left the set of things `wspy-run --profile` can express, and has to run as one or more
direct `wspy` command lines instead.** This isn't a new rule invented for the web UI — it's the same
fatal-combination behavior the real CLI already has (`--passes`, which is what `wspy-run`'s profiles
bin-pack onto, rejects `--interval`/`--per-core`/`--tree`/IBS/GPU flags outright; see `CLAUDE.md`'s
`wspy.c` entry). The mockup's "customize a checkbox → separate command lines with an explanatory note"
fallback (shipped in the web launcher's Run tab) is this rule, discovered bottom-up from the real
constraints before being named top-down here. Worth treating as the general rule going forward rather
than a fact specific to counters/tree/interval, since it will recur every time a new configuration or
option is added.

Realized as shipped 4.1 features (see "What shipped in 4.1" above): the web launcher's preset-first
framing with a live "customized away from preset" indicator, and structured configuration provenance
recording which preset/configuration/option choice actually produced a run so a report can say "this
was `deep-cpu`, with the TLB group swapped for L3" rather than re-deriving it from a flat argv.

Cross-cutting goal, promoted 2026-08-07 to 4.4(a) scope, now resolved as far as any open backlog item
drives it (the item itself has shipped and is off the list — see "Shipped since 4.3.1"): the same
preset/configuration/option vocabulary should describe `wspy-run`'s profile format, not just the web
UI. Landed: `wspy-run`'s own builtin profiles moved off hardcoded `PASS_NAMES`/`PASS_FLAGS` bash arrays
onto `profiles/*.conf`/`*.spec` files; the web UI's checklist argv builder
(`build_configuration_passes()`) now consults declarative data too, for the slice of it that's a pure
1:1 checkbox/option-to-flag mapping (`profiles/checklist-flags.conf`), with matching `data-key`-driven
generic reading client-side (`app.js`). Two things this explicitly did **not** end up doing, both
deliberate rather than overlooked: decomposing a named preset into equivalent checklist state (or vice
versa) — presets stay atomic, per the load-bearing rule above; and restructuring `wspy`'s own CLI flag
parsing (`wspy.c`, still an unstructured flat flag list) around this vocabulary — raised as an idea
here but never turned into a concrete backlog item, so treat it as dropped rather than pending unless a
real need for it surfaces.

### Critical-path / synchronization-latency: what's left
All six originally-scoped syscall-latency candidates (futex, blocking I/O, connect, nanosleep, wait,
poll) have shipped — see "What shipped in 4.2" above and `doc/INVESTIGATION_ARCHIVE.md` for the full
motivation and per-syscall design rationale. What remains open from this track:
- The *general*, table-driven mechanism (`tree_open`'s "syscall name → number → decode →
  log-vs-aggregate" generalization) was deliberately not built — six syscall families were still cheap
  enough as individual `if` branches in `ptrace_loop()`'s dispatch. Tracked in "Deferred indefinitely"
  above; revisit only if a seventh syscall family comes up.
- `ptrace` itself imposes a real stop-the-world cost on every syscall of the traced process, so
  absolute latency numbers collected this way are inflated relative to an untraced run. The *relative*
  split (fraction of wall time in futex-wait vs. read-wait vs. on-CPU) stays informative even when
  absolute numbers are skewed, but this is an inherent limitation of the mechanism — the low-overhead
  tracing alternative to `ptrace` that would fix it is also in "Deferred indefinitely" above, not
  actively planned.

### Installed-test-profile materialization deep-dive
Investigated 2026-08-15 against real data on this host (54 installed tests under
`~/.phoronix-test-suite/installed-tests/pts/`, plus the `workload/phoronix/*` suites already
materialized here via the existing composite.xml/OpenBenchmarking import path) — feeds 4.4(c)'s
"Materialize test points directly from installed/downloaded PTS test profiles" item above.

**The gap.** `materialize_phoronix_test_point()` (`web/joblib.py`) and `wspy-phoronix-import` already
turn a Phoronix XML source into one single-test-point suite per (test, option-combination) — but every
source they accept (an installed suite-definition.xml, an OpenBenchmarking result export, a cached
composite.xml) is *evidence a specific combination was already run*. There's no path from "I have
`pts/llama-cpp` installed" straight to "here are the option combinations I can materialize," short of
first driving the test through `phoronix-test-suite run`'s own interactive/batch menu (or its
BATCH_MODE env-var equivalent) to produce a composite.xml to import — a real round trip a human
shouldn't need just to get a pick-list. Two pieces of prior art already exist and should stay the
source of truth for how test-definition.xml's option shape is read, rather than re-deriving it a third
way:
- `wspy-ledger --phoronix-option-combos` (ledger.c, "What shipped in 4.3") already statically parses
  `<TestSettings>/<Option>/<Menu>/<Entry>` for a *count* (product across `<Option>` blocks; a
  free-form, non-`<Menu>` option is excluded and the count flagged as a lower bound) — the new
  discovery code should agree with this exclusion rule rather than diverge from it.
- `_phoronix_test_definition_path()`/`_phoronix_menu_entries()` (`web/joblib.py`, added for
  `repin_phoronix_test_point()`'s menu-validation step) already resolve a `test_id` to its
  `test-definition.xml` and read its `<Menu><Entry>` list — but flatten across every `<Option>` into one
  list, which is fine for repin's single-arguments-string validation but loses the per-axis grouping
  this needs; extending it to return `[(display_name, identifier, arg_prefix, arg_postfix, [(name,
  value), ...]), ...]` per `<Option>` (order-preserved) covers both callers.

**Confirmed real profile shapes** (read directly from `test-definition.xml` on this host, not assumed):
zero-`<Option>` fixed tests (`coremark`, `aobench` — exactly one implicit point, "default"),
single-`<Option>` tests, and multi-`<Option>` tests where a naive cross product explodes fast:
Blender's `Blend File` (6 entries) × `Compute` (6, including `CUDA`/`OptiX`/`HIP`/`METAL`/`ONEAPI`) =
36; llama.cpp's `Backend` (5, including `CUDA`/`ROCM`/`SYCL`/`VULKAN`) × `Model` (8 downloaded
`.gguf` files) × `Test` (4 prompt/generation sizes) = 160. Auto-expanding either is exactly the
"combinatorial blow-up nobody asked for" the user framing this item called out.

**Confirmed Arguments/Description composition rule** — read back from `workload/phoronix/llama-cpp/*`,
`workload/phoronix/blender/*`, and `workload/phoronix/openvino/*`'s already-materialized `source.json`
files (all produced by a real PTS batch run + import, so this is observed behavior, not guessed):
- `Arguments` = each selected `<Option>`'s `<ArgumentPrefix>` + chosen `<Entry><Value>` +
  `<ArgumentPostfix>` (prefix/postfix omitted when absent), concatenated in document order, one space
  between options. E.g. Blender's `Compute=CPU-Only` + `Blend File=BMW27` →
  `-b ../bmw27_gpu.blend -x 1 -F JPEG -f 1 -- --cycles-device CPU` (note: `<Option>` order in the XML
  puts `Blend File` first even though `Compute` is discussed second above).
- `<TestSettings><Default><Arguments>` (e.g. Blender's `-noaudio --enable-autoexec`) is **not** part of
  this string — it's applied separately at execution time regardless of which option combination is
  picked, confirmed by its absence from every materialized point's `Arguments`. A composer function must
  not fold it in.
- `Description` = each selected option's `<DisplayName>: <Entry><Name>`, joined by `" - "` in the same
  order — e.g. `"Backend: CPU BLAS - Model: DeepSeek-R1-Distill-Llama-8B-Q8_0 - Test: Prompt Processing
  512"`. This is also what `repin_phoronix_test_point()`'s `_PHORONIX_ARGUMENTS_DESCRIPTION_RE` already
  parses back out for its own single-axis case — a multi-axis composer and that regex need to agree on
  the format, not just the single-axis case.
- `<Execute><Description>` must be present (any non-empty text) or a real PTS install batch-runs every
  menu entry regardless of `<Arguments>` — `_build_phoronix_suite_xml()`'s existing comment already
  documents this trap; a multi-axis composer inherits the same requirement, just building both strings
  from the axis picks instead of one.

**Design:**
1. **Discovery** enumerates `installed-tests/<namespace>/<name>-<version>/` (what `phoronix-test-suite`
   has actually *built*, not just downloaded — the same distinction
   `list_installed_phoronix_test_versions()` already draws against `test-profiles/`), resolves each to
   its `test-definition.xml`, and classifies by axis count.
2. **Zero axes:** one implicit point, materialize directly (same as an empty-Arguments call today) —
   no picker needed.
3. **One axis:** every entry is independently a complete, standalone configuration — list all N as
   flat candidates with a "materialize all" convenience action. This is the "single option/choice"
   case the item's own framing calls straightforward, and it's still just N independent calls into the
   existing `materialize_phoronix_test_point()`, no new materialization code needed once composition is
   axis-aware.
4. **Two or more axes:** never auto-expand the product (the same count `--phoronix-option-combos`
   already reports). Present a per-axis checklist instead; a submission is one or more explicit tuples
   (one value chosen per axis per tuple), each composed into an `Arguments`/`Description` pair via the
   rule above and materialized individually. A picker that only allows building explicit tuples — never
   a "select all values on every axis" shortcut that's secretly the cross product — is what keeps this
   from silently becoming case 4's blow-up with extra steps.
5. **GPU/backend axis flagging.** Heuristic keyword match against an axis's `DisplayName`/`Identifier`
   (`compute`, `backend`, `device`, `gpu`, `api`, `platform`) and independently against entry
   `Name`/`Value` text (`CUDA`, `OptiX`, `HIP`, `ROCm`, `Vulkan`, `SYCL`, `oneAPI`, `Metal`, `OpenCL`) —
   either match flags the axis. A flagged axis starts with nothing pre-selected and a visible badge;
   it's never defaulted or bulk-selected the way a plain non-flagged axis's "materialize all" can be.
   `check_gpu_build()` (`web/server.py`, already used by the Run tab's GPU checklist) answers a related
   but distinct question — whether *wspy's own build* can measure GPU counters during the run — and is
   worth surfacing alongside as a second badge, but doesn't answer whether the *workload's* chosen
   backend (e.g. a CUDA runtime for Blender's `Compute=CUDA`) can actually execute; that's a separate,
   harder question this design deliberately doesn't try to answer authoritatively (see next point).
6. **Best-effort "looks buildable" hint, never a filter.** Cross-reference each entry's `Value` text
   against the installed test's own `installed-tests/<name>-<version>/` directory listing — confirmed
   useful live: this host's `llama-cpp-2.5.0` directory contains `llama.cpp-BLAS`/`llama.cpp-ROCM`/
   `llama.cpp-SYCL`/`llama.cpp-VULKAN` binaries (no `llama.cpp-CUDA` — that backend's build was skipped
   or failed on this host) and every downloaded `.gguf` model file, so a substring match against either
   set is a real, checkable signal for *this* test. It's still per-test undocumented PTS install-script
   behavior with no cross-test guarantee, so treat a match as "confirmed built" and a non-match as
   "unconfirmed" (badge only) rather than hiding unmatched entries — a heuristic false negative must
   never make a real option unreachable.
7. **CLI equivalent**, for parity with the web picker and to keep `wspy-phoronix-import` scriptable:
   `--from-installed <test_id> --option <identifier>=<value> [--option ...]` materializes exactly one
   tuple per invocation (composing Arguments/Description the same way); a `--all-single-axis` convenience
   flag covers case 3 only. Deliberately no flag that expands a whole matrix in one call — the discipline
   belongs to the tool, not to hoping nobody passes `--all-axes`.

This is additive: the existing suite-XML/composite.xml/OpenBenchmarking-URL import sources
(`wspy-phoronix-import`'s current modes) stay exactly as-is for reproducing an already-published or
already-run result. This is a third, proactive source for building candidate points *before* ever
running anything through PTS.

## 4.4 priorities
Goal: refocus away from adding more analysis surface and toward (a) making the large amount of
functionality already shipped easier to actually use, (b) GPU support parity with the CPU side, and
(c) building out Phoronix suite coverage/workflow. Items are grouped by focus, not by dependency tier —
pick items within a group in any order that fits.

**4.4(a) — Ease of use / one-click web UI flows.** Grounded in a 2026-08-07 audit of the actual surface
area rather than guesswork: 16 separate CLI entry points (`wspy`, `wspy-run`, `wspy-validate`,
`wspy-ledger`, `wspy-store`, `wspy-summary`, `wspy-plot`, `wspy-core-report`, `wspy-archetype`,
`wspy-testpoint`, `wspy-publish`, `wspy-queue`, `wspy-sweep`, `wspy-analyze`, `wspy-bundle`,
`wspy-symbolize`) with no unified pipeline or suggested order; the report/studio/reference web pages
have accumulated roughly ten independent manual-trigger actions (generate process-tree views, AI
narrative analysis, characterization badge, similarity panel, apply default curation, add freeform
curation block, export, publish to WordPress, publish test-point report, publish reference matrix,
discover from WordPress), each its own card added the moment its capability shipped, with nothing on
the page indicating order or which are "normally do this" vs. optional; and per-suite flags
(`--phoronix-dest-root`/`--cpu2026-dest-root`) and run-identity conventions (`--run-id` alone vs.
`--hostname`+`--command` vs. `--hostname`+`--run-id`, depending which tool) have drifted apart tool by
tool despite resolving near-identical shapes underneath.

1. One-click end-to-end pipeline. Today a human runs `wspy-run`, then separately has to already know to
   run `wspy-store`'s ingest, `wspy-testpoint select-runs`, `wspy-testpoint render`, and a publish step —
   each its own command or its own web-UI button, in an order nowhere written down for a CLI-only user.
   Chain the common path (a finished run → ingested → selected → rendered → published) into one action,
   with a dry-run/preview step before anything writes or pushes, mirroring the caution
   `scripts/publish_reference_matrix.py`'s web button already applies ("Preview (dry-run)" checked by
   default). Web UI first — it already has every piece as a background-thread/SSE card; a CLI wrapper is
   a natural follow-on once the sequencing is settled, not a prerequisite.
2. CLI flag/identity consistency pass. Two concrete inconsistencies found in the 2026-08-07 audit: (1)
   `--phoronix-dest-root`/`--cpu2026-dest-root` are separate flags even though both suites resolve
   through the identical `find_materialized_*_test_point()` shape — every future suite added this way
   means another bespoke flag pair rather than one generic mechanism; (2) a run is identified three
   different ways depending which tool you're using, for a real reason (`wspy-summary`'s own rationale:
   two runs can share identical command+hostname when one is a redo of the other) but with that rule
   undocumented as a single cross-tool convention anywhere a user would find it before hitting the
   surprise. Audit and, where safe, unify; where a difference is load-bearing, document it once in one
   place rather than re-deriving it per tool.
**4.4(b) — GPU support:**

3. `rocprof`/`roctracer` deep profile (HIP kernel/memcpy/runtime activity, occupancy indicators) —
   heavier, optional trace-rich profile, same "default vs debug profile" pattern as IBS.
4. Queue/SDMA diagnostics (compute-queue utilization, copy/compute overlap, imbalance flags) — builds on
   4.2's GPU fusion layer (`gpu_fusion.c`, `--gpu-metrics`) for consistent per-metric data.
5. GPU coverage ledger (backend/device-class support, caveats) — same pattern as `wspy-ledger`, extended
   once GPU runs feed the same index.
6. Intel `i915` GPU PMU — an Intel-native busy/frequency alternative to the current AMD-sysfs/NVML-only
   GPU support, `perf_event_open()`-based rather than a vendor SMI/sysfs scrape. See the Intel hybrid /
   counter-grouping deep-dive for detail (the rest of that deep-dive's counter wishlist is non-GPU,
   tracked in 4.5).

**4.4(c) — Phoronix suite build-out:**

7. Phoronix-specific telemetry segmentation (`wspy-phoronix-segment`) — partitioning unified telemetry
   CSVs into per-test-case/per-trial datasets by correlating run manifests with PTS results,
   composite.xml, and log timestamps. See
   [phoronix_hook_investigation.md](file:///home/mev/source/wspy/doc/phoronix_hook_investigation.md)
   for design and prototypes. **Capture instrumentation already landed:**
   `scripts/pts_hooks/*.sh`/`scripts/setup_phoronix_hooks.sh` register PTS `result_notifier` hooks and
   capture their output into a per-pass `pts_hooks.log` artifact across every launch path — see
   `doc/INVESTIGATION_ARCHIVE.md`'s "Phoronix `result_notifier` hook capture" write-up. **Still open:**
   teaching `wspy-phoronix-segment.py` to prefer `pts_hooks.log` over composite.xml/log-timestamp
   correlation, and the segmentation tool itself.
8. `wspy-run`-profile-driven batchable equivalent of the single-test-point Phoronix suite flow
   (`web/joblib.py`/`wspy-phoronix-import`/web launcher's Phoronix tab — see "What shipped in 4.3" for
   what's already landed) — a saved profile or `-c` file, run non-interactively/scriptable/batchable
   across many materialized test points at once. Only the direct wspy/checklist Run tab path (one test
   point, launched by a human clicking Run) exists today.
9. Materialize test points directly from installed/downloaded PTS test profiles, with no prior PTS
   run/import round-trip. Today's only materialization paths (`wspy-phoronix-import`, "What shipped in
   4.3") both require *evidence a specific option combination already ran* (an installed
   suite-definition.xml or a composite.xml result) — there's no path from "this test profile is
   installed" straight to "candidate test points a human can pick from" without first driving it
   through PTS's own interactive/batch menu once. See the "Installed-test-profile materialization
   deep-dive" below for the full design: zero/single-option profiles materialize directly or as a flat
   pick-list, multi-option profiles get an explicit per-axis picker (never an auto-expanded cross
   product), and GPU/backend-shaped axes are flagged for explicit confirmation rather than defaulted.
   Web UI: a third Phoronix-tab source alongside the existing installed-suite/OpenBenchmarking-URL
   import sources.

## 4.5 priorities
Goal: lower priority than 4.4 but still real, wanted work — pick up once 4.4's three focus areas are
substantially done. Not ordered into dependency tiers; a few internal dependencies are called out inline.
Cross-referenced by name, not number, per this doc's own convention — item numbers here will shift the
next time this section is reorganized.

**Report-layer additions on data already collected:**

1. `--tree-open` → file-I/O topology summary (hot paths, open-failure rates, startup storms,
   process→file maps) — `tree_open`/`SYS_openat` capture already exists (`topdown.c`).
2. System (`--system`) → per-interface network attribution and local-vs-system-pressure attribution,
   plus steal-time capture (user/system/iowait are already captured and printed — this item is the
   missing steal column and the analysis layer on top, not the raw mix itself).
3. Tree/lifecycle enrichments (exit code/signal summary, spawn/exit burst indicators, optional
   `comm`-pattern role tagging).

**Infra:**

4. Collapse `wspy-run`'s builtin profiles onto native `--passes` bin-packing. Most profiles are already
   collapsed as far as `--passes`'s own architectural constraints allow (their remaining separate passes
   use `--interval 1`, hard-fatal'd against `--passes`, or are IBS/`--tree`, excluded the same way); the
   one real remaining candidate is `deep-cpu-intel`, which still hand-authors 4 separate `wspy`
   invocations that don't touch any `--passes`-incompatible flag. Note: collapsing it changes on-disk
   output shape from 4 files to 1, so anything downstream assuming those 4 filenames (external scripts,
   `tests/capability_matrix.sh`) would need checking.
5. Statistical regression harness (tolerance bands, not exact-value) + per-profile overhead guardrails —
   needs deterministic micro-workloads plus the normalized store/stats infrastructure already shipped.

**Hardware counter expansion (Intel/AMD, non-GPU):**

6. Intel counter expansion (real DRAM bandwidth via `uncore_imc_free_running_0`/`_1`, true LLC/L3 via
   `uncore_cbox_0`..`_11`, per-core-domain/iGPU RAPL energy via the `power` PMU's `energy-cores`/
   `energy-gpu`, C-state residency via `cstate_core`/`cstate_pkg`, PEBS-based precise memory-latency
   sampling as the Intel counterpart to AMD IBS sampling-mode) — see the Intel hybrid / counter-grouping
   deep-dive's "Additional Intel counters worth adding" list for the full real-hardware-grounded detail
   per counter (that list's one GPU item, `i915` GPU PMU, is tracked in 4.4(b) instead).
7. Zen5 fine-grained scheduler-stall counters (split ALU/AGU scheduler-stall counters, op-cache/
   execution-queue events) and `IBS_LD_L1_DTLB_REFILL_LAT` — see the Zen5/IBS deep-dive's remaining open
   thread for detail.
8. Platform formula registry — versioned event/formula mapping per CPU family/model, for auditability.
   Design against a real third candidate input once one of the two items above lands, rather than
   against the two AMD-only data points available today.

**Heavier/optional pieces:**

9. Config-first experiment definition system (full YAML/JSON suites/benchmarks/repetitions,
   resumable/selective re-execution) — full version of the lightweight config-file execution already in
   `wspy-run` (4.0); don't build both at once.
10. Process/thread migration diagnostics (did a process's threads actually move between cores during the
    run) — split out of 4.2's "Per-core imbalance/hot-core diagnostics" item; needs new instrumentation
    (periodic `/proc/<pid>/stat` `processor`-field sampling, or scheduler tracepoints) rather than just
    new analysis of data `--per-core` already collects.

## Deferred indefinitely
Explicitly not planned for any numbered release. Revisit only if a concrete need surfaces — don't let
these block or distract from 4.4/4.5 scoping.

- **Collector-plugin implementation** (perf stat / trace-cmd / GPU tools as collectors behind the
  `collector` field, normalization path) — the schema seam shipped in 4.0; no concrete non-wspy collector
  consumer exists to build the actual implementation against yet.
- **Optional dashboard backend** (e.g. Grafana) for exploratory slicing — static-site publishing already
  covers the real need; revisit only if that stops being enough.
- **Optional live TUI** (run progress, interval metrics, throttling/skew warnings) — the web UI already
  covers this; CLI-first model stays primary without a dedicated terminal surface.
- **General, table-driven syscall-level critical-path instrumentation** (`tree_open`'s "syscall name →
  number → decode → log-vs-aggregate" generalization) — six syscall families (futex, blocking I/O,
  connect, nanosleep, wait, poll) are still cheap enough as individual `if` branches in `ptrace_loop()`'s
  dispatch. Revisit only if a seventh syscall family comes up.
- **Low-overhead tracing alternative to `ptrace`** (`ftrace` tracepoints or minimal eBPF) for
  `--tree`/`--tree-open` — real value (`ptrace`'s stop-the-world cost skews I/O-heavy/fork-heavy
  measurements, see the Critical-path deep-dive below) but a genuine architecture change with no
  immediate forcing function. Revisit if the observer-effect problem becomes a concrete blocker on a
  specific workload rather than a standing theoretical caveat. **Already tried once, in `archive/
  wspy2.0/`:** that codebase had a selectable `--processtree-engine ftrace|ptrace|ptrace2|tracecmd`
  (`config.c`), and its `ftrace.c` engine is exactly this idea — enabled `sched_process_{fork,exec,exit}`
  tracepoints via `/sys/kernel/debug/tracing`, hand-parsed `trace_pipe` text lines. It was dropped in
  favor of `ptrace` because raw `ftrace` has no pause: nothing stops the exiting task between the kernel
  writing its trace record and the task actually being reaped, so under load the tree-builder's
  `/proc/<pid>/*` reads sometimes lost the race entirely (the process was already gone by the time the
  consumer got around to it) — worse than a staleness problem, a data-loss one, and it produced visibly
  inaccurate trees. `ptrace`'s `PTRACE_EVENT_EXIT` stop fixed this by construction: the tracee can't
  proceed to actually die until the tracer resumes it, so the read and the process's death can never
  race. This is why any future replacement of the exit-time snapshot (not the per-syscall stepping —
  see the syscall-argument-vs-timing distinction in the deep-dive) needs a synchronous in-kernel
  mechanism (e.g. a kprobe/fentry eBPF program snapshotting task state before returning), not just a
  different notification transport (`perf_event_open` tracepoints included) — swapping `ftrace`'s
  `trace_pipe` for a perf ring buffer would have made the loss less frequent, not eliminated it.
- **Optional deep trace analysis** (Perfetto-compatible export of tree+topdown+interval timelines) —
  depends entirely on the low-overhead tracing backend above; deferred alongside it. Not superseded by
  either of the two swimlane-based tree/timeline views under "Shipped since 4.3.1" above (the combined
  tree+timeline viewer, scoped to one same-invocation `--tree`+`--interval` pass, and the tree viewer's
  own `--interval`-independent "Timeline" view mode) — both are lighter, hand-rolled-SVG in-browser
  views with no export format and no dependency on replacing `ptrace`; this item is still about the
  heavier Perfetto-format export once a real low-overhead collector exists.
- **Temporal drift detection** (cluster movement across versions/configs/machines) — needs far more
  historical run accumulation across the shipped clustering work than exists today to be meaningful;
  revisit once that history actually exists.
- **AMD IBS sampling-mode: `--interval`-integrated periodic rates** — `ibs_sample.c` only drains the perf
  ring buffer once, at end-of-run; a real per-tick rate needs an actual poll-loop architectural change, a
  different scale of work than the fixed-offset decode work that already shipped. The end-of-run-only
  rate is sufficient for now.
- **Uprobe-based function-argument capture** ("ltrace-style" hooks for hot functions found via
  symbol-level profiling, e.g. recovering GEMM dimensions from a BLAS library's hottest routine) — heavy,
  niche, root-required. Revisit if a real investigation actually hits the "Pareto list of hot symbols
  isn't enough, need the real call arguments" wall this was scoped for.

## Open questions for prioritization
Each carries a recommendation; treat these as the current default, not a closed decision. (Several
earlier open questions here have been resolved by shipped work or promoted to active backlog items —
native multi-pass execution, ARM64 support, publication automation, core/thread affinity, minimum
metadata set for publishable, cross-machine comparability scoring (`env_score`/`mixed-env`, shipped 4.3),
the static-vs-interactive-backend question (resolved: static-first, see "Deferred indefinitely" above),
and the `wspy-run` declarative-profile refactor (promoted to 4.4(a) above) — rather than a stale
"resolved" note here for each.)

- **Does wspy's counter-group naming/organization need a separate Intel-focused and AMD-focused split
  (CLI flags, `--counters=` group names, web UI panels), since today's single vocabulary can feel
  AMD-centric on Intel hardware?** Raised 2026-07-22 off a real Intel Coremark run where several
  requested groups (`opcache`, `memory`) silently produced zero columns. Recommendation: no — don't fork
  the vocabulary. The Preset/Configuration/Option deep-dive (now active 4.4(a) scope) already commits to
  one vocabulary shared by the CLI/`wspy-run`/web UI specifically so none of them invents its own mental
  model, and forking by vendor would double that surface while also making `wspy-summary`/`wspy-plot`'s
  cross-run comparisons harder, since those already lean on shared column *identity* across vendors. The
  actual problem is **coverage, not naming**: `intel_raw_events[]` has zero entries for
  `COUNTER_OPCACHE`/`COUNTER_MEMORY` today, so those group names are silent no-ops on Intel with no
  warning — tracked as 4.5's Intel counter expansion item above. A follow-up worth scoping alongside that
  work: making coverage/capability reporting explicit about "not implemented on this vendor" vs.
  "requested but failed to open" — today both look identical (silently zero columns) from the CLI/web UI.

## External brainstorming references
- ReBench — reproducible experiment configuration, resumable execution, explicit benchmark
  parameter tracking: https://rebench.readthedocs.io/en/latest/
- Airspeed Velocity (asv) — static-site publication for benchmark trends with an interactive
  frontend model: https://asv.readthedocs.io/en/stable/
- Grafana OSS — optional dashboard-based slicing/templating if the interactive-backend path is
  taken: https://grafana.com/oss/grafana/
- Perfetto — timeline/trace analysis and SQL-based trace queries, relevant to the optional deep
  trace analysis pipeline (4.4): https://perfetto.dev/docs/
- OpenBenchmarking.org — public Phoronix Test Suite result archive; individual result pages expose an
  "Export Benchmark Data: Result File to Test Suite (XML)" link, the seed mechanism for the now-shipped
  "openbenchmarking.org-seeded single-test-point Phoronix suites" item (see "What shipped in 4.3"):
  https://openbenchmarking.org/

Note (2026-07-22): an earlier research pass hit an HTTP 403 fetching OpenBenchmarking.org directly from
this environment and left it unreviewed (see prior revisions of this note); a user-provided result URL
in this same conversation confirmed the export-XML link exists and is usable as a seed, so that blocker
was environment/fetch-specific, not a real access restriction — reviewed manually, not by this
environment's own fetch tooling.
