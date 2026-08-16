#!/bin/bash
# tests/phoronix_batch_smoke.sh - smoke tests for wspy-phoronix-batch (INVESTIGATION.md 4.4(c)
# "wspy-run-profile-driven batchable equivalent of the single-test-point Phoronix suite flow").
#
# Real materialized test points (joblib.materialize_phoronix_test_point(), the same function every
# other Phoronix import path already uses), isolated via PTS_USER_PATH (never the real
# ~/.phoronix-test-suite -- this project's own hard-learned lesson from testing this exact area
# before). Exercises point resolution (--point/--points-file/--all, an unknown identity skipped with
# a warning rather than aborting the batch), a real dry-run integration check against the actual
# wspy-sweep binary (proving the two tools' spec-JSON interface really matches, not just that a fake
# stand-in accepts it), and a full run against a fake wspy-sweep (local-suite-copy preparation,
# --summary-out consumption, runs/ symlink-back into each test point's own directory) plus
# --no-link-back/--dry-run both correctly skipping that last step.
#
# Usage: ./tests/phoronix_batch_smoke.sh (run from repo root; expects ./wspy-phoronix-batch and
# ./wspy-sweep to be present -- both plain Python scripts, no build step).

set -u
cd "$(dirname "$0")/.." || exit 1

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

export PTS_USER_PATH="$WORKDIR/ptsdata"
DEST="$WORKDIR/dest"

# Materializes two real test points via the exact same function every other Phoronix import path
# already uses, plus one on-disk-but-not-in-our-batch point (to prove --all picks up everything and
# --point only picks up what's asked for).
python3 -c "
import sys
sys.path.insert(0, 'web')
import joblib
joblib.materialize_phoronix_test_point(
    {'test_id': 'pts/testa-1.0.0', 'arguments': 'argsA'}, '$DEST', 'file', '/tmp/src.xml')
joblib.materialize_phoronix_test_point(
    {'test_id': 'pts/testb-1.0.0', 'arguments': 'argsB'}, '$DEST', 'file', '/tmp/src.xml')
joblib.materialize_phoronix_test_point(
    {'test_id': 'pts/testc-1.0.0', 'arguments': 'argsC'}, '$DEST', 'file', '/tmp/src.xml')
"

echo ""
echo "=== Testing --point with an unknown identity is skipped, not fatal ==="
OUT="$(./wspy-phoronix-batch --point testa-argsa --point totally-unknown-xyz \
    --dest "$DEST" --profile quick --dry-run --outroot "$WORKDIR/out" 2>&1)"
echo "$OUT" | grep -q "no materialized test point matching 'totally-unknown-xyz'" || \
    fail "expected a skip warning for the unknown identity: $OUT"
echo "$OUT" | grep -q "1 test point(s): testa-argsa" || fail "expected the known point to still run: $OUT"
[ "$FAIL" -eq 0 ] && echo "unknown-identity skip OK"

echo ""
echo "=== Testing --dry-run never copies a local suite ==="
LOCALDIR="$PTS_USER_PATH/test-suites/local"
rm -rf "$LOCALDIR"
./wspy-phoronix-batch --point testa-argsa --dest "$DEST" --profile quick --dry-run \
    --outroot "$WORKDIR/out" >/dev/null 2>&1
[ -d "$LOCALDIR/testa-argsa" ] && fail "--dry-run must not copy anything into test-suites/local/"
[ "$FAIL" -eq 0 ] && echo "--dry-run no-side-effects OK"

echo ""
echo "=== Testing --all batches every materialized point, --point batches only what's named ==="
OUT="$(./wspy-phoronix-batch --all --dest "$DEST" --profile quick --dry-run --outroot "$WORKDIR/out" 2>&1)"
echo "$OUT" | grep -q "3 test point(s)" || fail "expected --all to find all 3 materialized points: $OUT"
OUT="$(./wspy-phoronix-batch --point testb-argsb --dest "$DEST" --profile quick --dry-run --outroot "$WORKDIR/out" 2>&1)"
echo "$OUT" | grep -q "1 test point(s): testb-argsb" || fail "expected --point to find only testb-argsb: $OUT"
[ "$FAIL" -eq 0 ] && echo "--all vs --point scoping OK"

echo ""
echo "=== Testing --points-file (comments/blank lines ignored) ==="
POINTSFILE="$WORKDIR/points.txt"
cat > "$POINTSFILE" <<'EOF'
# a comment line
testa-argsa

testc-argsc
EOF
OUT="$(./wspy-phoronix-batch --points-file "$POINTSFILE" --dest "$DEST" --profile quick --dry-run --outroot "$WORKDIR/out" 2>&1)"
echo "$OUT" | grep -q "testa-argsa, testc-argsc" || fail "expected both points-file identities: $OUT"
[ "$FAIL" -eq 0 ] && echo "--points-file OK"

echo ""
echo "=== Testing real wspy-sweep integration (--dry-run, proves the spec-JSON interface matches) ==="
if ! OUT="$(./wspy-phoronix-batch --all --dest "$DEST" --profile deep-cpu --dry-run --outroot "$WORKDIR/out" 2>&1)"; then
    fail "real wspy-sweep --dry-run integration failed: $OUT"
fi
echo "$OUT" | grep -q "batch-run local/testa-argsa" || fail "expected testa's batch-run command in wspy-sweep's own dry-run output: $OUT"
echo "$OUT" | grep -q "deep-cpu --" || fail "expected the --profile to reach wspy-sweep's own dry-run output: $OUT"
[ "$FAIL" -eq 0 ] && echo "real wspy-sweep --dry-run integration OK"

echo ""
echo "=== Testing --profile and --config together propagates wspy-sweep's own rejection ==="
CONFFILE="$WORKDIR/my-passes.conf"
echo "counters --topdown" > "$CONFFILE"
if ./wspy-phoronix-batch --all --dest "$DEST" --profile quick -c "$CONFFILE" --dry-run --outroot "$WORKDIR/out" >/dev/null 2>&1; then
    fail "expected a nonzero exit when both --profile and --config are given"
fi
[ "$FAIL" -eq 0 ] && echo "--profile + --config rejection OK"

echo ""
echo "=== Testing a real (non-dry-run) batch against a fake wspy-sweep: local-suite copy + link-back ==="
FAKEBIN="$WORKDIR/fakebin"
mkdir -p "$FAKEBIN"
cat > "$FAKEBIN/wspy-sweep" <<'PYEOF'
#!/usr/bin/env python3
# Fake wspy-sweep: reads the real spec JSON wspy-phoronix-batch built, "runs" each workload by just
# creating its rundir on disk, and writes a --summary-out JSON matching the real tool's own shape --
# enough for wspy-phoronix-batch's own summary-consumption/link-back logic to be exercised for real,
# without depending on wspy-run/wspy actually working.
import json, os, sys
args = sys.argv[1:]
spec_path = args[args.index("--spec") + 1]
outroot = args[args.index("-o") + 1]
summary_path = args[args.index("--summary-out") + 1] if "--summary-out" in args else None
with open(spec_path) as f:
    spec = json.load(f)
summary = []
for i, wl in enumerate(spec["workloads"]):
    run_id = "fake-run-%03d" % i
    rundir = os.path.join(outroot, spec["suite"], wl["name"], run_id)
    os.makedirs(rundir, exist_ok=True)
    summary.append({"cell_id": wl["name"], "workload_name": wl["name"], "run_id": run_id,
                     "rundir": rundir, "exit_code": 0})
if summary_path:
    with open(summary_path, "w") as f:
        json.dump(summary, f)
sys.exit(0)
PYEOF
chmod +x "$FAKEBIN/wspy-sweep"

OUTROOT2="$WORKDIR/out2"
OUT="$(./wspy-phoronix-batch --point testa-argsa --point testb-argsb --dest "$DEST" --profile quick \
    --outroot "$OUTROOT2" --wspy-sweep-bin "$FAKEBIN/wspy-sweep" 2>&1)"
[ -f "$LOCALDIR/testa-argsa/suite-definition.xml" ] || fail "expected testa's local suite copy to exist: $OUT"
[ -f "$LOCALDIR/testb-argsb/suite-definition.xml" ] || fail "expected testb's local suite copy to exist: $OUT"

TESTA_LINK="$DEST/testa/argsa/runs/fake-run-000"
[ -L "$TESTA_LINK" ] || fail "expected testa's run to be linked back at $TESTA_LINK: $OUT"
[ "$(readlink -f "$TESTA_LINK")" = "$(readlink -f "$OUTROOT2/phoronix/testa-argsa/fake-run-000")" ] || \
    fail "testa's runs/ symlink doesn't point at the real rundir"
echo "$OUT" | grep -q "linked testa-argsa" || fail "expected a link-back confirmation message: $OUT"
[ "$FAIL" -eq 0 ] && echo "real batch: local-suite copy + link-back OK"

echo ""
echo "=== Testing --no-link-back skips the symlink step ==="
rm -rf "$DEST/testc"
python3 -c "
import sys
sys.path.insert(0, 'web')
import joblib
joblib.materialize_phoronix_test_point(
    {'test_id': 'pts/testc-1.0.0', 'arguments': 'argsC'}, '$DEST', 'file', '/tmp/src.xml')
"
./wspy-phoronix-batch --point testc-argsc --dest "$DEST" --profile quick --no-link-back \
    --outroot "$WORKDIR/out3" --wspy-sweep-bin "$FAKEBIN/wspy-sweep" >/dev/null 2>&1
[ -d "$DEST/testc/argsc/runs" ] && fail "--no-link-back should not have created a runs/ directory at all"
[ "$FAIL" -eq 0 ] && echo "--no-link-back OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-phoronix-batch smoke tests passed ==="
    exit 0
else
    echo "=== wspy-phoronix-batch smoke tests FAILED ==="
    exit 1
fi
