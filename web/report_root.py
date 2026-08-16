"""
web/report_root.py -- shared report-root git plumbing (doc/REPORT_HIERARCHY.md's WSPY_REPORT_ROOT
convention: a separate git-tracked tree, sibling to this checkout, holding curated *.md reports).

Extracted from wspy-publish (INVESTIGATION.md's "Test-point-level curated performance-summary README
deep-dive") so a second tool -- wspy-testpoint's run role-assignment persistence -- can reuse the same
clone-or-verify logic without duplicating it. Kept separate from web/wp_client.py: that module is
WordPress-specific, this one is plain git/filesystem with no WP dependency at all.

Commits made through commit_paths() are always local only. push_report_root() (INVESTIGATION.md
4.4(a) "One-click end-to-end pipeline", the -> published leg) is the one deliberate exception -- and
even that stays consistent with the rule's own reasoning ("a push is a shared/visible action that
deserves an explicit human decision every time, not automation"): it's reachable only through a
separate, explicitly-opted-into web-UI action (the report page's "Publish test-point report" card's
own "push to remote" checkbox, unchecked by default), never bundled into the automatic
ingest/select-runs/render chain, and defaults to git's own --dry-run so even that explicit opt-in
previews what would be pushed before a second, separate confirmation actually posts anything.
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


def commit_paths(report_root_path, rel_paths, message):
    """git add's each of rel_paths then commits, scoped to exactly those paths (a bare `git commit`
    with nothing restricting it would sweep up any *other* staged changes a human happens to have
    pending in the same report-root clone -- not just wrong scope, a real risk of committing someone
    else's in-progress work). "Nothing changed" (rel_paths already match what's committed) is a soft
    success, not an error -- the common re-run-with-no-real-changes case every subcommand's commit step
    needs to handle the same way. Detected by checking the index directly (`git diff --cached
    --name-only`, restricted to rel_paths) rather than string-matching git commit's own output: its
    "nothing to commit" message text varies depending on *unrelated* repo state (e.g. some other
    untracked file elsewhere produces "nothing added to commit but untracked files present" instead of
    "nothing to commit, working tree clean" -- confirmed live, a real bug this fixes, not a
    hypothetical), so parsing that text is fragile in a way checking the index isn't."""
    for rel_path in rel_paths:
        rc, _, err = run_git(["add", rel_path], cwd=report_root_path)
        if rc != 0:
            return False, "git add %s failed in %s: %s" % (rel_path, report_root_path, err)
    rc, out, err = run_git(["diff", "--cached", "--name-only", "--"] + rel_paths, cwd=report_root_path)
    if rc == 0 and not out.strip():
        return True, "no change to commit (already matches)"
    rc, out, err = run_git(["commit", "-m", message, "--"] + rel_paths, cwd=report_root_path)
    if rc != 0:
        return False, "git commit failed in %s: %s" % (report_root_path, err)
    rc, sha, _ = run_git(["rev-parse", "--short", "HEAD"], cwd=report_root_path)
    return True, "committed %d file(s) as %s (local only -- not pushed)" % (len(rel_paths), sha)


def push_report_root(report_root_path, dry_run=True):
    """Pushes report_root_path's current branch to its configured remote -- see this module's own
    top-of-file comment for why this, unlike every other function here, is allowed to touch the
    remote at all, and under what conditions. dry_run (default True) runs `git push --dry-run`
    instead of a real push -- git's own real dry-run, not a hand-rolled preview, so what it reports
    (commit range, branch names) is exactly what a real push would do, not an approximation of it.
    A plain `git push` with no explicit remote/refspec relies on the clone's own tracking branch
    (set up by `git clone`/`git push -u` normally, true for every report-root this module itself
    creates via ensure_report_root()) -- if a report-root was set up some other way with no
    tracking branch configured, git's own error message explains that rather than this function
    guessing at a remote/branch to force. Returns (ok, message) -- message is git's own combined
    stdout+stderr (git push's real, human-readable summary -- commit range, or "Everything
    up-to-date" -- goes to stderr even on success, so neither stream alone reliably has it)."""
    args = ["push", "--dry-run"] if dry_run else ["push"]
    rc, out, err = run_git(args, cwd=report_root_path)
    combined = "\n".join(s for s in (out, err) if s)
    if rc != 0:
        return False, combined or "git push failed in %s (exit %d)" % (report_root_path, rc)
    return True, combined or ("dry-run: nothing to push" if dry_run else "push completed")
