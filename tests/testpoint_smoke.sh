#!/bin/bash
# tests/testpoint_smoke.sh - smoke tests for wspy-testpoint (INVESTIGATION.md's
# "Test-point-level curated performance-summary README deep-dive", Tier 3 item 5, piece 1: run
# selection / role-assignment).
#
# Exercises the default role-assignment heuristic (stats-pool/excluded/supplementary from
# status+pass-set), human --set/--primary override persistence across a re-run (the living-document
# guarantee), --dry-run, idempotent no-op commits, --hostname filtering, Phoronix test/test-point
# identity resolution, and error handling for an unknown --set run_id -- all against a local bare git
# repo standing in for the report-root remote, so this needs no network access and no real hardware
# counters. Run once here (no build/GPU axis), same idiom as tests/wspy_queue_smoke.sh.
#
# Usage: ./tests/testpoint_smoke.sh (run from repo root; expects ./wspy-testpoint to be present --
# a plain Python script, no build step).

set -u
cd "$(dirname "$0")/.." || exit 1

FAIL=0
fail() { echo "FAIL: $*"; FAIL=1; }

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

git init --bare -q "$WORKDIR/remote.git"
OUTROOT="$WORKDIR/outroot"
REPORTROOT="$WORKDIR/reportroot"
mkdir -p "$OUTROOT/manual/mybench"

make_run() {
    local run_id="$1" status_code="$2" passes_json="$3" day="$4" host="${5:-myhost}"
    local d="$OUTROOT/manual/mybench/$run_id"
    mkdir -p "$d"
    cat > "$d/manifest.json" <<EOF
{"suite":"manual","benchmark":"mybench","run_id":"$run_id","command":["true"],"passes":$passes_json}
EOF
    cat > "$d/counters.manifest.json" <<EOF
{"schema_version":"1.9.0","host":{"hostname":"$host","cpu_vendor":"AMD"},
 "timing":{"start_time":"2026-08-0${day}T00:00:00Z","elapsed_seconds":1.0},
 "exit_status":{"known":true,"exited":true,"exit_code":$status_code,"signaled":false}}
EOF
}

COUNTERS_PASS='{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json","status":"ok"}'
COUNTERS_FAIL='{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json","status":"failed"}'
TREE_PASS='{"name":"tree","output":"tree.txt","manifest":"tree.manifest.json","status":"ok"}'

make_run run1 0 "[$COUNTERS_PASS]" 1                    # stats-pool candidate (oldest)
make_run run2 0 "[$COUNTERS_PASS]" 2                    # stats-pool candidate, most recent -> default primary
make_run run3 1 "[$COUNTERS_FAIL]" 3                     # FAIL -> excluded
make_run run4 0 "[$COUNTERS_PASS,$TREE_PASS]" 4          # extra pass -> supplementary
make_run run5 0 "[$COUNTERS_PASS]" 5 otherhost           # different host -> not a candidate at all

RUNSJSON="$REPORTROOT/manual/mybench/default/test-machine/runs.json"

TP="./wspy-testpoint select-runs --suite manual --benchmark mybench --machine test-machine \
    --output-root $OUTROOT --report-root $REPORTROOT --report-root-remote $WORKDIR/remote.git \
    --hostname myhost"

echo "=== Testing default role-assignment heuristic ==="
OUT="$($TP 2>&1)"
echo "$OUT" | grep -q "stats-pool.*run1" || fail "run1 (matching command/pass-set/status) not stats-pool"
echo "$OUT" | grep -q "stats-pool.*run2.*\[primary\]" || fail "run2 not stats-pool+primary (most recent)"
echo "$OUT" | grep -q "excluded.*run3" || fail "run3 (FAIL status) not excluded"
echo "$OUT" | grep -q "supplementary.*run4" || fail "run4 (extra pass) not supplementary"
echo "$OUT" | grep -q "run5" && fail "run5 (different hostname) should not appear at all"
[ -f "$RUNSJSON" ] || fail "runs.json not written"
[ "$(git -C "$REPORTROOT" log --oneline | wc -l)" -eq 1 ] || fail "expected exactly one commit after first run"
[ "$FAIL" -eq 0 ] && echo "default role-assignment OK"

echo ""
echo "=== Testing --set/--primary overrides persist across a later default-heuristic re-run ==="
$TP --set run1=excluded --primary run4 >/dev/null 2>&1
OUT="$($TP 2>&1)"
echo "$OUT" | grep -q "excluded.*run1" || fail "run1 override (--set excluded) did not persist"
echo "$OUT" | grep -q "run4.*\[primary\]" || fail "run4 --primary override did not persist across a plain re-run"
echo "$OUT" | grep -q "human-set" || fail "human_set marker missing from overridden run's output"
[ "$FAIL" -eq 0 ] && echo "override persistence OK"

echo ""
echo "=== Testing idempotency: identical state produces no new commit ==="
BEFORE="$(git -C "$REPORTROOT" rev-parse HEAD)"
$TP >/dev/null 2>&1
AFTER="$(git -C "$REPORTROOT" rev-parse HEAD)"
[ "$BEFORE" = "$AFTER" ] || fail "re-running with no changes created a new commit ($BEFORE -> $AFTER)"
[ "$FAIL" -eq 0 ] && echo "idempotency OK"

echo ""
echo "=== Testing --dry-run writes nothing and commits nothing ==="
BEFORE_HASH="$(md5sum "$RUNSJSON" | cut -d' ' -f1)"
BEFORE_COMMIT="$(git -C "$REPORTROOT" rev-parse HEAD)"
$TP --set run2=excluded --dry-run >/dev/null 2>&1
AFTER_HASH="$(md5sum "$RUNSJSON" | cut -d' ' -f1)"
AFTER_COMMIT="$(git -C "$REPORTROOT" rev-parse HEAD)"
[ "$BEFORE_HASH" = "$AFTER_HASH" ] || fail "--dry-run modified runs.json on disk"
[ "$BEFORE_COMMIT" = "$AFTER_COMMIT" ] || fail "--dry-run created a commit"
[ "$FAIL" -eq 0 ] && echo "--dry-run OK"

echo ""
echo "=== Testing --json output is valid and reflects current state ==="
JSON="$($TP --json 2>/dev/null)"
echo "$JSON" | python3 -c "
import json, sys
d = json.load(sys.stdin)
assert d['schema_version'] == '1.0', d
assert d['primary_run_id'] == 'run4', d['primary_run_id']
assert d['primary_human_set'] is True, d
roles = {r['run_id']: r['role'] for r in d['runs']}
assert roles == {'run1': 'excluded', 'run2': 'stats-pool', 'run3': 'excluded', 'run4': 'supplementary'}, roles
" || fail "--json output did not match expected structure/roles"
[ "$FAIL" -eq 0 ] && echo "--json output OK"

echo ""
echo "=== Testing an unknown --set run_id errors out cleanly ==="
if $TP --set does-not-exist=excluded >/dev/null 2>&1; then
    fail "expected a nonzero exit for an unknown --set run_id"
fi
[ "$FAIL" -eq 0 ] && echo "unknown --set run_id error handling OK"

echo ""
echo "=== Testing Phoronix test/test-point identity resolution ==="
PHORONIX_DEST="$WORKDIR/phoronix_dest"
mkdir -p "$PHORONIX_DEST/mytest/myoptions"
cat > "$PHORONIX_DEST/mytest/myoptions/suite-definition.xml" <<'EOF'
<PhoronixTestSuite/>
EOF
cat > "$PHORONIX_DEST/mytest/myoptions/source.json" <<'EOF'
{"test_id": "pts/mytest-1.0.0", "arguments": "", "source_kind": "url", "source_ref": "x",
 "generated_at": "2026-08-01T00:00:00Z", "installed": true}
EOF
mkdir -p "$OUTROOT/phoronix/mytest-myoptions/prun1"
cat > "$OUTROOT/phoronix/mytest-myoptions/prun1/manifest.json" <<'EOF'
{"suite":"phoronix","benchmark":"mytest-myoptions","run_id":"prun1","command":["true"],
 "passes":[{"name":"counters","output":"counters.csv","manifest":"counters.manifest.json","status":"ok"}]}
EOF
cat > "$OUTROOT/phoronix/mytest-myoptions/prun1/counters.manifest.json" <<'EOF'
{"schema_version":"1.9.0","host":{"hostname":"myhost","cpu_vendor":"AMD"},
 "timing":{"start_time":"2026-08-01T00:00:00Z","elapsed_seconds":1.0},
 "exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false}}
EOF
OUT="$(./wspy-testpoint select-runs --suite phoronix --benchmark mytest-myoptions --machine test-machine \
    --output-root "$OUTROOT" --report-root "$REPORTROOT" --report-root-remote "$WORKDIR/remote.git" \
    --phoronix-dest-root "$PHORONIX_DEST" --hostname myhost 2>&1)"
[ -f "$REPORTROOT/phoronix/mytest/myoptions/test-machine/runs.json" ] || \
    fail "expected runs.json under phoronix/mytest/myoptions/ (test/test-point split), got: $OUT"
[ "$FAIL" -eq 0 ] && echo "Phoronix test/test-point resolution OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-testpoint smoke tests passed ==="
    exit 0
else
    echo "=== wspy-testpoint smoke tests FAILED ==="
    exit 1
fi
