#!/usr/bin/env python3
"""
scripts/publish_cpu2026_benchmarks.py -- materializes doc/REPORT_HIERARCHY.md's level-3 benchmark
pages for cpu2026: one page per SPEC CPU2026 benchmark, nested under the existing "cpu2026"
WordPress page, sourced from a git-tracked <report-root>/cpu2026/<bench>/README.md.

Two-phase per benchmark ("file-system first"):
  1. Generate <report-root>/cpu2026/<bench>/README.md if it doesn't already exist -- additive, same
     "generate only if missing, human edits survive" convention write_phoronix_test_readme()
     (web/joblib.py) already established. Committed locally via report_root.commit_paths(), never
     pushed.
  2. Publish/update the corresponding WordPress draft page, nested at
     levels=[("cpu2026", "cpu2026"), (bench, bench)], via wp_client.publish_page_at_path() -- content
     is the same underlying data (description/language/doc URL), rendered directly as Gutenberg block
     markup by wp_content() below rather than through markdown_lite.to_wp_blocks(): that module
     deliberately doesn't support link syntax (built for wspy-analyze's LLM narrative output, which
     never produces links) and its italic-underscore parsing mangles a benchmark id like
     "706.stockfish_r" sitting inside a URL -- confirmed live during this script's own development.

Benchmark list/metadata comes from workload/cpu2026/spec_benchmarks.json (52 real benchmarks, cross-
checked against the local SPEC install's own benchspec/CPU/ listing -- the official docs page's own
rendering silently omitted one, 709.cactus_r; 998/999.specrand_* excluded as internal
reportable-run-validation benchmarks with no public per-benchmark docs page).

Reuses existing primitives rather than inventing new plumbing (the "standing recipe" this exists to
be): report_root.ensure_report_root()/commit_paths(), wp_client.publish_page_at_path(),
wp_client.load_config(). A future Phoronix pass should look the same shape -- a suite-specific data
file plus a driver this size, not new machinery.

Usage:
  publish_cpu2026_benchmarks.py --dry-run                  # preview everything, touches nothing
  publish_cpu2026_benchmarks.py --bench 706.stockfish_r     # just one benchmark, for review
  publish_cpu2026_benchmarks.py                             # all 52
  publish_cpu2026_benchmarks.py --publish                   # also flip each leaf page to published
"""
import argparse
import html
import json
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "web"))
import report_root  # noqa: E402
import wp_client  # noqa: E402
from wp_client import load_config  # noqa: E402

DATA_FILE = os.path.join(REPO_ROOT, "workload", "cpu2026", "spec_benchmarks.json")
SUITE_SLUG = "cpu2026"
SUITE_TITLE = "cpu2026"


def load_benchmarks():
    with open(DATA_FILE) as f:
        return json.load(f)["benchmarks"]


def resolve_report_root(args):
    if args.report_root:
        return args.report_root
    cfg = load_config()
    if cfg and cfg.get("report_root"):
        return cfg["report_root"]
    return report_root.DEFAULT_REPORT_ROOT


def readme_content(entry):
    return (
        "# %s\n\n"
        "%s. %s.\n\n"
        "See the [SPEC CPU2026 benchmark description](%s) for full details.\n"
        % (entry["bench"], entry["description"], entry["language"], entry["doc_url"])
    )


def wp_content(entry):
    """WordPress page content, built directly as Gutenberg block markup rather than routed through
    markdown_lite.to_wp_blocks(): that module's own docstring explicitly scopes out link syntax
    (built for wspy-analyze's LLM narrative output, which never produces links) -- confirmed live, its
    italic-underscore parsing mangled a benchmark id like "706.stockfish_r" sitting inside a URL. This
    content's shape is small and fixed (one heading, two paragraphs, one link), so hand-building it
    here avoids misusing a module outside what it documents supporting."""
    title = html.escape(entry["bench"])
    description = html.escape("%s. %s." % (entry["description"], entry["language"]))
    link_text = html.escape("SPEC CPU2026 benchmark description")
    url = html.escape(entry["doc_url"], quote=True)
    return (
        '<!-- wp:heading {"level":1} -->\n<h1>%s</h1>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:paragraph -->\n<p>%s</p>\n<!-- /wp:paragraph -->\n\n'
        '<!-- wp:paragraph -->\n<p>See the <a href="%s">%s</a> for full details.</p>\n'
        '<!-- /wp:paragraph -->'
        % (title, description, url, link_text)
    )


def ensure_readme(report_root_path, entry, dry_run):
    """Additive -- generate <report-root>/cpu2026/<bench>/README.md only if missing, matching
    write_phoronix_test_readme()'s convention so a human's later edits survive a re-run. Returns
    (rel_path, content, created_bool)."""
    out_dir = os.path.join(report_root_path, SUITE_SLUG, entry["bench"])
    path = os.path.join(out_dir, "README.md")
    rel_path = os.path.relpath(path, report_root_path)
    if os.path.isfile(path):
        with open(path) as f:
            return rel_path, f.read(), False
    content = readme_content(entry)
    if not dry_run:
        os.makedirs(out_dir, exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
    return rel_path, content, True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bench", action="append", default=None,
                     help="restrict to this benchmark id (repeatable); default: all 52")
    ap.add_argument("--report-root", default=None, help="override the report-root path")
    ap.add_argument("--report-root-remote", default=report_root.DEFAULT_REPORT_ROOT_REMOTE,
                     help="report-root git remote, for clone-if-missing")
    ap.add_argument("--publish", action="store_true",
                     help="flip each WP page to status=publish (default: leave as draft)")
    ap.add_argument("--dry-run", action="store_true",
                     help="print what would be written/committed/published, touch nothing")
    args = ap.parse_args()

    benchmarks = load_benchmarks()
    if args.bench:
        wanted = set(args.bench)
        benchmarks = [b for b in benchmarks if b["bench"] in wanted]
        missing = wanted - {b["bench"] for b in benchmarks}
        if missing:
            print("publish_cpu2026_benchmarks: unknown --bench value(s): %s"
                  % ", ".join(sorted(missing)), file=sys.stderr)
            return 1

    report_root_path = resolve_report_root(args)
    if not args.dry_run:
        ok, msg = report_root.ensure_report_root(report_root_path, args.report_root_remote)
        print("publish_cpu2026_benchmarks: %s" % msg, file=sys.stderr)
        if not ok:
            return 1

    wp = None
    if not args.dry_run:
        cfg = load_config()
        wp = (cfg or {}).get("wordpress")
        if not wp:
            print("publish_cpu2026_benchmarks: no WordPress config -- run `wspy-publish configure` "
                  "first", file=sys.stderr)
            return 1

    # Phase 1: file-system README generation + a single commit covering every new file.
    rows = []  # (entry, rel_path, content, created_bool)
    new_rel_paths = []
    for entry in benchmarks:
        rel_path, content, created = ensure_readme(report_root_path, entry, args.dry_run)
        rows.append((entry, rel_path, content, created))
        print("  README %s: %s" % (entry["bench"], "created" if created else "exists"))
        if created:
            new_rel_paths.append(rel_path)

    if new_rel_paths:
        message = "Add cpu2026 benchmark README(s): %s" % ", ".join(
            entry["bench"] for entry, _, _, created in rows if created)
        if args.dry_run:
            print("publish_cpu2026_benchmarks: --dry-run -- would commit %d new README(s)"
                  % len(new_rel_paths), file=sys.stderr)
        else:
            ok, msg = report_root.commit_paths(report_root_path, new_rel_paths, message)
            print("publish_cpu2026_benchmarks: %s" % msg, file=sys.stderr)
            if not ok:
                return 1

    # Phase 2: WordPress draft pages, content built from the same underlying benchmark data.
    failures = 0
    for entry, _, _, _ in rows:
        levels = [(SUITE_SLUG, SUITE_TITLE), (entry["bench"], entry["bench"])]
        blocks = wp_content(entry)
        if args.dry_run:
            print("  WP %s: would publish_page_at_path(levels=%r, content=%d bytes, publish=%s)"
                  % (entry["bench"], levels, len(blocks), args.publish))
            continue
        try:
            page, created = wp_client.publish_page_at_path(
                wp["site_url"], wp["username"], wp["app_password"], levels, blocks,
                do_publish=args.publish)
        except wp_client.WPError as e:
            print("  WP %s: FAILED (status=%s code=%s): %s" % (entry["bench"], e.status, e.code, e),
                  file=sys.stderr)
            failures += 1
            continue
        print("  WP %s: %s (id=%s, status=%s) link=%s"
              % (entry["bench"], "created" if created else "found/updated", page["id"],
                 page.get("status"), page.get("link")))

    if failures:
        print("publish_cpu2026_benchmarks: %d page(s) failed" % failures, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
