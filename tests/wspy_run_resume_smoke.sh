#!/bin/bash
# tests/wspy_run_resume_smoke.sh - smoke tests for `wspy-run --resume` (item 6 Phase B,
# INVESTIGATION.md 4.4(a) "Detect and resume interrupted wspy-run profiles").
#
# Exercises the real ./wspy-run script (not through wspy-queue) against a fake `wspy` binary
# that records every pass it's actually invoked for and writes a real-shaped manifest.json --
# same "substitute a fake wspy that just records how it was invoked" approach
# tests/wspy_queue_smoke.sh already established, reused here rather than reinvented. Does not
# need ./wspy built at all.
#
# Usage: ./tests/wspy_run_resume_smoke.sh (run from repo root; expects ./wspy-run to be present)

set -u
cd "$(dirname "$0")/.." || exit 1

FAILURES=0
CHECKS=0

fail() {
  echo "FAIL: $1"
  FAILURES=$((FAILURES + 1))
}

check() {
  CHECKS=$((CHECKS + 1))
  if ! eval "$1"; then
    fail "$2"
  fi
}

if [ ! -x ./wspy-run ]; then
  echo "SKIP: ./wspy-run not found or not executable"
  exit 0
fi
if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP: python3 not available -- --resume's skip decision always degrades to \"rerun\" without it"
  exit 0
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

FAKEBIN="$WORKDIR/fakebin"
mkdir -p "$FAKEBIN"

# Fake wspy: honors just enough of -o/--manifest/--config-name/--config-option/-- <workload> to
# let wspy-run's run_pass() and pass_resume_check.py treat it like the real thing. Every
# invocation appends its own --config-name to $FAKE_WSPY_INVOKE_LOG -- the thing this test
# actually asserts on ("was this pass skipped, or genuinely rerun").
cat > "$FAKEBIN/wspy" <<'PYEOF'
#!/usr/bin/env python3
import sys, json, os

args = sys.argv[1:]
outfile = manifest = config_name = preset = None
config_options = []
i = 0
while i < len(args):
    a = args[i]
    if a == "-o":
        outfile = args[i + 1]; i += 2
    elif a == "--manifest":
        manifest = args[i + 1]; i += 2
    elif a == "--config-name":
        config_name = args[i + 1]; i += 2
    elif a == "--preset-name":
        preset = args[i + 1]; i += 2
    elif a == "--config-option":
        k, _, v = args[i + 1].partition("=")
        config_options.append({"name": k, "value": v}); i += 2
    elif a == "--affinity":
        i += 2
    elif a == "--":
        i += 1
        break
    else:
        i += 1
workload = args[i:]

log = os.environ.get("FAKE_WSPY_INVOKE_LOG")
if log:
    with open(log, "a") as f:
        f.write((config_name or "?") + "\n")

if outfile:
    with open(outfile, "w") as f:
        f.write("fake output for %s\n" % config_name)

if manifest:
    doc = {
        "manifest_schema_version": "1.9.0",
        "command": {"argv": workload},
        "host": {"hostname": "fakehost", "cpu_vendor": "AMD"},
        "timing": {"start_time": "2026-08-16T00:00:00.000Z", "elapsed_seconds": 0.1},
        "exit_status": {"known": True, "exited": True, "signaled": False, "exit_code": 0},
        "configuration_provenance": {"preset": preset, "configuration": config_name,
                                      "options": config_options},
    }
    with open(manifest, "w") as f:
        json.dump(doc, f)
sys.exit(0)
PYEOF
chmod +x "$FAKEBIN/wspy"

CONF="$WORKDIR/two-passes.conf"
cat > "$CONF" <<'EOF'
passA --counters=ipc --no-rusage
passB --counters=topdown --no-rusage
EOF

WSPY_RUN="$(pwd)/wspy-run"
LOG="$WORKDIR/invoke.log"

run_it() {
  FAKE_WSPY_INVOKE_LOG="$LOG" "$WSPY_RUN" --wspy "$FAKEBIN/wspy" "$@"
}

# --- Test 1: a resumed run skips every pass that already completed cleanly with the exact
#     same configuration, and the regenerated top-level manifest.json reflects that. ---
OUT1="$WORKDIR/out1"
: > "$LOG"
run_it -o "$OUT1" --suite manual --benchmark resumetest --run-id run1 \
  -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke1.log 2>&1
RUNDIR1="$OUT1/manual/resumetest/run1"
check '[ -f "$RUNDIR1/manifest.json" ]' "first (uninterrupted) run did not produce a top-level manifest.json"
check '[ "$(sort -u "$LOG")" = "$(printf "passA\npassB")" ]' \
  "first run did not invoke fake wspy for both passes ($(cat "$LOG"))"

# Simulate the crash: the one file generate_manifest() writes last never got written.
rm -f "$RUNDIR1/manifest.json"
: > "$LOG"
run_it --resume "$RUNDIR1" -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke2.log 2>&1
check '[ ! -s "$LOG" ]' \
  "resume with an identical configuration re-invoked wspy for a pass that should have been skipped: $(cat "$LOG")"
check '[ -f "$RUNDIR1/manifest.json" ]' "resumed run did not regenerate the top-level manifest.json"
check 'grep -c "\"status\": \"skipped\"" "$RUNDIR1/manifest.json" | grep -q "^2$"' \
  "resumed run's manifest.json does not show both passes as skipped ($(cat "$RUNDIR1/manifest.json"))"

# --- Test 2: a resume with a genuinely different configuration (here: a different workload
#     command) must NOT skip -- the recorded pass_flags_hash no longer matches. ---
OUT2="$WORKDIR/out2"
: > "$LOG"
run_it -o "$OUT2" --suite manual --benchmark resumetest --run-id run2 \
  -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke3.log 2>&1
RUNDIR2="$OUT2/manual/resumetest/run2"
rm -f "$RUNDIR2/manifest.json"
: > "$LOG"
run_it --resume "$RUNDIR2" -c "$CONF" -- sleep 2 >/tmp/wspy_run_resume_smoke4.log 2>&1
check '[ "$(sort -u "$LOG")" = "$(printf "passA\npassB")" ]' \
  "resume with a different workload command wrongly skipped a pass instead of rerunning it ($(cat "$LOG"))"
check 'grep -c "\"status\": \"ok\"" "$RUNDIR2/manifest.json" | grep -q "^2$"' \
  "mismatched-configuration resume's manifest.json does not show both passes freshly re-run as ok"

# --- Test 3: a pass that never got the chance to write its own manifest (crashed
#     mid-execution, not just before the final top-level manifest) is always rerun, regardless
#     of any other pass's own resumability. ---
OUT3="$WORKDIR/out3"
: > "$LOG"
run_it -o "$OUT3" --suite manual --benchmark resumetest --run-id run3 \
  -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke5.log 2>&1
RUNDIR3="$OUT3/manual/resumetest/run3"
rm -f "$RUNDIR3/manifest.json" "$RUNDIR3/passB.manifest.json" "$RUNDIR3/passB.txt"
: > "$LOG"
run_it --resume "$RUNDIR3" -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke6.log 2>&1
check '[ "$(cat "$LOG")" = "passB" ]' \
  "resume did not skip passA and rerun only passB as expected ($(cat "$LOG"))"
check 'grep -q "\"name\": \"passA\", \"output\": \"passA.txt\", \"text_output\": null, \"manifest\": \"passA.manifest.json\", \"pts_hooks_log\": null, \"status\": \"skipped\"" "$RUNDIR3/manifest.json"' \
  "passA not recorded as skipped in the regenerated manifest.json"
check 'grep -q "\"name\": \"passB\", \"output\": \"passB.txt\", \"text_output\": null, \"manifest\": \"passB.manifest.json\", \"pts_hooks_log\": null, \"status\": \"ok\"" "$RUNDIR3/manifest.json"' \
  "passB not recorded as freshly-run/ok in the regenerated manifest.json"

# --- Test 4: --dry-run --resume reports the skip decision without touching the filesystem. ---
OUT4="$WORKDIR/out4"
: > "$LOG"
run_it -o "$OUT4" --suite manual --benchmark resumetest --run-id run4 \
  -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke7.log 2>&1
RUNDIR4="$OUT4/manual/resumetest/run4"
rm -f "$RUNDIR4/manifest.json"
: > "$LOG"
dry_out=$(run_it --dry-run --resume "$RUNDIR4" -c "$CONF" -- sleep 1 2>&1)
check '[ ! -s "$LOG" ]' "--dry-run --resume actually invoked wspy"
check 'echo "$dry_out" | grep -q "\[passA\] skipping"' \
  "--dry-run --resume did not report passA as skipping: $dry_out"
check 'echo "$dry_out" | grep -q "\[passB\] skipping"' \
  "--dry-run --resume did not report passB as skipping: $dry_out"

# --- Test 5: --resume rejects a directory whose run already finished (real top-level
#     manifest.json present -- nothing to resume). ---
OUT5="$WORKDIR/out5"
: > "$LOG"
run_it -o "$OUT5" --suite manual --benchmark resumetest --run-id run5 \
  -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke8.log 2>&1
RUNDIR5="$OUT5/manual/resumetest/run5"
run_it --resume "$RUNDIR5" -c "$CONF" -- sleep 1 >/tmp/wspy_run_resume_smoke9.log 2>&1
rc=$?
check '[ $rc -ne 0 ]' "--resume against an already-finished run directory exited 0 (should have been rejected)"

echo "$CHECKS checks, $FAILURES failures"
[ "$FAILURES" -eq 0 ]
