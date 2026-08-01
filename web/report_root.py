"""
web/report_root.py -- shared report-root git plumbing (doc/REPORT_HIERARCHY.md's WSPY_REPORT_ROOT
convention: a separate git-tracked tree, sibling to this checkout, holding curated *.md reports).

Extracted from wspy-publish (INVESTIGATION.md's "Test-point-level curated performance-summary README
deep-dive") so a second tool -- wspy-testpoint's run role-assignment persistence -- can reuse the same
clone-or-verify logic without duplicating it. Kept separate from web/wp_client.py: that module is
WordPress-specific, this one is plain git/filesystem with no WP dependency at all.

Commits made through run_git() are always local only -- nothing in this module ever pushes, since a
push is a shared/visible action that deserves an explicit human decision every time, not automation.
"""
import os
import subprocess

SCRIPT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # repo root, one up from web/
DEFAULT_REPORT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", "workload"))
DEFAULT_REPORT_ROOT_REMOTE = "https://github.com/mvermeulen/workload.git"


def run_git(args, cwd):
    result = subprocess.run(["git"] + args, cwd=cwd, capture_output=True, text=True)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def ensure_report_root(report_root, remote):
    """Clone `remote` into `report_root` if it doesn't exist yet; if it
    does, just verify it's a git repo. Returns (ok, message)."""
    if os.path.isdir(os.path.join(report_root, ".git")):
        rc, out, err = run_git(["remote", "get-url", "origin"], cwd=report_root)
        if rc != 0:
            return False, "%s exists but `git remote get-url origin` failed: %s" % (report_root, err)
        if out.rstrip("/").removesuffix(".git") != remote.rstrip("/").removesuffix(".git"):
            return False, ("%s exists but its origin (%s) does not match the expected report-root "
                            "remote (%s) -- refusing to touch it" % (report_root, out, remote))
        return True, "%s already cloned (origin: %s)" % (report_root, out)
    if os.path.exists(report_root):
        return False, "%s exists but is not a git repo -- refusing to touch it" % report_root
    rc, out, err = run_git(["clone", remote, report_root], cwd=os.path.dirname(report_root) or ".")
    if rc != 0:
        return False, "git clone %s %s failed: %s" % (remote, report_root, err)
    return True, "cloned %s -> %s" % (remote, report_root)
