# Report directory hierarchy

Convention for where durable, curated `*.md` workload reports (and, eventually, a cross-workload
report database) live on disk. This is a **convention only** as of this writing — no tool reads the
env var below yet, and no code writes into this tree yet. It's being established now, ahead of
INVESTIGATION.md's 4.3 Tier 3 "publishing/reporting expansion" work, so that item and everything
built on top of it (the web UI creating/updating reports at the right level, the Tier 3 item 6
benchmark reference-matrix database, the Tier 3 item 5 test-point README) target the same layout
from the start instead of each inventing its own.

This hierarchy is distinct from two other things that already exist and keep their current meaning:
- **`--output-root`/`OUTROOT`** (`web/server.py`, `workload/*/run_test.sh`) — raw per-run artifacts
  (CSV, manifest, `--tree` output, `wspy-store`'s database). Unrelated and unmoved: this hierarchy is
  generated/curated *from* that data, not a replacement for where it lives.
- **This repo's own `workload/` directory** — driver *scripts* (`workload/cpu2017/run_test.sh`, etc.,
  checked into git) plus, for Phoronix, gitignored raw per-test-point artifacts materialized by
  `wspy-phoronix-import` (`workload/phoronix/<test>/<test-point>/`, see CLAUDE.md's `workload/`
  entry). **Naming heads-up:** the new report root's *default name* is also literally `workload` (see
  below) — that is intentional per the convention below, not a typo, but it means "workload" refers
  to two different directories depending on whether you're inside the wspy checkout or not. Always
  disambiguate by full path: `<wspy-checkout>/workload/` (scripts + raw Phoronix artifacts, in this
  repo) vs. `<report-root>/` (curated reports, sibling to the checkout, never in this repo).

## Root directory

- Env var: `WSPY_REPORT_ROOT` (matches this codebase's existing `WSPY_*`-prefixed env var precedent,
  e.g. `WSPY_NO_DEPRECATION_WARNINGS` in `wspy.c`, `WSPY_PTS_HOOK_LOG` in `web/joblib.py`). Not yet
  read by any tool.
- Default (when unset): a `workload` directory one level up from the wspy checkout root — e.g. a
  checkout at `/home/mev/wspy` defaults to `/home/mev/workload`. Sibling, not nested, so it survives
  independently of the wspy checkout (a fresh `git clone` of wspy elsewhere doesn't orphan it) and
  isn't subject to this repo's own `.gitignore`.
- Version control: the author's stated intent is a separate GitHub repository for this tree (not yet
  created) — distinct from wspy's own repo, so a report/README committed under `<report-root>/` is
  never a wspy commit.
- This root is also the natural future home for the Tier 3 item 6 benchmark reference-matrix
  database (a queryable, pivoted-wide table for side-by-side comparison across tests/machines) once
  that lands — one root, one place to look for both the rendered reports and the database behind
  them.

## Directory levels

1. **`<report-root>/`** — the root itself, per above.
2. **`<report-root>/<suite>/`** — one per workload suite. Names mirror this repo's own
   `workload/<suite>/` subdirectories one-to-one (`cpu2017`, `phoronix`, `cpu2026`, `pbbsbench`, ...)
   so a suite name maps identically to "where its driver script lives" and "where its reports live."
3. **`<report-root>/<suite>/<test>/`** and **`<report-root>/<suite>/<test>/<test-point>/`** —
   test-level and test-point-level. `<test>/README.md` is the general, machine-independent benchmark
   description (what the test measures) — the same content class `write_phoronix_test_readme()`
   (`web/joblib.py`) already generates today for Phoronix at `workload/phoronix/<test>/README.md`;
   generalizing that pattern to every suite and relocating it under this hierarchy is Tier 3 work, not
   done by writing this doc. `<test>/<test-point>/` is specific to one option combination (e.g.
   `coremark/default`, mirroring the `default` test-point convention already visible throughout
   today's `workload/phoronix/<test>/default/` tree; suites with no meaningful option axis just use
   `default` here too).
4. **`<report-root>/<suite>/<test>/<test-point>/<machine>/`** — one per machine/SoC class. Naming:
   `<vendor>-<short-model>`, lowercase, hyphenated — e.g. `amd-395` (Ryzen AI Max+ 395), `amd-7840`
   (Ryzen 7840U), `intel-13950` (Core i9-13950HX). `<vendor>` is one of `amd`/`intel`/`arm` (matching
   `cpu_info.c`'s own `VENDOR_*` enum naming, lowercased). `<short-model>` is the numeric model
   designation with marketing prefixes stripped — deliberately informal/human-assigned for now, not
   derived from any existing wspy vendor/model string (see "Open questions" below).

## What lives at each level

| Level | Content |
| --- | --- |
| `<report-root>/` | Future cross-workload index/database (Tier 3 item 6) — not yet built. |
| `<report-root>/<suite>/` | Suite-level overview report — not yet defined further; likely a rollup of every test under it once tooling exists. |
| `<report-root>/<suite>/<test>/README.md` | General benchmark description, machine-independent, generated (additive/don't-overwrite, same convention `write_phoronix_test_readme()` already uses, so a human's edits survive re-generation). |
| `<report-root>/<suite>/<test>/<test-point>/` | Test-point overview aggregated across every machine that ran it — cross-machine comparison at a fixed test/options combination. |
| `<report-root>/<suite>/<test>/<test-point>/<machine>/README.md` | The curated performance report for this exact test point on this exact machine, aggregated across every run in that machine's history at this test point (same many-runs aggregation `wspy-summary` already does — min/max/mean/median/stddev/outlier/CI95 — per INVESTIGATION.md's Tier 3 item 5 scoping). This is the level closest to today's per-run report page/curation studio ("What shipped in 4.1"), rolled up across a run history instead of one run. |

## Relationship to existing pieces

- `wspy-store`'s normalized SQLite database remains the query engine underneath any future report
  generation here; this hierarchy is a rendered/exported view, not a second copy of the database.
- `write_phoronix_test_readme()`'s existing output (`workload/phoronix/<test>/README.md`) predates
  this convention and lives in the "wrong" place by this scheme (inside the wspy checkout,
  Phoronix-only). Migrating it to `<report-root>/phoronix/<test>/README.md` is future Tier 3 work,
  not done by writing this doc.
- The web UI's existing publish-ready export (`web/server.py`'s markdown/HTML/WordPress export,
  "What shipped in 4.1") already produces one-run markdown reports today, just as a download rather
  than a write into a persistent tree. Wiring that (or a new aggregate-report renderer) to write into
  the appropriate level of this hierarchy is the backlog item this doc exists to unblock — see
  INVESTIGATION.md's Tier 3.

## Open questions for later automation

- **Machine-name derivation.** Is `<vendor>-<short-model>` ever machine-generated from
  `cpu_info.c`'s own vendor/family/model detection, or always human-assigned? No mapping table exists
  today from a detected model string to a short code like `395`/`7840`/`13950`.
- **Collision handling.** Two genuinely different SKUs sharing a numeric model fragment (unlikely but
  not impossible, e.g. a `U` vs. `HX` suffix on the same number) — no disambiguation rule defined yet.
- **Suite-level and root-level report content.** Levels 1-2 in the table above have no defined
  content yet beyond "future rollup" — real design work once there's more than one machine's worth of
  test-point reports to roll up.
