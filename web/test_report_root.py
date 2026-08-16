#!/usr/bin/env python3
"""
web/test_report_root.py -- unit tests for report_root.py's push_report_root() (INVESTIGATION.md
4.4(a) "One-click end-to-end pipeline", the -> published leg: the report page's "Publish test-point
report" card's opt-in "push to remote" step). Exercises real git operations against a real local
bare repo standing in for the remote (no network access needed, same "local bare git repo as a
remote" approach web/test_testpoint_web.py's own sibling test files use elsewhere in this codebase
for report-root testing) -- not the hand-rolled-preview kind of test, since the whole point of this
function is that git's own --dry-run does the real work. Not wired into make test/run_tests.sh, same
"web/ is stdlib-only Python, not covered by the C toolchain's test targets" convention as the rest of
web/test_*.py -- run standalone:

    python3 web/test_report_root.py
"""
import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import report_root


def run(argv, cwd):
    subprocess.run(argv, cwd=cwd, check=True, capture_output=True, text=True)


def make_bare_remote_and_clone(tmp):
    """A real local bare repo (the "remote") plus a real clone of it (the "report root") -- entirely
    offline, no network access, matching real git push/pull semantics exactly (unlike a hand-rolled
    fake). Returns (remote_path, clone_path)."""
    remote = os.path.join(tmp, "remote.git")
    run(["git", "init", "--bare", "-b", "main", remote], cwd=tmp)
    clone = os.path.join(tmp, "clone")
    run(["git", "clone", remote, clone], cwd=tmp)
    run(["git", "config", "user.email", "test@example.com"], cwd=clone)
    run(["git", "config", "user.name", "Test"], cwd=clone)
    with open(os.path.join(clone, "README.md"), "w") as f:
        f.write("initial\n")
    run(["git", "add", "README.md"], cwd=clone)
    run(["git", "commit", "-m", "initial"], cwd=clone)
    run(["git", "push", "-u", "origin", "main"], cwd=clone)
    return remote, clone


class PushReportRootTest(unittest.TestCase):
    def test_dry_run_does_not_move_the_remote(self):
        with tempfile.TemporaryDirectory() as tmp:
            remote, clone = make_bare_remote_and_clone(tmp)
            with open(os.path.join(clone, "README.md"), "a") as f:
                f.write("a new local-only line\n")
            run(["git", "commit", "-am", "local change"], cwd=clone)
            before_rc, before_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=remote)

            ok, message = report_root.push_report_root(clone, dry_run=True)

            self.assertTrue(ok, message)
            after_rc, after_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=remote)
            self.assertEqual(before_sha, after_sha, "dry-run push moved the remote's ref")

    def test_dry_run_reports_what_would_be_pushed(self):
        with tempfile.TemporaryDirectory() as tmp:
            remote, clone = make_bare_remote_and_clone(tmp)
            with open(os.path.join(clone, "README.md"), "a") as f:
                f.write("a new local-only line\n")
            run(["git", "commit", "-am", "local change"], cwd=clone)

            ok, message = report_root.push_report_root(clone, dry_run=True)

            self.assertTrue(ok, message)
            self.assertIn("main", message)  # git's own dry-run summary names the branch/refspec

    def test_dry_run_with_nothing_to_push(self):
        with tempfile.TemporaryDirectory() as tmp:
            remote, clone = make_bare_remote_and_clone(tmp)
            ok, message = report_root.push_report_root(clone, dry_run=True)
            self.assertTrue(ok, message)

    def test_real_push_moves_the_remote(self):
        with tempfile.TemporaryDirectory() as tmp:
            remote, clone = make_bare_remote_and_clone(tmp)
            with open(os.path.join(clone, "README.md"), "a") as f:
                f.write("a new local-only line\n")
            run(["git", "commit", "-am", "local change"], cwd=clone)
            _, local_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=clone)

            ok, message = report_root.push_report_root(clone, dry_run=False)

            self.assertTrue(ok, message)
            _, remote_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=remote)
            self.assertEqual(local_sha, remote_sha, "real push did not move the remote's ref")

    def test_failure_reports_git_s_own_error(self):
        with tempfile.TemporaryDirectory() as tmp:
            not_a_repo = os.path.join(tmp, "not-a-repo")
            os.makedirs(not_a_repo)
            ok, message = report_root.push_report_root(not_a_repo, dry_run=True)
            self.assertFalse(ok)
            self.assertTrue(message)

    def test_default_is_dry_run(self):
        # The safety-critical default: calling with no dry_run argument at all must never push for
        # real -- this is the one guard against a caller (or a future refactor) accidentally
        # dropping the dry_run=True it meant to pass.
        with tempfile.TemporaryDirectory() as tmp:
            remote, clone = make_bare_remote_and_clone(tmp)
            with open(os.path.join(clone, "README.md"), "a") as f:
                f.write("a new local-only line\n")
            run(["git", "commit", "-am", "local change"], cwd=clone)
            before_rc, before_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=remote)

            ok, message = report_root.push_report_root(clone)

            self.assertTrue(ok, message)
            after_rc, after_sha, _ = report_root.run_git(["rev-parse", "main"], cwd=remote)
            self.assertEqual(before_sha, after_sha, "default call pushed for real")


if __name__ == "__main__":
    unittest.main()
