#!/bin/bash
# run_tests.sh - Run unit and integration tests

set -e

echo "=== Building Tests ==="
make clean > /dev/null 2>&1 || true
make test

echo ""
echo "=== Building wspy and proctree ==="
make wspy proctree wspy-validate wspy-ledger

echo ""
echo "=== Running Integration Tests ==="

# Basic run
echo "Testing wspy basic run..."
./wspy --no-ipc -- /bin/true

# CSV output
echo "Testing wspy CSV output..."
./wspy --no-ipc --csv -- /bin/true > test_output.csv
if [ ! -s test_output.csv ]; then
    echo "FAIL: CSV output is empty"
    exit 1
fi
rm test_output.csv

# Tree output
echo "Testing wspy tree output..."
./wspy --no-ipc --tree test_tree.out -- /bin/ls > /dev/null
if [ ! -s test_tree.out ]; then
    echo "FAIL: Tree output is empty"
    exit 1
fi

# Proctree test
echo "Testing proctree on generated tree..."
./proctree test_tree.out > test_proctree.out
if [ ! -s test_proctree.out ]; then
    echo "FAIL: Proctree output is empty"
    exit 1
fi
rm test_tree.out test_proctree.out

# Manifest output
echo "Testing wspy --manifest output..."
./wspy --no-ipc --csv -o test_manifest_out.csv --manifest test_manifest.json -- /bin/true > /dev/null
if [ ! -s test_manifest.json ]; then
    echo "FAIL: manifest output is empty"
    exit 1
fi
if command -v python3 > /dev/null 2>&1; then
    if ! python3 -m json.tool test_manifest.json > /dev/null; then
        echo "FAIL: manifest is not valid JSON"
        exit 1
    fi
fi
for expected in \
    '"schema_version": "1.2.0"' \
    '"wspy_version"' \
    '"argv": \["/bin/true"\]' \
    '"kind": "output"' \
    '"path": "test_manifest_out.csv"' \
    '"kind": "manifest"' \
    '"counter_coverage": {' \
    '"requested": 0' \
    '"measured": 0' \
    '"environment": {' \
    '"virt_role":' \
    '"environment_coverage": {' \
    '"probed": 9'; do
    if ! grep -q "$expected" test_manifest.json; then
        echo "FAIL: manifest missing expected content: $expected"
        exit 1
    fi
done
echo "  manifest output: OK"
rm test_manifest_out.csv test_manifest.json

# wspy-validate: basic pre-publish quality checks against a manifest
echo "Testing wspy-validate on a clean run..."
./wspy --no-ipc --csv -o test_validate_out.csv --manifest test_validate.manifest.json -- /bin/true > /dev/null
if ! ./wspy-validate test_validate.manifest.json > test_validate.out; then
    echo "FAIL: wspy-validate should exit 0 on a clean run"
    cat test_validate.out
    exit 1
fi
if ! grep -q "^test_validate.manifest.json: PASS$" test_validate.out; then
    echo "FAIL: wspy-validate did not report PASS for a clean run"
    cat test_validate.out
    exit 1
fi
if ! grep -q "1 manifest(s) checked: 1 passed, 0 warned, 0 failed" test_validate.out; then
    echo "FAIL: wspy-validate summary line did not match a clean single-manifest run"
    cat test_validate.out
    exit 1
fi
rm test_validate_out.csv test_validate.manifest.json test_validate.out
echo "  wspy-validate clean run: OK"

echo "Testing wspy-validate catches a missing required file..."
./wspy --no-ipc --csv -o test_validate_missing.csv --manifest test_validate_missing.manifest.json -- /bin/true > /dev/null
rm test_validate_missing.csv
if ./wspy-validate test_validate_missing.manifest.json > test_validate.out; then
    echo "FAIL: wspy-validate should exit non-zero when a required output file is missing"
    cat test_validate.out
    exit 1
fi
if ! grep -q "required file missing" test_validate.out; then
    echo "FAIL: wspy-validate did not report the missing output file"
    cat test_validate.out
    exit 1
fi
rm test_validate_missing.manifest.json test_validate.out
echo "  wspy-validate missing file detection: OK"

echo "Testing wspy-validate catches a nonzero workload exit status..."
./wspy --no-ipc --csv -o test_validate_fail.csv --manifest test_validate_fail.manifest.json -- /bin/false > /dev/null || true
if ./wspy-validate test_validate_fail.manifest.json > test_validate.out; then
    echo "FAIL: wspy-validate should exit non-zero when the workload exited non-zero"
    cat test_validate.out
    exit 1
fi
if ! grep -q "exited with nonzero status" test_validate.out; then
    echo "FAIL: wspy-validate did not report the nonzero exit status"
    cat test_validate.out
    exit 1
fi
rm test_validate_fail.csv test_validate_fail.manifest.json test_validate.out
echo "  wspy-validate nonzero exit detection: OK"

# Run index output
echo "Testing wspy --run-index output..."
rm -f test_run_index.jsonl
./wspy --no-ipc --run-index test_run_index.jsonl -- /bin/true > /dev/null
./wspy --no-ipc --run-index test_run_index.jsonl -- /bin/false > /dev/null || true
if [ ! -s test_run_index.jsonl ]; then
    echo "FAIL: run index output is empty"
    exit 1
fi
NUM_LINES=$(wc -l < test_run_index.jsonl)
if [ "$NUM_LINES" -ne 2 ]; then
    echo "FAIL: run index should have 2 lines (one append per run), got $NUM_LINES"
    exit 1
fi
if command -v python3 > /dev/null 2>&1; then
    if ! python3 -c "
import json
with open('test_run_index.jsonl') as f:
    lines = [l for l in f if l.strip()]
assert len(lines) == 2, 'expected 2 records'
records = [json.loads(l) for l in lines]
assert records[0]['run_id'] != records[1]['run_id'], 'run_id must be distinct per run'
assert records[0]['exit_status']['exit_code'] == 0
assert records[1]['exit_status']['exit_code'] == 1
"; then
        echo "FAIL: run index records are not valid/distinct JSONL"
        exit 1
    fi
fi
for expected in \
    '"schema_version":"1.2.0"' \
    '"wspy_version"' \
    '"command":\["/bin/true"\]' \
    '"counter_coverage":{"requested":0,"measured":0}' \
    '"environment":{"virt_role":' \
    '"environment_coverage":{"captured":'; do
    if ! grep -q "$expected" test_run_index.jsonl; then
        echo "FAIL: run index missing expected content: $expected"
        exit 1
    fi
done
echo "  run index output: OK"
rm test_run_index.jsonl

# wspy-ledger: coverage ledger generated from a run index
echo "Testing wspy-ledger on a mix of done/skipped/needs-tool-support/unsupported workloads..."
rm -f test_ledger_index.jsonl
./wspy --no-ipc --run-index test_ledger_index.jsonl -- /bin/echo done-workload > /dev/null
./wspy --no-ipc --run-index test_ledger_index.jsonl -- /bin/sh -c 'exit 1' -- needs-tool-support-workload > /dev/null || true
cat > test_ledger_list.txt <<'LEDGER_LIST'
done-workload
needs-tool-support-workload
skipped-workload
unsupported-workload	unsupported	docker based
LEDGER_LIST
if ! ./wspy-ledger --run-index test_ledger_index.jsonl test_ledger_list.txt > test_ledger.out; then
    echo "FAIL: wspy-ledger should exit 0 without --strict"
    cat test_ledger.out
    exit 1
fi
for expected in \
    'done-workload +done' \
    'needs-tool-support-workload +needs-tool-support' \
    'skipped-workload +skipped' \
    'unsupported-workload +unsupported +docker based' \
    '4 workload\(s\): 1 done, 1 skipped, 1 unsupported, 1 needs-tool-support'; do
    if ! grep -qE "$expected" test_ledger.out; then
        echo "FAIL: wspy-ledger output missing expected content: $expected"
        cat test_ledger.out
        exit 1
    fi
done
if ./wspy-ledger --run-index test_ledger_index.jsonl --strict test_ledger_list.txt > /dev/null; then
    echo "FAIL: wspy-ledger --strict should exit non-zero when workloads are skipped/needs-tool-support"
    exit 1
fi
CSV_OUT=$(./wspy-ledger --run-index test_ledger_index.jsonl --csv test_ledger_list.txt)
if ! echo "$CSV_OUT" | head -1 | grep -q '^name,status,runs_matched,runs_succeeded,last_run_id,last_start_time,note$'; then
    echo "FAIL: wspy-ledger --csv header did not match"
    echo "$CSV_OUT"
    exit 1
fi
rm test_ledger_index.jsonl test_ledger_list.txt test_ledger.out
echo "  wspy-ledger: OK"

# Counter capability discovery + coverage reporting
echo "Testing wspy --capabilities (no workload command needed)..."
CAPS_OUT=$(./wspy --capabilities 2>&1)
if ! echo "$CAPS_OUT" | grep -q "^counter capability report: "; then
    echo "FAIL: --capabilities did not print a capability report"
    exit 1
fi
if ./wspy --capabilities > /dev/null 2>&1; then
    :
else
    echo "FAIL: --capabilities should exit 0 even without perf access"
    exit 1
fi
echo "  wspy --capabilities: OK"

# AMD IBS capability probing (--capabilities also reports ibs_fetch/ibs_op;
# gracefully "not supported" on non-AMD hosts or kernels without IBS, so this
# only checks the report is present and well-formed, not that IBS itself is
# available on whatever machine runs this).
echo "Testing wspy --capabilities IBS probing..."
if ! echo "$CAPS_OUT" | grep -qE "^IBS capability report: (supported|not supported)$"; then
    echo "FAIL: --capabilities did not print an IBS capability report line"
    echo "$CAPS_OUT"
    exit 1
fi
if ! echo "$CAPS_OUT" | grep -qE "^  ibs_fetch +(available|not present)"; then
    echo "FAIL: --capabilities did not report ibs_fetch status"
    echo "$CAPS_OUT"
    exit 1
fi
if ! echo "$CAPS_OUT" | grep -qE "^  ibs_op +(available|not present)"; then
    echo "FAIL: --capabilities did not report ibs_op status"
    echo "$CAPS_OUT"
    exit 1
fi
echo "  wspy --capabilities IBS probing: OK"

# ibs-basic/ibs-memory-deep collection profiles: gracefully "no IBS columns"
# on a host/kernel without IBS support (mirrors the GPU-flag pattern above),
# and CSV header/value column counts must match on hosts that do have it.
echo "Testing wspy --ibs-basic / --ibs-memory-deep..."
IBS_BASIC_OUT=$(./wspy --csv --ibs-basic -- /bin/true 2>&1)
if ! echo "$IBS_BASIC_OUT" | grep -q "^elapsed,"; then
    echo "FAIL: --ibs-basic did not produce CSV output"
    echo "$IBS_BASIC_OUT"
    exit 1
fi
if echo "$CAPS_OUT" | grep -q "^IBS capability report: supported"; then
    if ! echo "$IBS_BASIC_OUT" | grep -q "ibs_fetch,ibs_op,"; then
        echo "FAIL: --ibs-basic did not add ibs_fetch/ibs_op CSV columns on an IBS-supported host"
        echo "$IBS_BASIC_OUT"
        exit 1
    fi
    IBS_HEADER_COLS=$(echo "$IBS_BASIC_OUT" | grep "^elapsed," | head -1 | tr ',' '\n' | wc -l)
    IBS_VALUE_COLS=$(echo "$IBS_BASIC_OUT" | grep -v "^elapsed," | grep -v "^warning:" | grep -v "^error:" | tail -1 | tr ',' '\n' | wc -l)
    if [ "$IBS_HEADER_COLS" != "$IBS_VALUE_COLS" ]; then
        echo "FAIL: --ibs-basic CSV header/value column count mismatch ($IBS_HEADER_COLS vs $IBS_VALUE_COLS)"
        echo "$IBS_BASIC_OUT"
        exit 1
    fi
else
    if echo "$IBS_BASIC_OUT" | grep -q "ibs_fetch,ibs_op,"; then
        echo "FAIL: --ibs-basic added IBS columns on a host --capabilities reports as unsupported"
        exit 1
    fi
fi
IBS_DEEP_OUT=$(./wspy --ibs-memory-deep -- /bin/true 2>&1)
if [ $? -ne 0 ]; then
    echo "FAIL: --ibs-memory-deep should exit 0 even without perf access / IBS support"
    echo "$IBS_DEEP_OUT"
    exit 1
fi
echo "  wspy --ibs-basic / --ibs-memory-deep: OK"

# Counters that fail to open (e.g. no perf access) must degrade gracefully
# rather than aborting the whole run -- this is the other half of coverage
# reporting: a real run's output says what it could and couldn't measure
# instead of refusing to produce output at all.
echo "Testing wspy graceful degradation when counters are unavailable..."
# set -e means this line itself already asserts a zero exit status --
# previously any single unopenable counter (e.g. no perf access) was fatal.
DEGRADE_OUT=$(./wspy --csv --topdown -- /bin/true)
if ! echo "$DEGRADE_OUT" | grep -q "counters_measured,counters_requested"; then
    echo "FAIL: CSV output missing counter coverage columns"
    exit 1
fi
echo "  graceful degradation on unavailable counters: OK"

# wspy-run profile launcher
echo "Testing wspy-run --list..."
if ! ./wspy-run --list | grep -q "^quick:"; then
    echo "FAIL: wspy-run --list did not show the quick profile"
    exit 1
fi
if ! ./wspy-run --list | grep -q "^deep-cpu:"; then
    echo "FAIL: wspy-run --list did not show the deep-cpu profile"
    exit 1
fi
echo "  wspy-run --list: OK"

echo "Testing wspy-run --dry-run..."
DRYOUT=$(./wspy-run --dry-run -o test_wspy_run_dry --wspy ./wspy quick -- /bin/true)
if ! echo "$DRYOUT" | grep -q -- "--ipc --system --rusage"; then
    echo "FAIL: wspy-run --dry-run did not print the expected quick-profile flags"
    exit 1
fi
if [ -e test_wspy_run_dry ]; then
    echo "FAIL: wspy-run --dry-run should not create the output directory"
    exit 1
fi
echo "  wspy-run --dry-run: OK"

# Real execution of a builtin profile is not exercised here: every builtin
# profile enables at least one hardware/software counter (quick uses --ipc,
# tree-heavy uses --software, ...), which requires perf permissions this
# suite otherwise avoids depending on (see the --no-ipc-only tests above and
# below). --dry-run above already validates builtin profile flag assembly;
# the --config test below validates real pass execution mechanics using
# --no-ipc-only passes that need no special privileges.
echo "Testing wspy-run --config with --manifest-dir and --run-index..."
rm -rf test_wspy_run_cfg test_wspy_run_manifests
cat > test_wspy_run.conf <<'EOF'
# comment line, should be ignored
noop --no-ipc
csvpass --no-ipc --csv
EOF
./wspy-run --wspy ./wspy -o test_wspy_run_cfg -c test_wspy_run.conf \
    --manifest-dir test_wspy_run_manifests \
    --run-index test_wspy_run_index.jsonl \
    -- /bin/true > /dev/null
if [ ! -s test_wspy_run_cfg/noop.txt ]; then
    echo "FAIL: wspy-run config pass 'noop' did not produce noop.txt"
    exit 1
fi
if [ ! -s test_wspy_run_cfg/csvpass.csv ]; then
    echo "FAIL: wspy-run config pass 'csvpass' did not produce csvpass.csv (--csv should select .csv extension)"
    exit 1
fi
if [ ! -s test_wspy_run_manifests/noop.manifest.json ] || [ ! -s test_wspy_run_manifests/csvpass.manifest.json ]; then
    echo "FAIL: wspy-run --manifest-dir did not produce a manifest per pass"
    exit 1
fi
NUM_INDEX_LINES=$(wc -l < test_wspy_run_index.jsonl)
if [ "$NUM_INDEX_LINES" -ne 2 ]; then
    echo "FAIL: wspy-run --run-index should have one line per pass (2 passes), got $NUM_INDEX_LINES"
    exit 1
fi
rm -rf test_wspy_run_cfg test_wspy_run_manifests test_wspy_run_index.jsonl test_wspy_run.conf
echo "  wspy-run --config with --manifest-dir/--run-index: OK"

echo "Testing wspy-run error handling (unknown profile, missing workload)..."
if ./wspy-run bogus-profile -- /bin/true 2>&1 | grep -q "unknown profile"; then
    echo "  wspy-run unknown profile error: OK"
else
    echo "FAIL: wspy-run should reject an unknown profile name"
    exit 1
fi
if ./wspy-run quick 2>&1 | grep -q "missing workload command"; then
    echo "  wspy-run missing workload error: OK"
else
    echo "FAIL: wspy-run should reject a missing workload command"
    exit 1
fi

# Tree stress + integrity test
echo "Testing wspy tree stress and integrity counters..."
STRESS_PROCS="${WSPY_TREE_STRESS_PROCS:-2000}"
./wspy --no-ipc --tree test_tree_stress.out -- /bin/sh -c 'n="$1"; i=0; while [ "$i" -lt "$n" ]; do /bin/true & i=$((i+1)); done; wait' sh "$STRESS_PROCS" > /dev/null

# Ensure proctree can still reconstruct output from stress run
./proctree test_tree_stress.out > /dev/null

if ! awk '
BEGIN {
    root_count = 0;
    fork_lines = 0;
    exit_lines = 0;
    unknown_lines = 0;
    saw_summary = 0;
    errors = 0;
}

/^#/ {
    if ($2 == "ptrace-summary") {
        saw_summary = 1;
        for (i = 3; i <= NF; i++) {
            split($i, kv, "=");
            summary[kv[1]] = kv[2] + 0;
        }
    }
    next;
}

{
    if (NF < 3) next;

    event = $3;
    pid = $2 + 0;

    if (event == "root") {
        root_count++;
        known[pid] = 1;
        next;
    }

    if (event == "fork") {
        child = $4 + 0;
        fork_lines++;
        known[pid] = 1;
        known[child] = 1;

        if (pid <= 0 || child <= 0 || pid == child) {
            printf("invalid fork edge parent=%d child=%d\n", pid, child) > "/dev/stderr";
            errors++;
            next;
        }

        if ((child in parent_of) && parent_of[child] != pid) {
            printf("multiple parents for pid %d: %d and %d\n", child, parent_of[child], pid) > "/dev/stderr";
            errors++;
            next;
        }
        parent_of[child] = pid;
        next;
    }

    if (event == "exit") {
        exit_lines++;
        if (!(pid in known)) {
            printf("exit for unknown pid %d\n", pid) > "/dev/stderr";
            errors++;
        }
        next;
    }

    if (event == "unknown") {
        unknown_lines++;
        next;
    }
}

END {
    if (root_count != 1) {
        printf("expected exactly one root event, got %d\n", root_count) > "/dev/stderr";
        errors++;
    }

    if (!saw_summary) {
        printf("missing ptrace summary footer\n") > "/dev/stderr";
        errors++;
    } else {
        if (summary["fork_events"] != fork_lines) {
            printf("fork counter mismatch: footer=%d lines=%d\n", summary["fork_events"], fork_lines) > "/dev/stderr";
            errors++;
        }
        if (summary["exit_events"] != exit_lines) {
            printf("exit counter mismatch: footer=%d lines=%d\n", summary["exit_events"], exit_lines) > "/dev/stderr";
            errors++;
        }
        if (summary["unknown_traps"] != unknown_lines) {
            printf("unknown trap mismatch: footer=%d lines=%d\n", summary["unknown_traps"], unknown_lines) > "/dev/stderr";
            errors++;
        }
        if (!("wait_eintr" in summary)) {
            printf("wait_eintr missing from footer\n") > "/dev/stderr";
            errors++;
        }
    }

    if (errors > 0) exit 1;
}
' test_tree_stress.out; then
    echo "FAIL: Tree stress integrity check failed"
    exit 1
fi

rm test_tree_stress.out
echo "  tree stress integrity: OK"

# Network interface name test (ensure no '(null)' appears)
echo "Testing system network interface names (non-AMDGPU build)..."
if ./wspy --system --no-ipc -- /bin/true 2>&1 | grep -q '(null)'; then
    echo "FAIL: Found (null) network interface name"
    exit 1
else
    echo "  network interface names: OK"
fi

# GPU option warnings when built without AMDGPU
echo "Testing GPU option warnings (non-AMDGPU build)..."
if ./wspy --gpu-busy -- /bin/true 2>&1 | grep -q "GPU support not built"; then
    echo "  --gpu-busy warning: OK"
else
    echo "FAIL: --gpu-busy should warn when built without AMDGPU"
    exit 1
fi

if ./wspy --gpu-metrics -- /bin/true 2>&1 | grep -q "GPU support not built"; then
    echo "  --gpu-metrics warning: OK"
else
    echo "FAIL: --gpu-metrics should warn when built without AMDGPU"
    exit 1
fi

if ./wspy --gpu-smi -- /bin/true 2>&1 | grep -q "GPU support not built"; then
    echo "  --gpu-smi warning: OK"
else
    echo "FAIL: --gpu-smi should warn when built without AMDGPU"
    exit 1
fi

# AMDGPU build test (if AMDGPU support is available). ROCm was historically
# only installed under /opt/rocm, but distro packages (e.g. Debian/Ubuntu's
# rocm-smi-lib) may instead put amd_smi/amdsmi.h under /usr, so check both.
if [ -e "/opt/rocm/include/amd_smi/amdsmi.h" ] || [ -e "/usr/include/amd_smi/amdsmi.h" ]; then
    echo "Testing AMDGPU build..."
    make clean > /dev/null 2>&1 || true
    if make AMDGPU=1 > /dev/null 2>&1; then
        echo "  AMDGPU build: OK"
        
        # Verify GPU options work without warnings
        if ./wspy -v --gpu-busy -- sleep 0.05 2>&1 | grep -q "initial gpu busy percent"; then
            echo "  --gpu-busy functional: OK"
        else
            echo "WARNING: --gpu-busy did not show expected debug output"
        fi
        
        # Test GPU busy in system metrics (CSV)
        echo "Testing GPU busy in system CSV output..."
        if ./wspy --system --gpu-busy --csv -- sleep 0.1 2>&1 | grep -q "gpu_busy"; then
            echo "  GPU busy in system CSV: OK"
        else
            echo "FAIL: GPU busy column missing from system CSV output"
            exit 1
        fi

        # Test standalone GPU busy (CSV)
        echo "Testing standalone GPU busy CSV output..."
        OUTPUT=$(./wspy --gpu-busy --csv -- sleep 0.1 2>&1 | head -1)
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,gpu_busy,ipc"; then
            echo "  Standalone GPU busy CSV column order: OK"
        else
            echo "FAIL: GPU busy column not in expected position (after rusage columns)"
            echo "Got: $OUTPUT"
            exit 1
        fi

        # Test standalone GPU busy (normal output)
        if ./wspy --gpu-busy -- sleep 0.1 2>&1 | grep -q "gpu busy"; then
            echo "  Standalone GPU busy in normal output: OK"
        else
            echo "FAIL: Standalone GPU busy missing from normal output"
            exit 1
        fi
        
        # Test standalone GPU metrics (normal output)
        echo "Testing standalone GPU metrics output..."
        OUTPUT=$(./wspy --gpu-metrics -- sleep 0.1 2>&1)
        if echo "$OUTPUT" | grep -q "gpu temp" && echo "$OUTPUT" | grep -q "gpu activity" && echo "$OUTPUT" | grep -q "gpu power" && echo "$OUTPUT" | grep -q "gpu freq"; then
            echo "  GPU metrics in normal output: OK"
        else
            echo "FAIL: GPU metrics missing from normal output"
            exit 1
        fi
        
        # Test standalone GPU metrics (CSV)
        OUTPUT=$(./wspy --gpu-metrics --csv -- sleep 0.1 2>&1 | head -1)
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,gpu_temp,gpu_activity,gpu_power,gpu_freq,ipc"; then
            echo "  GPU metrics CSV column order: OK"
        else
            echo "FAIL: GPU metrics columns not in expected position"
            echo "Got: $OUTPUT"
            exit 1
        fi
        
        # Test combined GPU busy and metrics
        OUTPUT=$(./wspy --gpu-busy --gpu-metrics -- sleep 0.1 2>&1)
        if echo "$OUTPUT" | grep -q "gpu busy" && echo "$OUTPUT" | grep -q "gpu temp" && echo "$OUTPUT" | grep -q "gpu activity"; then
            echo "  Combined GPU busy and metrics: OK"
        else
            echo "FAIL: Combined GPU options not working"
            exit 1
        fi

        # Network interface names under AMDGPU build (should also be valid)
        echo "Testing system network interface names (AMDGPU build)..."
        if ./wspy --system --no-ipc -- /bin/true 2>&1 | grep -q '(null)'; then
            echo "FAIL: Found (null) network interface name (AMDGPU build)"
            exit 1
        else
            echo "  network interface names (AMDGPU build): OK"
        fi
        
        # Test GPU busy in system metrics (normal output)
        if ./wspy --system --gpu-busy -- sleep 0.1 2>&1 | grep -q "gpu busy"; then
            echo "  GPU busy in system normal output: OK"
        else
            echo "FAIL: GPU busy missing from system normal output"
            exit 1
        fi
        
        # Test GPU busy with interval mode
        if ./wspy --system --gpu-busy --csv --interval 1 -- sleep 2 2>&1 | grep -q "gpu_busy"; then
            echo "  GPU busy in interval mode: OK"
        else
            echo "FAIL: GPU busy missing from interval output"
            exit 1
        fi
        
        # Verify GPU metrics are NOT shown without --gpu-busy
        if ./wspy --system --csv -- sleep 0.1 2>&1 | grep -q "gpu_busy"; then
            echo "FAIL: GPU busy should not appear without --gpu-busy flag"
            exit 1
        else
            echo "  GPU busy gating: OK"
        fi
        
        # Test standalone GPU metrics (normal output)
        echo "Testing standalone GPU metrics output..."
        OUTPUT=$(./wspy --gpu-metrics -- sleep 0.1 2>&1)
        if echo "$OUTPUT" | grep -q "gpu temp" && echo "$OUTPUT" | grep -q "gpu activity" && echo "$OUTPUT" | grep -q "gpu power" && echo "$OUTPUT" | grep -q "gpu freq"; then
            echo "  GPU metrics in normal output: OK"
        else
            echo "FAIL: GPU metrics missing from normal output"
            exit 1
        fi
        
        # Test standalone GPU metrics (CSV)
        OUTPUT=$(./wspy --gpu-metrics --csv -- sleep 0.1 2>&1 | head -1)
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,gpu_temp,gpu_activity,gpu_power,gpu_freq,ipc"; then
            echo "  GPU metrics CSV column order: OK"
        else
            echo "FAIL: GPU metrics columns not in expected position"
            echo "Got: $OUTPUT"
            exit 1
        fi
        
        # Test combined GPU busy and metrics
        OUTPUT=$(./wspy --gpu-busy --gpu-metrics -- sleep 0.1 2>&1)
        if echo "$OUTPUT" | grep -q "gpu busy" && echo "$OUTPUT" | grep -q "gpu temp" && echo "$OUTPUT" | grep -q "gpu activity"; then
            echo "  Combined GPU busy and metrics: OK"
        else
            echo "FAIL: Combined GPU options not working"
            exit 1
        fi
        
        # Test GPU metrics values are numeric
        OUTPUT=$(./wspy --gpu-metrics --csv -- sleep 0.1 2>&1 | tail -1)
        TEMP=$(echo "$OUTPUT" | cut -d',' -f4)
        ACTIVITY=$(echo "$OUTPUT" | cut -d',' -f5)
        POWER=$(echo "$OUTPUT" | cut -d',' -f6)
        FREQ=$(echo "$OUTPUT" | cut -d',' -f7)
        if [ "$TEMP" -ge 0 ] 2>/dev/null && [ "$ACTIVITY" -ge 0 ] 2>/dev/null && [ "$FREQ" -ge 0 ] 2>/dev/null; then
            echo "  GPU metrics values numeric: OK"
        else
            echo "FAIL: GPU metrics values not numeric (temp=$TEMP, activity=$ACTIVITY, power=$POWER, freq=$FREQ)"
            exit 1
        fi
        
        # Clean and rebuild standard version
        make clean > /dev/null 2>&1 || true
        make > /dev/null 2>&1
    else
        echo "WARNING: AMDGPU build failed (ROCm may not be properly installed)"
    fi
else
    echo "SKIP: ROCm not found, skipping AMDGPU build test"
fi

echo ""
echo "=== All tests completed successfully ==="
