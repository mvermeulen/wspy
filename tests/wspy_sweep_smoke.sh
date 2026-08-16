#!/bin/bash
# tests/wspy_sweep_smoke.sh - smoke tests for wspy-sweep (INVESTIGATION.md's 4.2 Tier 2 "Comparison
# matrix mode" item). wspy-sweep had no automated test coverage at all before this -- a real,
# pre-existing gap found while extending it for 4.4(c)'s "wspy-run-profile-driven batchable Phoronix
# suite flow" item (wspy-phoronix-batch now depends on wspy-sweep's own spec format, so this closes
# the gap for both). Exercises the quick-form/declarative-spec duality (INVESTIGATION.md's own
# --profile/--config|-c "config_file" extension -- previously only the "profile" half of wspy-run's
# own builtin-profile-vs--c-<file> duality was actually implemented despite the module docstring's
# claim to mirror both), the --affinity axis cross-product, and mutual-exclusivity validation for
# both invocation shapes -- all against a fake wspy-run binary that just records its own argv, so
# this needs no build step, no root/perf access, and no real workload.
#
# Usage: ./tests/wspy_sweep_smoke.sh (run from repo root; expects ./wspy-sweep to be present -- a
# plain Python script, no build step).

set -u
cd "$(dirname "$0")/.." || exit 1

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

FAKEBIN="$WORKDIR/fakebin"
mkdir -p "$FAKEBIN"

# Records every invocation's argv (one line per call) to $WORKDIR/wspy-run-calls.log and exits 0,
# unless FAIL_ON is set and appears as a substring of this call's own argv (used to exercise
# wspy-sweep's own failure-tracking/reporting, not to test wspy-run itself).
cat > "$FAKEBIN/wspy-run" <<'PYEOF'
#!/usr/bin/env python3
import os, sys
argv = sys.argv[1:]
log_path = os.environ.get("WSPY_RUN_CALL_LOG")
if log_path:
    with open(log_path, "a") as f:
        f.write(" ".join(argv) + "\n")
fail_on = os.environ.get("WSPY_RUN_FAIL_ON")
if fail_on and fail_on in " ".join(argv):
    sys.exit(1)
sys.exit(0)
PYEOF
chmod +x "$FAKEBIN/wspy-run"

cat > "$FAKEBIN/wspy-store" <<'PYEOF'
#!/usr/bin/env python3
import sys
sys.exit(0)
PYEOF
chmod +x "$FAKEBIN/wspy-store"

CALLLOG="$WORKDIR/wspy-run-calls.log"
export WSPY_RUN_CALL_LOG="$CALLLOG"

SWEEP="./wspy-sweep --wspy-run-bin $FAKEBIN/wspy-run --wspy-store-bin $FAKEBIN/wspy-store -o $WORKDIR/out"

echo ""
echo "=== Testing quick form with --profile (dry-run) ==="
OUT="$($SWEEP --profile deep-cpu --dry-run -- true 2>&1)"
echo "$OUT" | grep -q -- "deep-cpu -- true" || fail "expected the profile positional before --: $OUT"
[ "$FAIL" -eq 0 ] && echo "quick-form --profile dry-run OK"

echo ""
echo "=== Testing quick form with --config/-c (dry-run) ==="
CONFFILE="$WORKDIR/my-passes.conf"
echo "counters --topdown" > "$CONFFILE"
OUT="$($SWEEP -c "$CONFFILE" --dry-run -- true 2>&1)"
echo "$OUT" | grep -q -- "-c $CONFFILE -- true" || fail "expected -c <file> before --: $OUT"
[ "$FAIL" -eq 0 ] && echo "quick-form --config dry-run OK"

echo ""
echo "=== Testing --profile and --config together is rejected ==="
if OUT="$($SWEEP --profile deep-cpu -c "$CONFFILE" --dry-run -- true 2>&1)"; then
    fail "expected a nonzero exit when both --profile and --config are given: $OUT"
fi
echo "$OUT" | grep -qi "mutually exclusive" || fail "expected a mutually-exclusive error message: $OUT"
[ "$FAIL" -eq 0 ] && echo "--profile + --config rejection OK"

echo ""
echo "=== Testing neither --profile nor --config is rejected ==="
if OUT="$($SWEEP --dry-run -- true 2>&1)"; then
    fail "expected a nonzero exit when neither --profile nor --config is given: $OUT"
fi
[ "$FAIL" -eq 0 ] && echo "missing --profile/--config rejection OK"

echo ""
echo "=== Testing the --affinity axis cross-product (real invocation, not dry-run) ==="
: > "$CALLLOG"
OUT="$($SWEEP --affinity all,nosmt --profile quick -- true 2>&1)"
echo "$OUT" | grep -q "2 cell(s) (1 workload(s) x 2 axis combination(s))" || fail "expected 2 cells reported: $OUT"
[ "$(wc -l < "$CALLLOG")" -eq 2 ] || fail "expected exactly 2 real wspy-run invocations, got: $(cat "$CALLLOG")"
grep -q -- "--affinity all " "$CALLLOG" || fail "expected one cell with --affinity all"
grep -q -- "--affinity nosmt " "$CALLLOG" || fail "expected one cell with --affinity nosmt"
[ "$FAIL" -eq 0 ] && echo "--affinity cross-product OK"

echo ""
echo "=== Testing a spec file with 'profile' (real invocation) ==="
: > "$CALLLOG"
SPEC1="$WORKDIR/spec1.json"
cat > "$SPEC1" <<EOF
{"suite": "specsuite", "workloads": [{"name": "w1", "command": ["true"]}], "profile": "deep-cpu"}
EOF
OUT="$($SWEEP --spec "$SPEC1" 2>&1)"
grep -q -- "deep-cpu -- true" "$CALLLOG" || fail "expected the spec's profile in the real call: $(cat "$CALLLOG")"
grep -q -- "--suite specsuite" "$CALLLOG" || fail "expected the spec's suite name: $(cat "$CALLLOG")"
[ "$FAIL" -eq 0 ] && echo "spec with 'profile' OK"

echo ""
echo "=== Testing --summary-out (caller-discoverable run_id/rundir, e.g. wspy-phoronix-batch) ==="
SUMMARY="$WORKDIR/summary.json"
$SWEEP --spec "$SPEC1" --summary-out "$SUMMARY" >/dev/null 2>&1
python3 -c "
import json
with open('$SUMMARY') as f:
    summary = json.load(f)
assert len(summary) == 1, summary
entry = summary[0]
assert entry['workload_name'] == 'w1', entry
assert entry['exit_code'] == 0, entry
assert entry['rundir'].endswith('specsuite/w1/' + entry['run_id']), entry
" || fail "--summary-out did not produce the expected {workload_name, run_id, rundir, exit_code} shape"
[ "$FAIL" -eq 0 ] && echo "--summary-out OK"

echo ""
echo "=== Testing --summary-out is not written on --dry-run ==="
rm -f "$SUMMARY"
$SWEEP --spec "$SPEC1" --summary-out "$SUMMARY" --dry-run >/dev/null 2>&1
[ -f "$SUMMARY" ] && fail "--summary-out should not be written on --dry-run (nothing real happened)"
[ "$FAIL" -eq 0 ] && echo "--summary-out dry-run skip OK"

echo ""
echo "=== Testing a spec file with 'config_file' (real invocation) ==="
: > "$CALLLOG"
SPEC2="$WORKDIR/spec2.json"
cat > "$SPEC2" <<EOF
{"suite": "specsuite2", "workloads": [{"name": "w1", "command": ["true"]}], "config_file": "$CONFFILE"}
EOF
OUT="$($SWEEP --spec "$SPEC2" 2>&1)"
grep -q -- "-c $CONFFILE -- true" "$CALLLOG" || fail "expected -c <file> in the real call: $(cat "$CALLLOG")"
[ "$FAIL" -eq 0 ] && echo "spec with 'config_file' OK"

echo ""
echo "=== Testing a spec with both 'profile' and 'config_file' is rejected ==="
SPEC3="$WORKDIR/spec3.json"
cat > "$SPEC3" <<EOF
{"suite": "s", "workloads": [{"name": "w1", "command": ["true"]}], "profile": "quick", "config_file": "$CONFFILE"}
EOF
if OUT="$($SWEEP --spec "$SPEC3" --dry-run 2>&1)"; then
    fail "expected a nonzero exit for a spec with both profile and config_file: $OUT"
fi
[ "$FAIL" -eq 0 ] && echo "spec with both profile+config_file rejection OK"

echo ""
echo "=== Testing a spec with neither 'profile' nor 'config_file' is rejected ==="
SPEC4="$WORKDIR/spec4.json"
cat > "$SPEC4" <<EOF
{"suite": "s", "workloads": [{"name": "w1", "command": ["true"]}]}
EOF
if OUT="$($SWEEP --spec "$SPEC4" --dry-run 2>&1)"; then
    fail "expected a nonzero exit for a spec with neither profile nor config_file: $OUT"
fi
[ "$FAIL" -eq 0 ] && echo "spec with neither profile/config_file rejection OK"

echo ""
echo "=== Testing a failing cell is reported but doesn't stop the sweep ==="
: > "$CALLLOG"
SPEC5="$WORKDIR/spec5.json"
cat > "$SPEC5" <<EOF
{"suite": "s", "workloads": [{"name": "good", "command": ["true"]}, {"name": "bad", "command": ["false", "TRIGGER_FAIL"]}],
 "profile": "quick"}
EOF
export WSPY_RUN_FAIL_ON=TRIGGER_FAIL
if OUT="$($SWEEP --spec "$SPEC5" 2>&1)"; then
    fail "expected a nonzero exit when one cell fails"
fi
unset WSPY_RUN_FAIL_ON
[ "$(wc -l < "$CALLLOG")" -ge 2 ] || fail "expected both cells to still run despite one failing: $(cat "$CALLLOG")"
echo "$OUT" | grep -qi "1/2 cell(s) failed" || fail "expected a failure summary: $OUT"
[ "$FAIL" -eq 0 ] && echo "failing-cell reporting OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-sweep smoke tests passed ==="
    exit 0
else
    echo "=== wspy-sweep smoke tests FAILED ==="
    exit 1
fi
