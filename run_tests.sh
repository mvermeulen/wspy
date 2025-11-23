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

# AMDGPU build test (if AMDGPU support is available)
if [ -d "/opt/rocm" ]; then
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
            echo "  GPU busy in CSV: OK"
        else
            echo "FAIL: GPU busy column missing from CSV output"
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
            echo "  GPU busy in normal output: OK"
        else
            echo "FAIL: GPU busy missing from normal output"
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
