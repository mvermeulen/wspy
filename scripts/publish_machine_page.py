#!/usr/bin/env python3
"""
scripts/publish_machine_page.py -- publishes this physical machine's entry into the
doc/REPORT_HIERARCHY.md machine catalog: a /machine/<short-name>/ detail page (this machine's real
hardware profile, from scripts/map_cpu_hierarchy.py) plus a regenerated /machine/ index page listing
every machine registered so far, both on the file-system report-root and the live WordPress site.

Run locally, once per physical machine -- map_cpu_hierarchy.py's hardware detection is inherently
local, it can only describe the machine it's actually run on. Contributes one
<report-root>/machine/<short-name>/ entry to the shared, git-tracked report-root, which is what lets
the /machine/ index accumulate entries across multiple physical machines over time as each one runs
this script.

Naming: <vendor>-<short-model>-<ram-gib>gb (e.g. amd-395-64gb), confirmed with the author as the
starting disambiguation scheme for two machines sharing a chip but differing in memory --
doc/REPORT_HIERARCHY.md's own previously-unresolved "collision handling" question. vendor/short_model
are parsed back out of the given --short-name itself rather than re-derived from a marketing string
(deliberately informal/human-assigned, matching REPORT_HIERARCHY.md's own stance) -- ram_gib comes
from map_cpu_hierarchy.py's own get_total_system_memory(), imported directly.

Unlike cpu2026/Phoronix's benchmark READMEs, the detail page's content is always regenerated (not
additive-once): it's a factual hardware snapshot with no room for human commentary in this first cut,
and hardware can genuinely change (a RAM upgrade). The index page is always regenerated too, since it
must reflect the full current set of registered machines, not just whichever existed when first
created.

Collision safety: if <report-root>/machine/<short-name>/machine.json already exists with a DIFFERENT
vendor/short_model/ram_gib than what's freshly detected, refuses rather than silently overwriting --
protects against two different physical machines accidentally landing on the same computed name.
--force overrides.

Each sidecar also records this script's own `socket.gethostname()` (refreshed on every run, not a
collision field -- a rename shouldn't block re-registering the same physical hardware). This is what
lets INVESTIGATION.md 4.3 Tier 3 item 5's reference-matrix database resolve a wspy-store run's own
recorded hostname back to a `<short-name>` for display, via `web/machine_registry.py`'s read-only
scan of this catalog -- the intended single place a hostname/short-name association is ever written,
so nothing else in the tree maintains a second, parallel copy of it.

Usage:
  publish_machine_page.py --short-name amd-370-64gb --dry-run   # preview, touches nothing
  publish_machine_page.py --short-name amd-370-64gb              # real run, this machine only
  publish_machine_page.py --short-name amd-370-64gb --publish    # also flip both pages to published
"""
import argparse
import html
import json
import os
import socket
import subprocess
import sys
from datetime import datetime, timezone

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
sys.path.insert(0, os.path.join(REPO_ROOT, "web"))
sys.path.insert(0, SCRIPT_DIR)
import report_root  # noqa: E402
import wp_client  # noqa: E402
from wp_client import load_config, save_config  # noqa: E402
from map_cpu_hierarchy import get_total_system_memory  # noqa: E402

CATALOG_SLUG = "machine"
CATALOG_TITLE = "machine"
MAP_CPU_HIERARCHY_BIN = os.path.join(SCRIPT_DIR, "map_cpu_hierarchy.py")


def resolve_report_root(args):
    if args.report_root:
        return args.report_root
    cfg = load_config()
    if cfg and cfg.get("report_root"):
        return cfg["report_root"]
    return report_root.DEFAULT_REPORT_ROOT


def parse_short_name(short_name):
    """(vendor, short_model) parsed from the first two '-'-separated segments of short_name -- e.g.
    "amd-370-64gb" -> ("amd", "370"). Deliberately not re-derived from a marketing string; the human
    already gave us the identity when choosing the name."""
    parts = short_name.split("-")
    if len(parts) < 2:
        raise ValueError('--short-name must look like "<vendor>-<short-model>[-<suffix>]", '
                          'e.g. "amd-395-64gb"')
    return parts[0], parts[1]


def round_ram_gib(total_bytes):
    """Bytes -> a rounded-to-nearest-integer GiB figure for the sidecar/index (real system memory
    always reads a little under its nominal SKU size, e.g. 60.9 GiB on a 64 GiB machine -- round to
    the nearest whole GiB rather than trying to snap to a fixed SKU-size list, keeping this simple)."""
    return round(total_bytes / (1024 ** 3))


def run_map_cpu_hierarchy():
    """The full default text report -- topology tree, core table, cache/memory/GPU summary -- via
    subprocess (matches this codebase's established pattern for shelling out to an existing tool
    rather than refactoring it into a string-returning library function, e.g.
    write_phoronix_test_readme()'s own phoronix-test-suite info subprocess call)."""
    result = subprocess.run([sys.executable, MAP_CPU_HIERARCHY_BIN], capture_output=True, text=True)
    return result.stdout


def check_collision(report_root_path, short_name, vendor, short_model, ram_gib, force):
    """Returns None if safe to proceed, or an error message string if an existing sidecar under this
    short name describes a genuinely different machine and --force wasn't given."""
    sidecar_path = os.path.join(report_root_path, CATALOG_SLUG, short_name, "machine.json")
    if not os.path.isfile(sidecar_path):
        return None
    with open(sidecar_path) as f:
        existing = json.load(f)
    mismatch = [f"{field}: existing={existing.get(field)!r} vs detected={detected!r}"
                for field, detected in (("vendor", vendor), ("short_model", short_model),
                                         ("ram_gib", ram_gib))
                if existing.get(field) != detected]
    if mismatch and not force:
        return ("--short-name %r already registered for a different machine (%s) -- pick a "
                "different --short-name, or pass --force if this really is the same physical "
                "machine (e.g. after a RAM upgrade)" % (short_name, "; ".join(mismatch)))
    return None


def build_index_content(report_root_path):
    """Scans every <report-root>/machine/*/machine.json sidecar and returns (readme_markdown,
    wp_html) for the /machine/ index page -- always freshly regenerated so it reflects every
    currently-registered machine, not just whichever existed when this page was first created."""
    catalog_dir = os.path.join(report_root_path, CATALOG_SLUG)
    entries = []
    if os.path.isdir(catalog_dir):
        for name in sorted(os.listdir(catalog_dir)):
            sidecar_path = os.path.join(catalog_dir, name, "machine.json")
            if os.path.isfile(sidecar_path):
                with open(sidecar_path) as f:
                    entries.append(json.load(f))

    md_lines = ["# Machine catalog", "",
                "| Short name | Vendor | Model | RAM | Cores |", "|---|---|---|---|---|"]
    for e in entries:
        md_lines.append("| [%s](%s/) | %s | %s | %s GiB | %s |" % (
            e["short_name"], e["short_name"], e["vendor"], e["short_model"], e["ram_gib"],
            e["core_summary"]))
    readme_md = "\n".join(md_lines) + "\n"

    rows_html = "".join(
        '<tr><td><a href="%s/">%s</a></td><td>%s</td><td>%s</td><td>%s GiB</td><td>%s</td></tr>' % (
            html.escape(e["short_name"]), html.escape(e["short_name"]), html.escape(e["vendor"]),
            html.escape(e["short_model"]), e["ram_gib"], html.escape(e["core_summary"]))
        for e in entries
    )
    wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>Machine catalog</h1>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:table -->\n<figure class="wp-block-table"><table><thead><tr>'
        '<th>Short name</th><th>Vendor</th><th>Model</th><th>RAM</th><th>Cores</th></tr></thead>'
        '<tbody>%s</tbody></table></figure>\n<!-- /wp:table -->' % rows_html
    )
    return readme_md, wp_html


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--short-name", default=None,
                     help='this machine\'s catalog short name, e.g. "amd-395-64gb" -- defaults to '
                          '~/.config/wspy/publish.json\'s "machine_short_name" once this machine has '
                          'been registered once; required the first time')
    ap.add_argument("--core-summary", default=None,
                     help="override the auto-derived core-count summary shown in the index table")
    ap.add_argument("--report-root", default=None, help="override the report-root path")
    ap.add_argument("--report-root-remote", default=report_root.DEFAULT_REPORT_ROOT_REMOTE,
                     help="report-root git remote, for clone-if-missing")
    ap.add_argument("--force", action="store_true",
                     help="proceed even if an existing sidecar for this --short-name looks like a "
                          "different machine")
    ap.add_argument("--publish", action="store_true",
                     help="flip both pages to status=publish (default: leave as drafts)")
    ap.add_argument("--dry-run", action="store_true",
                     help="print what would be written/committed/published, touch nothing")
    args = ap.parse_args()

    cfg = load_config() or {}
    args.short_name = args.short_name or cfg.get("machine_short_name")
    if not args.short_name:
        print("publish_machine_page: --short-name is required the first time this machine is "
              "registered (no machine_short_name found in %s)" % wp_client.CONFIG_PATH, file=sys.stderr)
        return 1

    try:
        vendor, short_model = parse_short_name(args.short_name)
    except ValueError as e:
        print("publish_machine_page: %s" % e, file=sys.stderr)
        return 1

    ram_gib = round_ram_gib(get_total_system_memory())
    core_summary = args.core_summary or ("%dT" % os.cpu_count())
    report_text = run_map_cpu_hierarchy()

    report_root_path = resolve_report_root(args)
    collision_error = check_collision(report_root_path, args.short_name, vendor, short_model, ram_gib,
                                       args.force)
    if collision_error:
        print("publish_machine_page: %s" % collision_error, file=sys.stderr)
        return 1

    if not args.dry_run:
        ok, msg = report_root.ensure_report_root(report_root_path, args.report_root_remote)
        print("publish_machine_page: %s" % msg, file=sys.stderr)
        if not ok:
            return 1

    machine_dir = os.path.join(report_root_path, CATALOG_SLUG, args.short_name)
    sidecar_path = os.path.join(machine_dir, "machine.json")
    readme_path = os.path.join(machine_dir, "README.md")
    index_path = os.path.join(report_root_path, CATALOG_SLUG, "README.md")

    sidecar = {
        "short_name": args.short_name, "vendor": vendor, "short_model": short_model,
        "ram_gib": ram_gib, "core_summary": core_summary, "hostname": socket.gethostname(),
        # hostname is this script's own machine, always refreshed on every run (not a collision
        # field in check_collision() -- a rename shouldn't block re-registering the same physical
        # hardware) -- it's what lets INVESTIGATION.md 4.3 Tier 3 item 5's reference-matrix database
        # resolve a wspy-store run's own recorded hostname back to this catalog's short_name.
        "generated_at": datetime.now(timezone.utc).isoformat(),
    }
    detail_md = "# %s\n\n```\n%s\n```\n" % (args.short_name, report_text.rstrip())

    if not args.dry_run:
        os.makedirs(machine_dir, exist_ok=True)
        with open(sidecar_path, "w") as f:
            json.dump(sidecar, f, indent=2)
            f.write("\n")
        with open(readme_path, "w") as f:
            f.write(detail_md)
        # Regenerate the index AFTER writing this machine's own sidecar, so it's included.
        index_md, _ = build_index_content(report_root_path)
        with open(index_path, "w") as f:
            f.write(index_md)
        print("  wrote %s" % sidecar_path)
        print("  wrote %s" % readme_path)
        print("  regenerated %s" % index_path)
    else:
        index_md, _ = build_index_content(report_root_path)
        print("  would write %s:\n%s" % (sidecar_path, json.dumps(sidecar, indent=2)))
        print("  would write %s (%d bytes)" % (readme_path, len(detail_md)))
        print("  would regenerate %s:\n%s" % (index_path, index_md))

    rel_paths = [os.path.relpath(p, report_root_path) for p in (sidecar_path, readme_path, index_path)]
    if args.dry_run:
        print("publish_machine_page: --dry-run -- would commit %s" % ", ".join(rel_paths),
              file=sys.stderr)
        print("  WP: would publish detail + index pages (publish=%s)" % args.publish)
        return 0

    ok, msg = report_root.commit_paths(
        report_root_path, rel_paths, "Add/update machine catalog entry: %s" % args.short_name)
    print("publish_machine_page: %s" % msg, file=sys.stderr)
    if not ok:
        return 1

    wp = cfg.get("wordpress")
    if not wp:
        print("publish_machine_page: no WordPress config -- run `wspy-publish configure` first",
              file=sys.stderr)
        return 1

    detail_wp_html = (
        '<!-- wp:heading {"level":1} -->\n<h1>%s</h1>\n<!-- /wp:heading -->\n\n'
        '<!-- wp:preformatted -->\n<pre class="wp-block-preformatted">%s</pre>\n'
        '<!-- /wp:preformatted -->'
        % (html.escape(args.short_name), html.escape(report_text))
    )
    try:
        page, created = wp_client.publish_page_at_path(
            wp["site_url"], wp["username"], wp["app_password"],
            [(CATALOG_SLUG, CATALOG_TITLE), (args.short_name, args.short_name)], detail_wp_html,
            do_publish=args.publish)
    except wp_client.WPError as e:
        print("publish_machine_page: detail page FAILED (status=%s code=%s): %s"
              % (e.status, e.code, e), file=sys.stderr)
        return 1
    print("  WP detail: %s (id=%s, status=%s) link=%s"
          % ("created" if created else "found/updated", page["id"], page.get("status"), page["link"]))

    _, index_wp_html = build_index_content(report_root_path)
    try:
        index_page, index_created = wp_client.publish_page_content(
            wp["site_url"], wp["username"], wp["app_password"], CATALOG_SLUG, 0, CATALOG_TITLE,
            index_wp_html, do_publish=args.publish)
    except wp_client.WPError as e:
        print("publish_machine_page: index page FAILED (status=%s code=%s): %s"
              % (e.status, e.code, e), file=sys.stderr)
        return 1
    print("  WP index: %s (id=%s, status=%s) link=%s"
          % ("created" if index_created else "found/updated", index_page["id"],
             index_page.get("status"), index_page["link"]))

    if cfg.get("machine_short_name") != args.short_name:
        cfg["machine_short_name"] = args.short_name
        save_config(cfg)
        print("  remembered machine_short_name=%s in %s -- future runs on this machine (this "
              "script, and the web UI's \"Publish to WordPress\" form) default to it"
              % (args.short_name, wp_client.CONFIG_PATH))
    return 0


if __name__ == "__main__":
    sys.exit(main())
