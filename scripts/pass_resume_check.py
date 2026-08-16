#!/usr/bin/env python3
"""
scripts/pass_resume_check.py - wspy-run --resume's (item 6 Phase B, INVESTIGATION.md 4.4(a)
"Detect and resume interrupted wspy-run profiles") per-pass skip decision.

wspy-run shells out to this rather than hand-rolling JSON field extraction in bash (grep/sed
against a manifest.json is fragile against escaping/nesting; a real parse isn't) -- same
"python3 for anything that needs real parsing" posture estimate_tree_timeout.py already
established for this script's own caller.

Usage: pass_resume_check.py <manifest.json> <expected-pass-flags-hash>

Exit 0 ("skip" -- this pass already ran with this exact configuration and finished cleanly):
  the manifest parses, its exit_status is known/exited/not-signaled/exit_code==0 (same "clean
  exit" definition validate.c/server.py's run_status_from_exit_status() already use), and its
  configuration_provenance.options carries a "pass_flags_hash" entry equal to
  <expected-pass-flags-hash> (wspy-run's own compute_pass_flags_hash(), covering this pass's
  flags + --affinity + the workload argv -- everything that determines its actual behavior).

Exit 1 ("rerun" -- anything else): missing/unreadable manifest (including "this pass never
finished at all", since --manifest is the last thing a wspy invocation writes -- covers "never
resume a pass that was itself interrupted mid-execution" on its own, no separate check needed),
a dirty exit, no recorded pass_flags_hash at all (a manifest written before this Phase B slice
existed), or a hash that doesn't match (the profile/workload/--affinity/config file changed
since the interrupted attempt). No stdout on either path -- a bash `if script ...; then` caller
needs only the exit code; a one-line stderr note names *why* on the rerun path, for --resume's
own per-pass transcript.
"""
import json
import sys


def main(argv):
    if len(argv) != 3:
        print("usage: pass_resume_check.py <manifest.json> <expected-pass-flags-hash>", file=sys.stderr)
        return 1
    manifest_path, expected_hash = argv[1], argv[2]

    try:
        with open(manifest_path) as f:
            data = json.load(f)
    except (OSError, ValueError):
        print(f"pass_resume_check: no readable manifest at {manifest_path} -- rerunning", file=sys.stderr)
        return 1

    exit_status = data.get("exit_status") or {}
    if not (exit_status.get("known") and exit_status.get("exited")
            and not exit_status.get("signaled") and exit_status.get("exit_code") == 0):
        print(f"pass_resume_check: {manifest_path} recorded a non-clean exit -- rerunning", file=sys.stderr)
        return 1

    options = (data.get("configuration_provenance") or {}).get("options") or []
    recorded_hash = None
    for opt in options:
        if isinstance(opt, dict) and opt.get("name") == "pass_flags_hash":
            recorded_hash = opt.get("value")
            break

    if recorded_hash is None:
        print(f"pass_resume_check: {manifest_path} has no recorded pass_flags_hash "
              "(written before wspy-run --resume support existed) -- rerunning", file=sys.stderr)
        return 1
    if recorded_hash != expected_hash:
        print(f"pass_resume_check: {manifest_path}'s recorded configuration doesn't match this "
              f"invocation ({recorded_hash} != {expected_hash}) -- rerunning", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
