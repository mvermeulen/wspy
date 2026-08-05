"""
web/machine_registry.py -- read-only hostname -> machine-slug lookup, built from
`scripts/publish_machine_page.py`'s existing `<report-root>/machine/<short-name>/machine.json`
catalog (run once per physical machine). doc/REPORT_HIERARCHY.md's `<vendor>-<short-model>` slug is
deliberately informal/human-assigned; this module never derives or writes one -- it only reads the
catalog that script already maintains (INVESTIGATION.md 4.3 Tier 3 item 5's "Machine identity"
design, settled 2026-08-04, revised after finding that catalog already existed rather than adding a
second, separately-maintained registry).

Any sidecar written before publish_machine_page.py started recording `hostname` has no such key --
skipped rather than erroring, same "degrade independently" idiom the rest of this codebase uses for
partial data (e.g. provenance.c's per-field capture). Re-running publish_machine_page.py on that
machine refreshes it.
"""
import json
import os

CATALOG_SLUG = "machine"


def load(report_root_path):
    """{"<hostname>": "<machine-short-name>"} scanned from every
    <report-root>/machine/*/machine.json sidecar that has a "hostname" field. {} if the catalog
    directory doesn't exist yet."""
    catalog_dir = os.path.join(report_root_path, CATALOG_SLUG)
    out = {}
    if not os.path.isdir(catalog_dir):
        return out
    for name in sorted(os.listdir(catalog_dir)):
        sidecar_path = os.path.join(catalog_dir, name, "machine.json")
        try:
            with open(sidecar_path) as f:
                entry = json.load(f)
        except (OSError, ValueError):
            continue
        hostname = entry.get("hostname")
        short_name = entry.get("short_name")
        if hostname and short_name:
            out[hostname] = short_name
    return out


def resolve_slug(report_root_path, hostname):
    """This hostname's registered machine short-name, or None if it's never run
    scripts/publish_machine_page.py (or did so before that script recorded hostname)."""
    return load(report_root_path).get(hostname)
