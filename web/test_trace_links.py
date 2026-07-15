#!/usr/bin/env python3
"""
web/test_trace_links.py - unit tests for server.py's _rundir_triple_for_path()/
_resolve_trace_links() (item 14, "Traceability links: summary row -> manifest
-> raw CSV -> plots -> tree artifacts"). Not wired into make test/run_tests.sh,
matching web/'s existing "stdlib-only Python, not covered by the C toolchain's
test targets" convention (see CLAUDE.md's web/ entry and web/test_joblib.py's
own docstring) -- run standalone:

    python3 web/test_trace_links.py

Only the pure path-derivation logic is covered here; the actual `wspy-summary
--trace` subprocess invocation (_discovery_trace()) is exercised by hand
against a real store (see this item's PR description), not re-tested here.
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import server


class RundirTripleForPathTest(unittest.TestCase):
    def test_resolves_path_under_output_root(self):
        root = "/data/wspy/runs"
        path = "/data/wspy/runs/phoronix/coremark/1000-1/amdtopdown.csv"
        self.assertEqual(server._rundir_triple_for_path(root, path),
                         ("phoronix", "coremark", "1000-1", "amdtopdown.csv"))

    def test_resolves_nested_filename(self):
        root = "/data/wspy/runs"
        path = "/data/wspy/runs/phoronix/coremark/1000-1/plots/amdtopdown.topdown.png"
        self.assertEqual(server._rundir_triple_for_path(root, path),
                         ("phoronix", "coremark", "1000-1", "plots/amdtopdown.topdown.png"))

    def test_none_for_path_outside_output_root(self):
        root = "/data/wspy/runs"
        path = "/some/other/host/path/amdtopdown.csv"
        self.assertIsNone(server._rundir_triple_for_path(root, path))

    def test_none_for_empty_path(self):
        self.assertIsNone(server._rundir_triple_for_path("/data/wspy/runs", ""))
        self.assertIsNone(server._rundir_triple_for_path("/data/wspy/runs", None))

    def test_none_for_too_shallow_path(self):
        # Missing the run_id level -- not a real unified-layout run directory.
        root = "/data/wspy/runs"
        path = "/data/wspy/runs/phoronix/coremark/amdtopdown.csv"
        self.assertIsNone(server._rundir_triple_for_path(root, path))

    def test_none_for_unsafe_segment(self):
        # A path-traversal-shaped segment must never resolve to a triple,
        # even though os.path.relpath() would happily compute one.
        root = "/data/wspy/runs"
        path = "/data/wspy/runs/../etc/coremark/1000-1/amdtopdown.csv"
        self.assertIsNone(server._rundir_triple_for_path(root, path))


class ResolveTraceLinksTest(unittest.TestCase):
    def test_builds_report_and_file_links(self):
        root = "/data/wspy/runs"
        fields = {
            "manifest_path": "/data/wspy/runs/phoronix/coremark/1000-1/amdtopdown.manifest.json",
            "output_path": "/data/wspy/runs/phoronix/coremark/1000-1/amdtopdown.csv",
            "tree_output_path": "",
        }
        links = server._resolve_trace_links(root, fields)
        self.assertEqual(links["report_url"], "/report/phoronix/coremark/1000-1")
        self.assertEqual(links["manifest_url"],
                         "/files/phoronix/coremark/1000-1/amdtopdown.manifest.json")
        self.assertEqual(links["output_url"], "/files/phoronix/coremark/1000-1/amdtopdown.csv")
        self.assertNotIn("tree_url", links)

    def test_no_links_when_nothing_resolves(self):
        fields = {
            "manifest_path": "/some/other/host/amdtopdown.manifest.json",
            "output_path": "/some/other/host/amdtopdown.csv",
            "tree_output_path": "",
        }
        links = server._resolve_trace_links("/data/wspy/runs", fields)
        self.assertEqual(links, {})

    def test_missing_fields_degrade_to_no_links(self):
        self.assertEqual(server._resolve_trace_links("/data/wspy/runs", {}), {})


if __name__ == "__main__":
    unittest.main()
