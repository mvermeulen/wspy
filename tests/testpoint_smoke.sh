#!/bin/bash
# tests/testpoint_smoke.sh - smoke tests for wspy-testpoint (INVESTIGATION.md's
# "Test-point-level curated performance-summary README deep-dive", Tier 3 item 5, pieces 1-4 plus the
# archetype cross-run stability fast-follow: run selection / role-assignment, aggregation, README
# rendering, and workload-characterization agreement across the stats-pool).
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
# Pieces 3-4 (render): carries the same redo-exclusion correctness proof through to the rendered
# README.md, plus the WARN-verdict callout, the supplementary-runs listing, living-document preservation
# of a hand-edited block's title/commentary across a re-render, idempotent no-op commits (this is where
# a real git-message-parsing bug was found and fixed -- commit_paths() now checks the index directly
# rather than string-matching git commit's own output, which varies with unrelated repo state), and
# --dry-run.
#
# Archetype stability fast-follow: real topdown (retire/frontend/backend/speculate) CSV fixtures drive
# real wspy-archetype --run resource_dominance classifications (compute-bound/memory-bound), exercising
# both the "Consistent" verdict (two runs agreeing) and the "Diverges" verdict (one run's classification
# changed via a second wspy-store ingestion of the same run_id with different CSV data).
#
# All against a local bare git repo standing in for the report-root remote, so this needs no network
# access and no real hardware counters. Run once here (no build/GPU axis), same idiom as
# tests/wspy_queue_smoke.sh.
#
# Usage: ./tests/testpoint_smoke.sh (run from repo root; expects ./wspy-testpoint to be present --
# a plain Python script, no build step -- and builds ./wspy-summary/./wspy-store/./wspy-archetype for
# the aggregate/render sections below).

set -u
cd "$(dirname "$0")/.." || exit 1

echo "=== Building wspy-summary/wspy-store/wspy-archetype ==="
make wspy-summary wspy-store wspy-archetype >/dev/null || {
    echo "FAIL: could not build wspy-summary/wspy-store/wspy-archetype"; exit 1; }

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
echo "=== Testing aggregate: a real wspy-run multi-pass run resolves via directory, not run_id equality ==="
# A run collected by wspy-run's own --suite/--benchmark layout never has its directory name (runs.json's
# run_id, wspy-run's own naming) equal to any pass's own run-index run_id -- each pass is a separate wspy
# invocation with its own independently-generated id (run_index.c). Regression coverage for the bug this
# fixed: aggregate must resolve a stats-pool entry to *all* of its passes' real store rows by directory
# path, not by treating the directory name itself as a store run_id.
MPDIR="$OUTROOT/manual/multipassbench/wraprun1"
mkdir -p "$MPDIR"
echo "ipc" > "$MPDIR/counters.csv"; echo "2.00" >> "$MPDIR/counters.csv"
echo "cpu_pct" > "$MPDIR/systemtime.csv"; echo "55.0" >> "$MPDIR/systemtime.csv"
build_pass_record() {
    local run_id="$1" csvfile="$2"
    cat <<EOF
{"schema_version":"1.9.0","run_id":"$run_id","collector":"wspy","wspy_version":"4.0","hostname":"mphost","cpu_vendor":"AMD","cpu_family":25,"cpu_model":1,"environment":{"virt_role":"host","hypervisor_vendor":null,"microcode_version":null,"bios_vendor":null,"bios_version":null,"bios_date":null,"cpu_governor":null,"cpu_scaling_driver":null,"cpu_governor_uniform":false,"memory_total_kb":null,"compiler_version":null,"libc_version":null},"environment_coverage":{"captured":0,"probed":9},"start_time":"2026-08-01T00:00:00Z","finish_time":"2026-08-01T00:00:00Z","elapsed_seconds":1.0,"command":["true"],"exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false,"term_signal":null},"options":{"counter_mask":"0x1","per_core":false,"system":true,"csv":true,"tree":false,"interval_seconds":0},"counter_coverage":{"requested":4,"measured":4},"output_files":{"output_path":"$csvfile","tree_output_path":null,"manifest_path":null}}
EOF
}
{
    build_pass_record wraprun1-pass-counters "$MPDIR/counters.csv"
    build_pass_record wraprun1-pass-systemtime "$MPDIR/systemtime.csv"
} > "$WORKDIR/multipass_run_index.jsonl"
./wspy-store --db "$STOREDB" --run-index "$WORKDIR/multipass_run_index.jsonl" >/dev/null || \
    fail "wspy-store ingestion (multi-pass fixture) failed"

MP_TP_DIR="$REPORTROOT/manual/multipassbench/default/mp-machine"
mkdir -p "$MP_TP_DIR"
cat > "$MP_TP_DIR/runs.json" <<'EOF'
{
  "schema_version": "1.0",
  "suite": "manual", "test": "multipassbench", "test_point": "default", "machine": "mp-machine",
  "primary_run_id": "wraprun1", "primary_human_set": false,
  "runs": [
    {"run_id": "wraprun1", "benchmark": "multipassbench", "hostname": "mphost", "status": "ok", "command": "true", "start_time": "2026-08-01T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"}
  ]
}
EOF

OUT="$(./wspy-testpoint aggregate --suite manual --benchmark multipassbench --machine mp-machine \
    --report-root "$REPORTROOT" --db "$STOREDB" --csv 2>&1)"
echo "$OUT" | grep -q "not found in" && fail "multi-pass run wrongly reported not found in store: $OUT"
echo "$OUT" | grep -qE '^true,ipc,1,2(\.0+)?,2(\.0+)?,2(\.0+)?,' || \
    fail "expected the counters pass's ipc=2.00 in aggregate output, got: $OUT"
echo "$OUT" | grep -qE '^true,cpu_pct,1,55' || \
    fail "expected the systemtime pass's cpu_pct=55.0 in aggregate output, got: $OUT"
[ "$FAIL" -eq 0 ] && echo "multi-pass directory resolution OK"

echo ""
echo "=== Testing render: README.md/curation.json content, correctness carried through from aggregate ==="
REN_TP_DIR="$REPORTROOT/manual/renderbench/default/render-machine"
mkdir -p "$REN_TP_DIR"
# Reuses aggrun1/aggrun2/aggrun3-redo, already ingested into $STOREDB above -- role assignment is a
# per-test-point decision, not a property of the run itself, so the same store rows can back a second,
# unrelated test point's runs.json. aggrun4-supp is deliberately never ingested: a supplementary run is
# listed by metadata only, never looked up in the store.
cat > "$REN_TP_DIR/runs.json" <<'EOF'
{
  "schema_version": "1.0",
  "suite": "manual", "test": "renderbench", "test_point": "default", "machine": "render-machine",
  "primary_run_id": "aggrun2", "primary_human_set": false,
  "runs": [
    {"run_id": "aggrun1", "benchmark": "renderbench", "hostname": "agghost", "status": "ok", "command": "true", "start_time": "2026-08-01T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"},
    {"run_id": "aggrun2", "benchmark": "renderbench", "hostname": "agghost", "status": "ok", "command": "true", "start_time": "2026-08-02T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"},
    {"run_id": "aggrun3-redo", "benchmark": "renderbench", "hostname": "agghost", "status": "failed", "command": "true", "start_time": "2026-08-03T00:00:00Z", "role": "excluded", "human_set": false, "reason": "redo, superseded"},
    {"run_id": "aggrun4-supp", "benchmark": "renderbench", "hostname": "agghost", "status": "ok", "command": "true", "start_time": "2026-08-04T00:00:00Z", "role": "supplementary", "human_set": false, "reason": "extra tree pass"}
  ]
}
EOF

REN="./wspy-testpoint render --suite manual --benchmark renderbench --machine render-machine \
    --report-root $REPORTROOT --report-root-remote $WORKDIR/remote.git --db $STOREDB"

$REN >/dev/null 2>&1
[ -f "$REN_TP_DIR/README.md" ] || fail "render did not write README.md"
[ -f "$REN_TP_DIR/curation.json" ] || fail "render did not write curation.json"
grep -q "| ipc | 2 | 1.8 | 1.9 | 1.85 |" "$REN_TP_DIR/README.md" || \
    fail "README.md missing expected n=2 mean=1.85 ipc row"
grep -q "0.5" "$REN_TP_DIR/README.md" && fail "the redo's ipc=0.5 leaked into the rendered README"
grep -q "WARN:thin" "$REN_TP_DIR/README.md" || fail "expected a WARN:thin callout for the n=2 bucket"
grep -q "aggrun4-supp" "$REN_TP_DIR/README.md" || fail "expected the supplementary run listed by name"
python3 -c "
import json
d = json.load(open('$REN_TP_DIR/curation.json'))
assert d['schema_version'] == '1.1', d
files = {b['source_file'] for b in d['blocks']}
assert 'sections/ipc.md' in files, files
assert 'updated' not in d and 'created' not in d, 'timestamp fields would defeat idempotent commits'
" || fail "curation.json did not match expected shape"
[ "$FAIL" -eq 0 ] && echo "render content/correctness OK"

echo ""
echo "=== Testing render: living document -- a hand-edited block survives re-render ==="
python3 -c "
import json
p = '$REN_TP_DIR/curation.json'
d = json.load(open(p))
for b in d['blocks']:
    if b['source_file'] == 'sections/ipc.md':
        b['title'] = 'IPC (human-retitled)'
        b['commentary'] = 'Human note: stable across both runs.'
json.dump(d, open(p, 'w'), indent=2)
"
$REN >/dev/null 2>&1
grep -q "IPC (human-retitled)" "$REN_TP_DIR/README.md" || fail "hand-edited title did not survive re-render"
grep -q "Human note: stable across both runs." "$REN_TP_DIR/README.md" || \
    fail "hand-edited commentary did not survive re-render"
[ "$FAIL" -eq 0 ] && echo "render living-document preservation OK"

echo ""
echo "=== Testing render: idempotent -- no new commit when nothing changed ==="
BEFORE="$(git -C "$REPORTROOT" rev-parse HEAD)"
$REN >/dev/null 2>&1
AFTER="$(git -C "$REPORTROOT" rev-parse HEAD)"
[ "$BEFORE" = "$AFTER" ] || fail "re-rendering with no changes created a new commit ($BEFORE -> $AFTER)"
[ "$FAIL" -eq 0 ] && echo "render idempotency OK"

echo ""
echo "=== Testing render --dry-run writes nothing ==="
[ -f "$REN_TP_DIR/README.md" ] && BEFORE_HASH="$(md5sum "$REN_TP_DIR/README.md" | cut -d' ' -f1)"
$REN --dry-run >/dev/null 2>&1
AFTER_HASH="$(md5sum "$REN_TP_DIR/README.md" | cut -d' ' -f1)"
[ "$BEFORE_HASH" = "$AFTER_HASH" ] || fail "render --dry-run modified README.md on disk"
[ "$FAIL" -eq 0 ] && echo "render --dry-run OK"

echo ""
echo "=== Testing render: archetype workload-characterization section (consistent case) ==="
ARCHDATA="$WORKDIR/archdata"
mkdir -p "$ARCHDATA"
echo "retire,frontend,backend,speculate" > "$ARCHDATA/computeish1.csv"; echo "70,10,15,5" >> "$ARCHDATA/computeish1.csv"
echo "retire,frontend,backend,speculate" > "$ARCHDATA/computeish2.csv"; echo "68,12,15,5" >> "$ARCHDATA/computeish2.csv"
echo "retire,frontend,backend,speculate" > "$ARCHDATA/memoryish.csv"; echo "15,10,70,5" >> "$ARCHDATA/memoryish.csv"

build_topdown_record() {
    local run_id="$1" csvfile="$2"
    cat <<EOF
{"schema_version":"1.9.0","run_id":"$run_id","collector":"wspy","wspy_version":"4.0","hostname":"archhost","cpu_vendor":"AMD","cpu_family":25,"cpu_model":1,"environment":{"virt_role":"host","hypervisor_vendor":null,"microcode_version":null,"bios_vendor":null,"bios_version":null,"bios_date":null,"cpu_governor":null,"cpu_scaling_driver":null,"cpu_governor_uniform":false,"memory_total_kb":null,"compiler_version":null,"libc_version":null},"environment_coverage":{"captured":0,"probed":9},"start_time":"2026-08-01T00:00:00Z","finish_time":"2026-08-01T00:00:00Z","elapsed_seconds":1.0,"command":["true"],"exit_status":{"known":true,"exited":true,"exit_code":0,"signaled":false,"term_signal":null},"options":{"counter_mask":"0x1","per_core":false,"system":true,"csv":true,"tree":false,"interval_seconds":0},"counter_coverage":{"requested":4,"measured":4},"output_files":{"output_path":"$csvfile","tree_output_path":null,"manifest_path":null}}
EOF
}
{
    build_topdown_record archrun1 "$ARCHDATA/computeish1.csv"
    build_topdown_record archrun2 "$ARCHDATA/computeish2.csv"
} > "$ARCHDATA/run_index.jsonl"
./wspy-store --db "$STOREDB" --run-index "$ARCHDATA/run_index.jsonl" >/dev/null || \
    fail "wspy-store ingestion (archetype fixture) failed"

ARCH_TP_DIR="$REPORTROOT/manual/archbench/default/arch-machine"
mkdir -p "$ARCH_TP_DIR"
cat > "$ARCH_TP_DIR/runs.json" <<'EOF'
{
  "schema_version": "1.0",
  "suite": "manual", "test": "archbench", "test_point": "default", "machine": "arch-machine",
  "primary_run_id": "archrun1", "primary_human_set": false,
  "runs": [
    {"run_id": "archrun1", "benchmark": "archbench", "hostname": "archhost", "status": "ok", "command": "true", "start_time": "2026-08-01T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"},
    {"run_id": "archrun2", "benchmark": "archbench", "hostname": "archhost", "status": "ok", "command": "true", "start_time": "2026-08-02T00:00:00Z", "role": "stats-pool", "human_set": false, "reason": "test"}
  ]
}
EOF

ARCH_REN="./wspy-testpoint render --suite manual --benchmark archbench --machine arch-machine \
    --report-root $REPORTROOT --report-root-remote $WORKDIR/remote.git --db $STOREDB"

$ARCH_REN >/dev/null 2>&1
grep -q "## Workload characterization" "$ARCH_TP_DIR/README.md" || \
    fail "expected a Workload characterization section"
grep -q "compute-bound" "$ARCH_TP_DIR/README.md" || fail "expected compute-bound in the characterization table"
grep -q "\*\*Consistent\*\*" "$ARCH_TP_DIR/README.md" || \
    fail "expected a Consistent verdict when both runs agree"
[ "$FAIL" -eq 0 ] && echo "archetype consistent-case OK"

echo ""
echo "=== Testing render: archetype workload-characterization section (diverging case) ==="
{
    build_topdown_record archrun1 "$ARCHDATA/computeish1.csv"
    build_topdown_record archrun2 "$ARCHDATA/memoryish.csv"
} > "$ARCHDATA/run_index2.jsonl"
./wspy-store --db "$STOREDB" --run-index "$ARCHDATA/run_index2.jsonl" >/dev/null || \
    fail "wspy-store re-ingestion (archetype diverge fixture) failed"
$ARCH_REN >/dev/null 2>&1
grep -q "\*\*Diverges\*\*" "$ARCH_TP_DIR/README.md" || \
    fail "expected a Diverges verdict once the two runs disagree on resource_dominance"
grep -q "memory-bound" "$ARCH_TP_DIR/README.md" || fail "expected memory-bound in the characterization table"
[ "$FAIL" -eq 0 ] && echo "archetype diverging-case OK"

echo ""
if [ "$FAIL" -eq 0 ]; then
    echo "=== All wspy-testpoint smoke tests passed ==="
    exit 0
else
    echo "=== wspy-testpoint smoke tests FAILED ==="
    exit 1
fi
