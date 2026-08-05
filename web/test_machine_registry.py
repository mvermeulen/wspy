#!/usr/bin/env python3
"""
web/test_machine_registry.py -- unit tests for web/machine_registry.py's read-only scan of
scripts/publish_machine_page.py's <report-root>/machine/*/machine.json catalog. Not wired into
make test/run_tests.sh, same "web/ is stdlib-only Python, not covered by the C toolchain's test
targets" convention as web/test_wp_client.py -- run standalone:

    python3 web/test_machine_registry.py

No real report-root needed: builds a throwaway catalog directory under tempfile.TemporaryDirectory().
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import machine_registry


def write_sidecar(report_root_path, short_name, **fields):
    machine_dir = os.path.join(report_root_path, "machine", short_name)
    os.makedirs(machine_dir, exist_ok=True)
    with open(os.path.join(machine_dir, "machine.json"), "w") as f:
        json.dump({"short_name": short_name, **fields}, f)


class LoadTest(unittest.TestCase):
    def test_empty_when_catalog_directory_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(machine_registry.load(tmp), {})

    def test_maps_hostname_to_short_name(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_sidecar(tmp, "amd-370-64gb", hostname="roswell", vendor="amd")
            write_sidecar(tmp, "amd-395", hostname="workbench", vendor="amd")
            self.assertEqual(machine_registry.load(tmp),
                              {"roswell": "amd-370-64gb", "workbench": "amd-395"})

    def test_skips_sidecars_with_no_hostname_field(self):
        # A sidecar written before publish_machine_page.py started recording hostname.
        with tempfile.TemporaryDirectory() as tmp:
            write_sidecar(tmp, "amd-395", vendor="amd")
            self.assertEqual(machine_registry.load(tmp), {})

    def test_skips_unparseable_sidecar(self):
        with tempfile.TemporaryDirectory() as tmp:
            machine_dir = os.path.join(tmp, "machine", "broken")
            os.makedirs(machine_dir)
            with open(os.path.join(machine_dir, "machine.json"), "w") as f:
                f.write("not json")
            self.assertEqual(machine_registry.load(tmp), {})


class ResolveSlugTest(unittest.TestCase):
    def test_returns_slug_for_known_hostname(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_sidecar(tmp, "amd-370-64gb", hostname="roswell")
            self.assertEqual(machine_registry.resolve_slug(tmp, "roswell"), "amd-370-64gb")

    def test_returns_none_for_unknown_hostname(self):
        with tempfile.TemporaryDirectory() as tmp:
            write_sidecar(tmp, "amd-370-64gb", hostname="roswell")
            self.assertIsNone(machine_registry.resolve_slug(tmp, "someone-elses-laptop"))


if __name__ == "__main__":
    unittest.main()
