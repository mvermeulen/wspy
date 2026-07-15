#!/bin/bash
# tests/wspy_queue_smoke.sh - smoke tests for wspy-queue and the job file
# format it shares with web/server.py (web/joblib.py).
#
# INVESTIGATION_4.0.md item 13, "Deployment/hosting design note": exercises
# the pending -> running -> done/failed job lifecycle, the requeue path, and
# job portability (a job file copied verbatim into a second, independent
# jobs/output tree processes correctly there) -- without needing root/perf
# access, real hardware counters, or a real workload, by substituting fake
# wspy/wspy-run/wspy-plot/wspy-store binaries that just record how they were
# invoked. Does not need ./wspy built at all.
#
# Usage: ./tests/wspy_queue_smoke.sh (run from repo root; expects ./wspy-run
# and ./wspy-queue to be present -- wspy-run needs no build step, wspy-queue
# is a plain Python script).

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

if [ ! -x ./wspy-queue ]; then
  echo "SKIP: ./wspy-queue not found or not executable"
  exit 0
fi
if [ ! -x ./wspy-run ]; then
  echo "SKIP: ./wspy-run not found or not executable"
  exit 0
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

FAKEBIN="$WORKDIR/fakebin"
mkdir -p "$FAKEBIN"

# Fake wspy: honors -o/--manifest/--run-index exactly enough for wspy-run's
# own run_pass() and joblib.py's execute_*_run() to treat it like the real
# thing, without touching perf/ptrace/root at all.
cat > "$FAKEBIN/wspy" <<'PYEOF'
#!/usr/bin/env python3
import sys, json
args = sys.argv[1:]
outfile = manifest = run_index = None
i = 0
while i < len(args):
    if args[i] == "-o":
        outfile = args[i + 1]; i += 2
    elif args[i] == "--manifest":
        manifest = args[i + 1]; i += 2
    elif args[i] == "--run-index":
        run_index = args[i + 1]; i += 2
    elif args[i] == "--":
        break
    else:
        i += 1
if outfile:
    open(outfile, "w").write("fake wspy output\nargs: %s\n" % " ".join(args))
if manifest:
    json.dump({"schema_version": "1.0.0", "exit_status": {"known": True, "code": 0}},
               open(manifest, "w"))
if run_index:
    with open(run_index, "a") as f:
        f.write(json.dumps({"hostname": "fakehost", "run_id": "fake-run"}) + "\n")
sys.exit(0)
PYEOF

cat > "$FAKEBIN/wspy-plot" <<'PYEOF'
#!/usr/bin/env python3
import sys
sys.exit(0)
PYEOF

cat > "$FAKEBIN/wspy-store" <<'PYEOF'
#!/usr/bin/env python3
import sys
sys.exit(0)
PYEOF

# Fake wspy-run that always fails to launch, used for the failure/requeue case.
MISSING_WSPY_RUN="$WORKDIR/no-such-wspy-run"

chmod +x "$FAKEBIN"/*

QUEUE="$(pwd)/wspy-queue"
WSPY_RUN="$(pwd)/wspy-run"

run_queue() {
  local jobs_dir="$1" output_root="$2"; shift 2
  "$QUEUE" --jobs-dir "$jobs_dir" --output-root "$output_root" \
    --wspy "$FAKEBIN/wspy" --wspy-run "$WSPY_RUN" --wspy-plot "$FAKEBIN/wspy-plot" \
    --wspy-store "$FAKEBIN/wspy-store" "$@"
}

# --- Test 1: preset-mode job, full pending -> running -> done lifecycle ---
JOBS1="$WORKDIR/jobs1"; RUNS1="$WORKDIR/runs1"
mkdir -p "$JOBS1" "$RUNS1"

add_out=$(run_queue "$JOBS1" "$RUNS1" add --suite manual --benchmark smoketest \
  --mode preset --profile quick -- sleep 1)
check '[ $? -eq 0 ] || true' "add (preset) exited nonzero"
job_id=$(echo "$add_out" | sed -n 's/^queued job \([^ ]*\).*/\1/p')
check '[ -n "$job_id" ]' "could not parse job id from add output: $add_out"
check '[ -f "$JOBS1/pending/$job_id.json" ]' "job file not created in pending/"

run_queue "$JOBS1" "$RUNS1" run >/tmp/wspy_queue_smoke_run1.log 2>&1
check '[ -f "$JOBS1/done/$job_id.json" ]' "preset job did not end up in done/ ($(cat /tmp/wspy_queue_smoke_run1.log))"
check '[ ! -e "$JOBS1/pending/$job_id.json" ]' "job file left behind in pending/ after processing"
check 'find "$RUNS1/manual/smoketest" -mindepth 1 -maxdepth 1 -type d | grep -q .' \
  "no run directory created under manual/smoketest"
rundir1=$(find "$RUNS1/manual/smoketest" -mindepth 1 -maxdepth 1 -type d | head -1)
check '[ -f "$rundir1/manifest.json" ]' "wspy-run-shaped manifest.json missing from run directory"

# --- Test 2: custom-mode job (checklist) ---
JOBS2="$WORKDIR/jobs2"; RUNS2="$WORKDIR/runs2"
mkdir -p "$JOBS2" "$RUNS2"
echo '{"counters": {"enabled": true, "groups": ["topdown"], "interval_secs": "1"}}' \
  > "$WORKDIR/checklist.json"

add_out=$(run_queue "$JOBS2" "$RUNS2" add --suite manual --benchmark custest \
  --mode custom --checklist-json "$WORKDIR/checklist.json" -- sleep 1)
job_id2=$(echo "$add_out" | sed -n 's/^queued job \([^ ]*\).*/\1/p')
check '[ -n "$job_id2" ]' "could not parse job id from custom add output: $add_out"

run_queue "$JOBS2" "$RUNS2" run >/tmp/wspy_queue_smoke_run2.log 2>&1
check '[ -f "$JOBS2/done/$job_id2.json" ]' "custom job did not end up in done/ ($(cat /tmp/wspy_queue_smoke_run2.log))"
rundir2=$(find "$RUNS2/manual/custest" -mindepth 1 -maxdepth 1 -type d | head -1)
check '[ -n "$rundir2" ] && [ -f "$rundir2/amdtopdown.csv" ]' \
  "custom job's single-group+interval pass did not produce amdtopdown.csv"

# --- Test 3: failure + requeue ---
JOBS3="$WORKDIR/jobs3"; RUNS3="$WORKDIR/runs3"
mkdir -p "$JOBS3" "$RUNS3"
add_out=$(run_queue "$JOBS3" "$RUNS3" add --suite manual --benchmark willfail \
  --mode preset --profile quick -- sleep 1)
job_id3=$(echo "$add_out" | sed -n 's/^queued job \([^ ]*\).*/\1/p')

"$QUEUE" --jobs-dir "$JOBS3" --output-root "$RUNS3" --wspy "$FAKEBIN/wspy" \
  --wspy-run "$MISSING_WSPY_RUN" --wspy-plot "$FAKEBIN/wspy-plot" \
  --wspy-store "$FAKEBIN/wspy-store" run >/tmp/wspy_queue_smoke_run3.log 2>&1
check '[ -f "$JOBS3/failed/$job_id3.json" ]' "job with a broken wspy-run path did not land in failed/"

"$QUEUE" --jobs-dir "$JOBS3" requeue "$job_id3" >/dev/null 2>&1
check '[ -f "$JOBS3/pending/$job_id3.json" ]' "requeue did not move the job back to pending/"
check '[ ! -e "$JOBS3/failed/$job_id3.json" ]' "requeue left a copy behind in failed/"

# --- Test 4: portability -- copy a job file to an independent jobs/output tree ---
JOBS4="$WORKDIR/jobs4"; RUNS4="$WORKDIR/runs4"
mkdir -p "$JOBS4/pending" "$RUNS4"
cp "$JOBS2/done/$job_id2.json" "$JOBS4/pending/"
check '! grep -q "$RUNS2" "$JOBS4/pending/$job_id2.json"' \
  "copied job file references the original machine's output-root path"
run_queue "$JOBS4" "$RUNS4" run >/tmp/wspy_queue_smoke_run4.log 2>&1
check '[ -f "$JOBS4/done/$job_id2.json" ]' \
  "job copied into a second, independent jobs/output tree did not process successfully"
check 'find "$RUNS4/manual/custest" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | grep -q .' \
  "portable job did not produce output under the second output-root"

# --- Test 5: invalid job is rejected at add-time, not queued ---
JOBS5="$WORKDIR/jobs5"; RUNS5="$WORKDIR/runs5"
mkdir -p "$JOBS5" "$RUNS5"
run_queue "$JOBS5" "$RUNS5" add --suite "bad suite" --mode preset --profile quick -- sleep 1 \
  >/tmp/wspy_queue_smoke_run5.log 2>&1
rc=$?
check '[ $rc -ne 0 ]' "add with an invalid suite name exited 0 (should have been rejected)"
check '[ -z "$(ls -A "$JOBS5/pending" 2>/dev/null)" ]' "invalid job was written to pending/ anyway"

echo
echo "$CHECKS checks, $FAILURES failures"
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
