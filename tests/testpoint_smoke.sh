#!/bin/bash
# tests/testpoint_smoke.sh - smoke tests for wspy-testpoint (INVESTIGATION.md's
# "Test-point-level curated performance-summary README deep-dive", Tier 3 item 5, pieces 1-2: run
# selection / role-assignment, and aggregation).
#
# Piece 1 (select-runs): exercises the default role-assignment heuristic (stats-pool/excluded/
# supplementary from status+pass-set), human --set/--primary override persistence across a re-run (the
# living-document guarantee), --dry-run, idempotent no-op commits, --hostname filtering, Phoronix
# test/test-point identity resolution, and error handling for an unknown --set run_id.
#
# Piece 2 (aggregate): exercises the core correctness property --run-id filtering (summary.c) exists
# for -- a redo sharing byte-identical command text with the runs it's redoing must never poison the
# reported statistics -- plus the missing-run warn-but-proceed path and clean errors for an absent
# runs.json/store.
#
# All against a local bare git repo standing in for the report-root remote, so this needs no network
# access and no real hardware counters. Run once here (no build/GPU axis), same idiom as
# tests/wspy_queue_smoke.sh.
#
# Usage: ./tests/testpoint_smoke.sh (run from repo root; expects ./wspy-testpoint to be present --
# a plain Python script, no build step -- and builds ./wspy-summary/./wspy-store for the aggregate
# section below).

set -u
cd "$(dirname "$0")/.." || exit 1

echo "=== Building wspy-summary/wspy-store ==="
make wspy-summary wspy-store >/dev/null || { echo "FAIL: could not build wspy-summary/wspy-store"; exit 1; }

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
echo "=== Testing aggregate: a redo sharing identical command text must not poison the mean ==="
STOREDB="$WORKDIR/store.db"
AGGDATA="$WORKDIR/aggdata"
mkdir -p "$AGGDATA"
echo "ipc" > "$AGGDATA/run1.csv"; echo "1.80" >> "$AGGDATA/run1.csv"
echo "ipc" > "$AGGDATA/run2.csv"; echo "1.90" >> "$AGGDATA/run2.csv"
echo "ipc" > "$AGGDATA/run3-redo.csv"; echo "0.50" >> "$AGGDATA/run3-redo.csv"

build_index_record() {
    local run_id="$1" start="$2" csvfile="$3"
    cat <<EOF
{"schema_version":"1.9.0","run_id":"$run_id","collector":"wspy","wspy_version":"4.0","hostname":"agghost","cpu_vendor":"AMD","cpu_family":25,"cpu_model":1,"environment":{"virt_role":"host","hypervisor_vendor":null,"microcode_version":null,"bios_vendor":null,"bios_version":null,"bios_date":null,"cpu_governor":null,"cpu_scaling_driver":null,"cpu_governor_uniform":false,"memory_total_kb":null,"compiler_version":null,"libc_version":null},"environment_coverage":{"captured":0,"probed":9},"start_time":"$start","finish_time":"$start","elapsed_seconds":1.0,"command":["true"],"exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false,"term_signal":null},"options":{"counter_mask":"0x1","per_core":false,"system":true,"csv":true,"tree":false,"interval_seconds":0},"counter_coverage":{"requested":4,"measured":4},"output_files":{"output_path":"$csvfile","tree_output_path":null,"manifest_path":null}}
EOF
}
{
    build_index_record aggrun1 "2026-08-01T00:00:00Z" "$AGGDATA/run1.csv"
    build_index_record aggrun2 "2026-08-02T00:00:00Z" "$AGGDATA/run2.csv"
    build_index_record aggrun3-redo "2026-08-03T00:00:00Z" "$AGGDATA/run3-redo.csv"
} > "$AGGDATA/run_index.jsonl"
./wspy-store --db "$STOREDB" --run-index "$AGGDATA/run_index.jsonl" >/dev/null || \
    fail "wspy-store ingestion failed"

AGG_TP_DIR="$REPORTROOT/manual/aggbench/default/agg-machine"
mkdir -p "$AGG_TP_DIR"
cat > "$AGG_TP_DIR/runs.json" <<'EOF'
{
  "schema_version": "1.0",
  "suite": "manual", "test": "aggbench", "test_point": "default", "machine": "agg-machine",
  "primary_run_id": "aggrun2", "primary_human_set": false,
  "runs": [
    {"run_id": "aggrun1", "benchmark": "aggbench", "hostname": "agghost", "status": "ok", "command": "true", "start_time": "2026-08-01T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"},
    {"run_id": "aggrun2", "benchmark": "aggbench", "hostname": "agghost", "status": "ok", "command": "true", "start_time": "2026-08-02T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"},
    {"run_id": "aggrun3-redo", "benchmark": "aggbench", "hostname": "agghost", "status": "failed", "command": "true", "start_time": "2026-08-03T00:00:00Z", "role": "excluded", "human_set": false, "reason": "redo, superseded"}
  ]
}
EOF

AGG="./wspy-testpoint aggregate --suite manual --benchmark aggbench --machine agg-machine \
    --report-root $REPORTROOT --db $STOREDB"

OUT="$($AGG --csv 2>&1)"
echo "$OUT" | grep -qE '^true,ipc,2,1\.8,1\.9,1\.85,' || \
    fail "expected n=2 mean=1.85 (redo's 0.5 excluded), got: $OUT"
echo "$OUT" | grep -q "0.5" && fail "the redo's ipc=0.5 leaked into aggregate output: $OUT"
[ "$FAIL" -eq 0 ] && echo "aggregate redo-exclusion OK"

echo ""
echo "=== Testing aggregate: a stats-pool run missing from the store warns but still proceeds ==="
python3 -c "
import json
p = '$AGG_TP_DIR/runs.json'
d = json.load(open(p))
d['runs'].append({'run_id': 'aggrun4-not-ingested', 'benchmark': 'aggbench', 'hostname': 'agghost',
                   'status': 'ok', 'command': 'true', 'start_time': '2026-08-04T00:00:00Z',
                   'role': 'stats-pool', 'human_set': False, 'reason': 'test'})
json.dump(d, open(p, 'w'), indent=2)
"
OUT="$($AGG --csv 2>&1)"
echo "$OUT" | grep -q "aggrun4-not-ingested" || fail "expected a warning naming the missing run, got: $OUT"
echo "$OUT" | grep -qE '^true,ipc,2,1\.8,1\.9,1\.85,' || \
    fail "expected aggregate to still proceed with the 2 present runs, got: $OUT"
[ "$FAIL" -eq 0 ] && echo "missing-run warn-but-proceed OK"

echo ""
echo "=== Testing aggregate: no runs.json present errors cleanly ==="
if ./wspy-testpoint aggregate --suite manual --benchmark does-not-exist --machine agg-machine \
    --report-root "$REPORTROOT" --db "$STOREDB" >/dev/null 2>&1; then
    fail "expected a nonzero exit when runs.json doesn't exist"
fi
[ "$FAIL" -eq 0 ] && echo "missing runs.json error handling OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-testpoint smoke tests passed ==="
    exit 0
else
    echo "=== wspy-testpoint smoke tests FAILED ==="
    exit 1
fi
