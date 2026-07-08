#!/bin/bash
# run_tests.sh - Run unit and integration tests

set -e

echo "=== Building Tests ==="
make clean > /dev/null 2>&1 || true
make test

echo ""
echo "=== Building wspy and proctree ==="
make wspy proctree

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
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,gpu_busy,ipc"; then
            echo "  Standalone GPU busy CSV column order: OK"
        else
            echo "FAIL: GPU busy column not in expected position (after stime)"
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
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,gpu_temp,gpu_activity,gpu_power,gpu_freq,ipc"; then
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
        if echo "$OUTPUT" | grep -q "elapsed,utime,stime,gpu_temp,gpu_activity,gpu_power,gpu_freq,ipc"; then
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
