#!/usr/bin/env python3
"""
scripts/test_pass_resume_check.py -- unit tests for pass_resume_check.py (wspy-run --resume's
per-pass skip decision, item 6 Phase B, INVESTIGATION.md 4.4(a)). Not wired into make test/
run_tests.sh, same "standalone Python helper, no C toolchain involvement" convention
estimate_tree_timeout.py's own lack of a dedicated test file already follows for scripts/ -- run
standalone:

    python3 scripts/test_pass_resume_check.py
"""
import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pass_resume_check


def write_manifest(path, exit_code=0, known=True, exited=True, signaled=False, pass_flags_hash="abc123"):
    doc = {
        "exit_status": {"known": known, "exited": exited, "signaled": signaled, "exit_code": exit_code},
        "configuration_provenance": {
            "preset": "deep-cpu", "configuration": "system-metrics",
            "options": ([{"name": "pass_flags_hash", "value": pass_flags_hash}]
                        if pass_flags_hash is not None else []),
        },
    }
    with open(path, "w") as f:
        json.dump(doc, f)


class MainTest(unittest.TestCase):
    def test_matching_hash_and_clean_exit_skips(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, exit_code=0, pass_flags_hash="abc123")
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 0)

    def test_mismatched_hash_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, exit_code=0, pass_flags_hash="abc123")
            self.assertEqual(pass_resume_check.main(["prog", path, "different"]), 1)

    def test_dirty_exit_code_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, exit_code=1, pass_flags_hash="abc123")
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 1)

    def test_signaled_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, exit_code=0, signaled=True, pass_flags_hash="abc123")
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 1)

    def test_unknown_exit_status_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, known=False, pass_flags_hash="abc123")
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 1)

    def test_no_recorded_hash_reruns(self):
        # A manifest written before this Phase B slice existed.
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            write_manifest(path, exit_code=0, pass_flags_hash=None)
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 1)

    def test_missing_manifest_reruns(self):
        # Covers "never resume a pass that was itself interrupted mid-execution" on its own --
        # --manifest is the last thing a wspy invocation writes, so no file at all means that
        # pass never finished.
        self.assertEqual(pass_resume_check.main(["prog", "/no/such/manifest.json", "abc123"]), 1)

    def test_malformed_json_reruns(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "run.manifest.json")
            with open(path, "w") as f:
                f.write("{not valid json")
            self.assertEqual(pass_resume_check.main(["prog", path, "abc123"]), 1)

    def test_wrong_argc_is_a_usage_error(self):
        self.assertEqual(pass_resume_check.main(["prog", "only-one-arg"]), 1)


if __name__ == "__main__":
    unittest.main()
