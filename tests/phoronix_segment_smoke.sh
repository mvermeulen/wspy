#!/bin/bash
# tests/phoronix_segment_smoke.sh - smoke tests for wspy-phoronix-segment (INVESTIGATION.md 4.4(c)
# "Phoronix-specific telemetry segmentation"; doc/phoronix_hook_investigation.md for the full
# investigation/design writeup this tool implements).
#
# Exercises both correlation sources against real fake fixtures driven through the real binary (no
# mocking): the preferred pts_hooks.log path (two test-option spans sliced out of a --interval CSV,
# other rows outside either span correctly excluded), the composite.xml + per-hash .log fallback path
# (a two-trial pass sliced using composite.xml's own test-run-times durations against each trial's
# precise start time -- regression coverage for the original scripts/wspy-phoronix-segment.py
# prototype's own worked behavior, now folded into the real tool), a pass with neither source
# degrading to "nothing segmented" without erroring, and a malformed/truncated pts_hooks.log (an
# unpaired trailing START) still segmenting whatever paired cleanly rather than aborting the whole
# pass. All isolated via PTS_USER_PATH (never the real ~/.phoronix-test-suite) and a scratch --rundir,
# same idiom tests/testpoint_smoke.sh already established.
#
# Usage: ./tests/phoronix_segment_smoke.sh (run from repo root; expects ./wspy-phoronix-segment to be
# present -- a plain Python script, no build step).

set -u
cd "$(dirname "$0")/.." || exit 1

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

epoch() {
    # ISO8601 UTC -> epoch seconds, via the same fromisoformat() parsing wspy-phoronix-segment's own
    # parse_iso8601() uses, so the smoke test can never drift from the tool's own interpretation.
    python3 -c "import datetime,sys; print(datetime.datetime.fromisoformat(sys.argv[1].replace('Z','+00:00')).timestamp())" "$1"
}

write_interval_csv() {
    # $1=path, rest = time values, one row per value (a single "ipc" data column is enough to prove
    # slicing works -- this tool doesn't care which/how-many non-time columns exist).
    local path="$1"; shift
    { echo "time,ipc"; for t in "$@"; do echo "$t,1.0"; done; } > "$path"
}

echo ""
echo "=== Testing the pts_hooks.log path (preferred, per-test-option granularity) ==="
RUNDIR1="$WORKDIR/run1"
mkdir -p "$RUNDIR1"
PASS_START1="2026-01-01T00:00:00.000Z"
PASS_FINISH1="2026-01-01T00:01:00.000Z"
EPOCH1="$(epoch "$PASS_START1")"

cat > "$RUNDIR1/manifest.json" <<EOF
{"suite":"phoronix","benchmark":"mybench","run_id":"run1",
 "passes":[{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json",
            "pts_hooks_log":"counters.pts_hooks.log","status":"ok"}]}
EOF
cat > "$RUNDIR1/counters.manifest.json" <<EOF
{"schema_version":"1.9.0","timing":{"start_time":"$PASS_START1","finish_time":"$PASS_FINISH1"}}
EOF
write_interval_csv "$RUNDIR1/counters.csv" 5 15 25 35 45 55

awk -v e="$EPOCH1" 'BEGIN{
    printf "%.6f\tSTART\th1\t1\t3\tpts/testa-1.0.0\targsA\t\t\n", e+10
    printf "%.6f\tFINISH\th1\t1\t3\tpts/testa-1.0.0\targsA\t1.0\t0.1\n", e+20
    printf "%.6f\tSTART\th2\t1\t2\tpts/testb-1.0.0\targsB\t\t\n", e+30
    printf "%.6f\tFINISH\th2\t1\t2\tpts/testb-1.0.0\targsB\t2.0\t0.2\n", e+40
}' > "$RUNDIR1/counters.pts_hooks.log"

OUT1="$(./wspy-phoronix-segment --rundir "$RUNDIR1" 2>&1)"
SEG1="$RUNDIR1/segmented/counters"
[ -f "$SEG1/testa_argsa_counters.csv" ] || fail "expected testa_argsa_counters.csv, got: $OUT1"
[ -f "$SEG1/testb_argsb_counters.csv" ] || fail "expected testb_argsb_counters.csv, got: $OUT1"
grep -q "^15,1.0$" "$SEG1/testa_argsa_counters.csv" 2>/dev/null || fail "testa slice missing its time=15 row"
grep -q "^35,1.0$" "$SEG1/testb_argsb_counters.csv" 2>/dev/null || fail "testb slice missing its time=35 row"
for f in "$SEG1/testa_argsa_counters.csv" "$SEG1/testb_argsb_counters.csv"; do
    for t in 5 25 45 55; do
        grep -q "^${t}," "$f" 2>/dev/null && fail "$f wrongly includes a row outside its own span (time=$t)"
    done
done
echo "$OUT1" | grep -q "Using pts_hooks.log" || fail "expected the tool to report using pts_hooks.log"
[ "$FAIL" -eq 0 ] && echo "pts_hooks.log path OK"

echo ""
echo "=== Testing the composite.xml + per-hash .log fallback path ==="
RUNDIR2="$WORKDIR/run2"
mkdir -p "$RUNDIR2"
PASS_START2="2026-02-01T00:00:00.000Z"
PASS_FINISH2="2026-02-01T00:02:00.000Z"

cat > "$RUNDIR2/manifest.json" <<EOF
{"suite":"phoronix","benchmark":"mybench","run_id":"run2",
 "passes":[{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json",
            "pts_hooks_log":null,"status":"ok"}]}
EOF
cat > "$RUNDIR2/counters.manifest.json" <<EOF
{"schema_version":"1.9.0","timing":{"start_time":"$PASS_START2","finish_time":"$PASS_FINISH2"}}
EOF
write_interval_csv "$RUNDIR2/counters.csv" 5 15 30 50

export PTS_USER_PATH="$WORKDIR/ptsdata"
RESDIR="$PTS_USER_PATH/test-results/2026-02-01-0000"
mkdir -p "$RESDIR/test-logs/deadbeef"
cat > "$RESDIR/composite.xml" <<'EOF'
<?xml version="1.0"?>
<PhoronixTestSuite>
  <System><TimeStamp>2026-02-01 00:00:05</TimeStamp></System>
  <Result>
    <Identifier>pts/mybench-1.0.0</Identifier>
    <Description>Algorithm: SHA256</Description>
    <Data><Entry><JSON>{"test-run-times": "10:10"}</JSON></Entry></Data>
  </Result>
</PhoronixTestSuite>
EOF
mkdir -p "$PTS_USER_PATH/installed-tests/pts/mybench-1.0.0"
cat > "$PTS_USER_PATH/installed-tests/pts/mybench-1.0.0/pts-install.json" <<'EOF'
{"test_installation": {"history": {"per_run_times": {
    "all": {}, "deadbeef": {"desc": "Algorithm: SHA256"}
}}}}
EOF
cat > "$RESDIR/test-logs/deadbeef/results.log" <<'EOF'
#####
2026-02-01 00:00 - Run 1
2026-02-01 00:00:10
#####
2026-02-01 00:00 - Run 2
2026-02-01 00:00:25
#####
EOF

OUT2="$(./wspy-phoronix-segment --rundir "$RUNDIR2" 2>&1)"
SEG2="$RUNDIR2/segmented/counters"
[ -f "$SEG2/algorithm_sha256_run1_counters.csv" ] || fail "expected trial-1 slice, got: $OUT2"
[ -f "$SEG2/algorithm_sha256_run2_counters.csv" ] || fail "expected trial-2 slice, got: $OUT2"
grep -q "^15,1.0$" "$SEG2/algorithm_sha256_run1_counters.csv" 2>/dev/null || fail "trial-1 slice missing its time=15 row"
grep -q "^30,1.0$" "$SEG2/algorithm_sha256_run2_counters.csv" 2>/dev/null || fail "trial-2 slice missing its time=30 row"
echo "$OUT2" | grep -q "Matched Phoronix results directory" || fail "expected the composite.xml fallback to report a match"
[ "$FAIL" -eq 0 ] && echo "composite.xml fallback path OK"
unset PTS_USER_PATH

echo ""
echo "=== Testing a pass with neither source degrades cleanly (no crash, nothing segmented) ==="
RUNDIR3="$WORKDIR/run3"
mkdir -p "$RUNDIR3"
export PTS_USER_PATH="$WORKDIR/empty-ptsdata"
cat > "$RUNDIR3/manifest.json" <<EOF
{"suite":"phoronix","benchmark":"nomatch","run_id":"run3",
 "passes":[{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json",
            "pts_hooks_log":null,"status":"ok"}]}
EOF
cat > "$RUNDIR3/counters.manifest.json" <<EOF
{"schema_version":"1.9.0","timing":{"start_time":"2026-03-01T00:00:00.000Z","finish_time":"2026-03-01T00:01:00.000Z"}}
EOF
write_interval_csv "$RUNDIR3/counters.csv" 5 15

if ! OUT3="$(./wspy-phoronix-segment --rundir "$RUNDIR3" 2>&1)"; then
    fail "tool exited non-zero on a pass with no correlation source at all: $OUT3"
fi
echo "$OUT3" | grep -q "nothing was segmented" || fail "expected an explicit 'nothing segmented' message, got: $OUT3"
[ -d "$RUNDIR3/segmented" ] && fail "should not have created a segmented/ dir when nothing matched"
[ "$FAIL" -eq 0 ] && echo "no-source degrade OK"
unset PTS_USER_PATH

echo ""
echo "=== Testing a truncated pts_hooks.log (unpaired trailing START) still segments what it can ==="
RUNDIR4="$WORKDIR/run4"
mkdir -p "$RUNDIR4"
PASS_START4="2026-04-01T00:00:00.000Z"
EPOCH4="$(epoch "$PASS_START4")"
cat > "$RUNDIR4/manifest.json" <<EOF
{"suite":"phoronix","benchmark":"mybench","run_id":"run4",
 "passes":[{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json",
            "pts_hooks_log":"counters.pts_hooks.log","status":"ok"}]}
EOF
cat > "$RUNDIR4/counters.manifest.json" <<EOF
{"schema_version":"1.9.0","timing":{"start_time":"$PASS_START4","finish_time":"2026-04-01T00:01:00.000Z"}}
EOF
write_interval_csv "$RUNDIR4/counters.csv" 5 15 25
awk -v e="$EPOCH4" 'BEGIN{
    printf "%.6f\tSTART\th1\t1\t1\tpts/testa-1.0.0\targsA\t\t\n", e+10
    printf "%.6f\tFINISH\th1\t1\t1\tpts/testa-1.0.0\targsA\t1.0\t0.1\n", e+20
    printf "%.6f\tSTART\th2\t1\t1\tpts/testb-1.0.0\targsB\t\t\n", e+30
}' > "$RUNDIR4/counters.pts_hooks.log"

if ! OUT4="$(./wspy-phoronix-segment --rundir "$RUNDIR4" 2>&1)"; then
    fail "tool exited non-zero on a truncated pts_hooks.log: $OUT4"
fi
SEG4="$RUNDIR4/segmented/counters"
[ -f "$SEG4/testa_argsa_counters.csv" ] || fail "the cleanly-paired span should still have segmented: $OUT4"
[ -f "$SEG4/testb_argsb_counters.csv" ] && fail "the unpaired trailing START must not produce a slice"
[ "$FAIL" -eq 0 ] && echo "truncated pts_hooks.log degrade OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-phoronix-segment smoke tests passed ==="
    exit 0
else
    echo "=== wspy-phoronix-segment smoke tests FAILED ==="
    exit 1
fi
