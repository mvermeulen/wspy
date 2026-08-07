#!/usr/bin/env python3
"""
scripts/publish_reference_matrix.py -- INVESTIGATION.md 4.3 Tier 3 item 2's site-wide publishing
pipeline. Materializes the benchmark reference-matrix database (item 5) as real content on
mvermeulen.org/workload, replacing today's empty auto-created suite-level stub pages.

Fetching the real /perf/workloads/<suite>/ pages during scoping showed their actual shape: rows=test
points, columns=metrics, one machine per page -- not a cross-machine table. That's exactly
render_reference_by_machine()'s shape (web/server.py, PR #212), so this script is mostly publishing
plumbing over data that already exists, not new analysis. Three page kinds:

  1. Per-(suite, machine) wide table at <suite>/by-machine-<machine>/ -- the real numeric content.
     Sourced from joblib.aggregate_reference_matrix_cell() for machines with local wspy-store
     presence, merged with server.recover_machine_metrics_from_wordpress() (item 21) for machines
     that are WordPress-only -- discovered via server.discover_wordpress_matrix_rows() (item 22), not
     just item 21's per-row gap-filling, since a WordPress-only machine has no local runs.json at all
     for enumerate_reference_matrix_cells() to find in the first place. A machine with local data for
     some test points and WordPress-only data for others gets both merged into one page -- local
     always wins per test point when both exist, same "real data over recovered" precedent
     render_reference_test_point_detail() already established. WordPress-recovered rows carry no
     wspy-summary reliability verdict, marked with `*` rather than silently blended in as equally
     trustworthy.
  2. Per-suite index at <suite>/ (today an empty auto-created stub) -- one row per test point with
     per-machine coverage (local run count, or "WordPress only"), plus links to every machine's own
     wide-table page from (1).
  3. Root rollup, listing every suite x machine page (1) actually exists for -- sourced from scanning
     what this script has already generated into the report-root (<suite>/by-machine/*.md), not a
     fresh computation, so it stays accurate regardless of which --suite/--machine this particular
     invocation touched. Same "always freshly regenerated from what's actually there" precedent
     publish_machine_page.py's own catalog index already uses.

Imports server.py directly for discover_wordpress_matrix_rows()/recover_machine_metrics_from_wordpress()
-- same precedent wspy-testpoint already set (`import server as web_export`) for reusing server.py's
functions from a separate CLI tool, rather than duplicating them.

Can't reuse render_reference_by_machine()'s own HTML (local-only /reference/ links, CSS classes
WordPress doesn't have) -- generates its own report-root markdown + WP Gutenberg block markup from the
same underlying row data instead, the same two-representations-from-one-source-of-truth precedent
publish_cpu2026_benchmarks.py/publish_machine_page.py already use.

WordPress discovery (item 22) is a real crawl cost (~50s/suite, confirmed live during that item's own
development) -- included by default, since finding WordPress-only machines is this script's whole
point, but skippable via --skip-wordpress-discovery for a fast local-only run.

Confirmed with the author (2026-08-07): generated-only, no hand-maintained content on the new site to
protect (the suite-level pages are empty stubs today). The old mvermeulen.org/perf/workloads/ site is
untouched by this script -- retiring its hand-maintenance in favor of this pipeline, once the new site
has real authority, is a deliberate future follow-on, not done here.

Usage:
  publish_reference_matrix.py --dry-run                       # preview everything, touches nothing
  publish_reference_matrix.py --suite cpu2026                  # just one suite
  publish_reference_matrix.py --suite cpu2026 --machine amd-370-64gb  # just one page, for review
  publish_reference_matrix.py                                  # every suite, every machine
  publish_reference_matrix.py --publish                        # also flip each leaf page to published
  publish_reference_matrix.py --skip-wordpress-discovery       # local wspy-store machines only, fast
"""
import argparse
import html
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "web"))
import joblib  # noqa: E402
import report_root  # noqa: E402
import wp_client  # noqa: E402
from wp_client import load_config  # noqa: E402
import server as web_export  # noqa: E402

SUITES = web_export.REFERENCE_MATRIX_SUITES
ROOT_SLUG = "reference-matrix"
ROOT_TITLE = "Reference matrix"


def resolve_report_root(args):
    if args.report_root:
        return args.report_root
    cfg = load_config()
    if cfg and cfg.get("report_root"):
        return cfg["report_root"]
    return report_root.DEFAULT_REPORT_ROOT


def local_cells_for_suite(report_root_path, suite):
    all_cells = joblib.enumerate_reference_matrix_cells(
        report_root_path, web_export.PHORONIX_DEST_ROOT, web_export.CPU2026_DEST_ROOT)
    return [c for c in all_cells if c["suite"] == suite]


def wordpress_rows_for_suite(wp_cfg, suite, skip):
    if skip or not wp_cfg:
        return []
    print("  crawling WordPress for %s (this can take a while)..." % suite, file=sys.stderr)
    return web_export.discover_wordpress_matrix_rows(
        wp_cfg, suite, progress=lambda msg: print("    %s" % msg, file=sys.stderr))


def build_machine_rows(agg_cfg, wp_cfg, suite, machine, local_cells, wp_rows):
    """Returns a sorted list of (test, test_point, rows, source) for one machine -- rows is
    aggregate_reference_matrix_cell()'s shape for source="local", or
    recover_machine_metrics_from_wordpress()'s shape for source="wordpress". Local always wins per
    test point when both a local cell and a WordPress-only row exist for the same (test, test_point)."""
    local_by_tp = {(c["test"], c["test_point"]): c for c in local_cells if c["machine"] == machine}
    wp_tps = {(r["test"], r["test_point"]) for r in wp_rows if r["machine"] == machine}

    entries = []
    for (test, test_point), c in local_by_tp.items():
        rows = joblib.aggregate_reference_matrix_cell(
            agg_cfg["wspy_testpoint_bin"], agg_cfg["db"], suite, c["benchmark"], machine,
            report_root_path=agg_cfg["report_root_path"])
        if rows:
            entries.append((test, test_point, rows, "local"))
    for test, test_point in wp_tps - set(local_by_tp):
        rows = web_export.recover_machine_metrics_from_wordpress(wp_cfg, suite, test, test_point, machine)
        if rows:
            entries.append((test, test_point, rows, "wordpress"))
    return sorted(entries, key=lambda e: (e[0], e[1]))


def cell_text(rows, metric, source):
    for r in rows:
        if r["metric"] == metric:
            mean, n = r.get("mean", ""), r.get("n", "")
            marker = ""
            if source == "wordpress":
                marker = "*"
            elif r.get("verdict") and r["verdict"] != "PASS":
                marker = "†"
            return "%s (n=%s)%s" % (mean, n, marker)
    return "—"


def build_machine_page(suite, machine, entries):
    """Returns (markdown, wp_html) for one machine's wide table across every test point in `suite`
    it has data for, or (None, None) if there's nothing to show. Cell text mirrors
    render_reference_by_machine()'s own "mean (n=N)" shape; `*` marks a WordPress-recovered cell (no
    reliability verdict behind it), `†` marks a non-PASS wspy-summary verdict on a real local
    cell -- footnotes only appear when at least one cell actually uses that marker."""
    metrics = sorted({r["metric"] for _, _, rows, _ in entries for r in rows})
    if not metrics:
        return None, None

    has_wp = any(source == "wordpress" for _, _, _, source in entries)
    has_warn = any(r.get("verdict") not in (None, "PASS")
                   for _, _, rows, source in entries if source == "local" for r in rows)

    md_lines = ["# %s -- %s" % (machine, suite), "",
                "| test point | " + " | ".join(metrics) + " |",
                "|---|" + "|".join("---" for _ in metrics) + "|"]
    body_rows_html = []
    for test, test_point, rows, source in entries:
        label = "%s / %s" % (test, test_point)
        cells = [cell_text(rows, m, source) for m in metrics]
        md_lines.append("| %s | %s |" % (label, " | ".join(cells)))
        cells_html = "".join("<td>%s</td>" % html.escape(c) for c in cells)
        body_rows_html.append("<tr><td>%s</td>%s</tr>" % (html.escape(label), cells_html))

    md_lines.append("")
    if has_wp:
        md_lines.append("`*` -- recovered from an already-published WordPress page, not this "
                         "machine's local wspy-store; no reliability verdict behind it.")
    if has_warn:
        md_lines.append("`†` -- carries a non-PASS wspy-summary verdict "
                         "(thin/noisy/mixed-pmu/mixed-env).")
    markdown = "\n".join(md_lines) + "\n"

    footnote_html = ""
    if has_wp:
        footnote_html += (
            '<!-- wp:paragraph --><p><em>`*` -- recovered from an already-published WordPress page, '
            "not this machine's local wspy-store; no reliability verdict behind it.</em></p>"
            "<!-- /wp:paragraph -->\n\n")
    if has_warn:
        footnote_html += (
            '<!-- wp:paragraph --><p><em>`†` -- carries a non-PASS wspy-summary verdict '
            "(thin/noisy/mixed-pmu/mixed-env).</em></p><!-- /wp:paragraph -->\n\n")

    header_html = "".join("<th>%s</th>" % html.escape(m) for m in metrics)
    wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>%s -- %s</h1>\n<!-- /wp:heading -->\n\n'
        '%s'
        '<!-- wp:table --><figure class="wp-block-table"><table><thead><tr>'
        '<th>test point</th>%s</tr></thead><tbody>%s</tbody></table></figure><!-- /wp:table -->'
        % (html.escape(machine), html.escape(suite), footnote_html, header_html,
           "".join(body_rows_html))
    )
    return markdown, wp_html


def build_suite_index(report_root_path, suite, local_cells, wp_rows, by_machine_slugs):
    """(markdown, wp_html) for the suite-level index page -- one row per (test, test_point) covering
    either a local cell or a WordPress-only discovery hit, plus links to every by-machine wide-table
    page this run knows about for this suite."""
    rows_by_tp = {}
    for c in local_cells:
        runs_json_path = os.path.join(report_root_path, suite, c["test"], c["test_point"],
                                       c["machine"], "runs.json")
        n = joblib.count_stats_pool_runs(runs_json_path)
        rows_by_tp.setdefault((c["test"], c["test_point"]), {})[c["machine"]] = "%d run(s)" % n
    for r in wp_rows:
        key = (r["test"], r["test_point"])
        rows_by_tp.setdefault(key, {}).setdefault(r["machine"], "WordPress only")

    md_lines = ["# %s -- reference matrix" % suite, ""]
    tp_rows_html = []
    if rows_by_tp:
        md_lines += ["| test | test point | machines |", "|---|---|---|"]
        for (test, test_point), by_machine in sorted(rows_by_tp.items()):
            machines_text = ", ".join("%s (%s)" % (m, s) for m, s in sorted(by_machine.items()))
            md_lines.append("| %s | %s | %s |" % (test, test_point, machines_text))
            tp_rows_html.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>" % (
                html.escape(test), html.escape(test_point), html.escape(machines_text)))
    else:
        md_lines.append("No test points yet.")

    md_lines.append("")
    md_lines.append("## By machine")
    for machine in sorted(by_machine_slugs):
        md_lines.append("- [%s](by-machine-%s/)" % (machine, machine))
    markdown = "\n".join(md_lines) + "\n"

    tp_table_html = (
        '<!-- wp:table --><figure class="wp-block-table"><table><thead><tr>'
        '<th>test</th><th>test point</th><th>machines</th></tr></thead><tbody>%s</tbody>'
        '</table></figure><!-- /wp:table -->\n\n' % "".join(tp_rows_html)
        if tp_rows_html else
        '<!-- wp:paragraph --><p>No test points yet.</p><!-- /wp:paragraph -->\n\n'
    )
    by_machine_items_html = "".join(
        '<!-- wp:list-item --><li><a href="%s/">%s</a></li><!-- /wp:list-item -->\n'
        % (html.escape("by-machine-" + m), html.escape(m))
        for m in sorted(by_machine_slugs)
    )
    wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>%s -- reference matrix</h1>\n<!-- /wp:heading -->\n\n'
        '%s'
        '<!-- wp:heading {"level":2} -->\n<h2>By machine</h2>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:list --><ul>%s</ul><!-- /wp:list -->'
        % (html.escape(suite), tp_table_html, by_machine_items_html)
    )
    return markdown, wp_html


def scan_existing_by_machine_pages(report_root_path):
    """{suite: [machine, ...]} from <report-root>/<suite>/by-machine/*.md already generated by this
    script (any prior run, or this one) -- not a fresh computation, so the root rollup always
    reflects what's actually been published regardless of which --suite/--machine this particular
    invocation touched."""
    result = {}
    for suite in SUITES:
        by_machine_dir = os.path.join(report_root_path, suite, "by-machine")
        if not os.path.isdir(by_machine_dir):
            continue
        machines = sorted(fn[:-3] for fn in os.listdir(by_machine_dir) if fn.endswith(".md"))
        if machines:
            result[suite] = machines
    return result


def build_root_rollup(by_suite):
    md_lines = ["# Reference matrix", ""]
    for suite in sorted(by_suite):
        md_lines.append("## %s" % suite)
        for machine in by_suite[suite]:
            md_lines.append("- [%s](%s/by-machine-%s/)" % (machine, suite, machine))
        md_lines.append("")
    markdown = "\n".join(md_lines) + "\n"

    sections_html = []
    for suite in sorted(by_suite):
        items_html = "".join(
            '<!-- wp:list-item --><li><a href="/%s/by-machine-%s/">%s</a></li><!-- /wp:list-item -->\n'
            % (html.escape(suite), html.escape(m), html.escape(m))
            for m in by_suite[suite]
        )
        sections_html.append(
            '<!-- wp:heading {"level":2} -->\n<h2>%s</h2>\n<!-- /wp:heading -->\n\n'
            '<!-- wp:list --><ul>%s</ul><!-- /wp:list -->\n\n' % (html.escape(suite), items_html)
        )
    wp_html = ('<!-- wp:heading {"level":1} -->\n<h1>Reference matrix</h1>\n<!-- /wp:heading -->\n\n'
               + "".join(sections_html))
    return markdown, wp_html


def write_generated_file(report_root_path, rel_path, content, dry_run, new_rel_paths):
    path = os.path.join(report_root_path, rel_path)
    if not dry_run:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
    new_rel_paths.append(rel_path)


def publish_leaf(wp, levels, content, args, label):
    if args.dry_run:
        print("  WP %s: would publish_page_at_path(levels=%r, content=%d bytes, publish=%s)"
              % (label, levels, len(content), args.publish))
        return True
    try:
        page, created = wp_client.publish_page_at_path(
            wp["site_url"], wp["username"], wp["app_password"], levels, content,
            do_publish=args.publish, force=args.force)
    except wp_client.WPContentDriftError as e:
        print("  WP %s: FAILED (drift) -- %s: %s" % (label, e, e.link), file=sys.stderr)
        return False
    except wp_client.WPError as e:
        print("  WP %s: FAILED (status=%s code=%s): %s" % (label, e.status, e.code, e),
              file=sys.stderr)
        return False
    print("  WP %s: %s (id=%s, status=%s) link=%s"
          % (label, "created" if created else "found/updated", page["id"], page.get("status"),
             page.get("link")))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--suite", action="append", default=None, choices=list(SUITES),
                     help="restrict to this suite (repeatable); default: all of %s" % (SUITES,))
    ap.add_argument("--machine", action="append", default=None,
                     help="restrict to this machine slug (repeatable); default: every machine found")
    ap.add_argument("--skip-wordpress-discovery", action="store_true",
                     help="skip the WordPress crawl (item 22) -- local wspy-store machines only, "
                          "much faster")
    ap.add_argument("--report-root", default=None, help="override the report-root path")
    ap.add_argument("--report-root-remote", default=report_root.DEFAULT_REPORT_ROOT_REMOTE,
                     help="report-root git remote, for clone-if-missing")
    ap.add_argument("--db", default=os.path.join(REPO_ROOT, "web", "runs", "store.db"),
                     help="normalized wspy-store database (default: %(default)s)")
    ap.add_argument("--wspy-testpoint-bin", default=os.path.join(REPO_ROOT, "wspy-testpoint"),
                     help="wspy-testpoint binary (default: %(default)s)")
    ap.add_argument("--publish", action="store_true",
                     help="flip each WP page to status=publish (default: leave as draft)")
    ap.add_argument("--force", action="store_true",
                     help="overwrite a leaf page even if its live content has drifted since wspy "
                          "last published it (default: refuse, since that usually means a hand-edit)")
    ap.add_argument("--dry-run", action="store_true",
                     help="print what would be written/committed/published, touch nothing")
    args = ap.parse_args()

    suites = args.suite or list(SUITES)
    machine_filter = set(args.machine) if args.machine else None

    report_root_path = resolve_report_root(args)
    if not args.dry_run:
        ok, msg = report_root.ensure_report_root(report_root_path, args.report_root_remote)
        print("publish_reference_matrix: %s" % msg, file=sys.stderr)
        if not ok:
            return 1

    wp = None
    wp_cfg = load_config()
    if not args.dry_run:
        wp = (wp_cfg or {}).get("wordpress")
        if not wp:
            print("publish_reference_matrix: no WordPress config -- run `wspy-publish configure` "
                  "first", file=sys.stderr)
            return 1

    # --dry-run must touch nothing, including the network -- WordPress discovery (item 22) is
    # read-only but still a real, slow (~50s/suite) live crawl, not something a "preview only" flag
    # should silently trigger. skip_wordpress_discovery alone still governs a real (non-dry-run) run.
    skip_discovery = args.skip_wordpress_discovery or args.dry_run
    if args.dry_run and not args.skip_wordpress_discovery:
        print("publish_reference_matrix: --dry-run also skips WordPress discovery (a real network "
              "crawl, not just a preview) -- pass --skip-wordpress-discovery explicitly if you want "
              "that noted, or drop --dry-run to actually crawl", file=sys.stderr)

    agg_cfg = {"wspy_testpoint_bin": args.wspy_testpoint_bin, "db": args.db,
               "report_root_path": report_root_path}

    new_rel_paths = []
    failures = 0
    touched_by_suite = {}

    for suite in suites:
        print("suite %s:" % suite, file=sys.stderr)
        local_cells = local_cells_for_suite(report_root_path, suite)
        wp_rows = wordpress_rows_for_suite(wp_cfg, suite, skip_discovery)

        machines = {c["machine"] for c in local_cells} | {r["machine"] for r in wp_rows}
        if machine_filter:
            machines &= machine_filter
        machines = sorted(machines)

        suite_machine_pages = []
        for machine in machines:
            entries = build_machine_rows(agg_cfg, wp_cfg, suite, machine, local_cells, wp_rows)
            markdown, wp_html = build_machine_page(suite, machine, entries)
            if markdown is None:
                print("  %s/%s: no data, skipping" % (suite, machine), file=sys.stderr)
                continue
            rel_path = os.path.join(suite, "by-machine", machine + ".md")
            write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
            print("  %s: wrote %s" % (machine, rel_path))
            levels = [(suite, suite), ("by-machine-" + machine, machine)]
            if not publish_leaf(wp, levels, wp_html, args, "%s/%s" % (suite, machine)):
                failures += 1
            else:
                suite_machine_pages.append(machine)

        if not machine_filter:
            # The suite index links every by-machine page this run touched -- only meaningful once
            # every machine in the suite has been considered, not a --machine-filtered subset.
            markdown, wp_html = build_suite_index(
                report_root_path, suite, local_cells, wp_rows, suite_machine_pages)
            rel_path = os.path.join(suite, "README.md")
            write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
            print("  suite index: wrote %s" % rel_path)
            if not publish_leaf(wp, [(suite, suite)], wp_html, args, suite):
                failures += 1

        if suite_machine_pages:
            touched_by_suite[suite] = suite_machine_pages

    if not args.suite and not machine_filter:
        # Merge in what this run itself just (or would have) generated -- under --dry-run,
        # write_generated_file() never actually writes, so a fresh report-root's disk scan alone
        # would under-report what this same invocation is previewing.
        by_suite = scan_existing_by_machine_pages(report_root_path)
        for suite, machines in touched_by_suite.items():
            by_suite[suite] = sorted(set(by_suite.get(suite, [])) | set(machines))
        markdown, wp_html = build_root_rollup(by_suite)
        rel_path = "README.md"
        write_generated_file(report_root_path, rel_path, markdown, args.dry_run, new_rel_paths)
        print("root rollup: wrote %s" % rel_path, file=sys.stderr)
        if not publish_leaf(wp, [(ROOT_SLUG, ROOT_TITLE)], wp_html, args, "root rollup"):
            failures += 1

    if new_rel_paths:
        message = "Update reference-matrix pages: %s" % ", ".join(sorted(set(new_rel_paths)))
        if args.dry_run:
            print("publish_reference_matrix: --dry-run -- would commit %d file(s)"
                  % len(new_rel_paths), file=sys.stderr)
        else:
            ok, msg = report_root.commit_paths(report_root_path, new_rel_paths, message)
            print("publish_reference_matrix: %s" % msg, file=sys.stderr)
            if not ok:
                return 1

    if failures:
        print("publish_reference_matrix: %d page(s) failed" % failures, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
