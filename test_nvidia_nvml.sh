#!/bin/bash
# Test script for nvidia_nvml module
# Needs a real NVIDIA GPU + driver installed (dlopen's libnvidia-ml.so.1 at
# runtime); not wired into make test/./run_tests.sh, same as test_amd_smi.sh.

set -e

# Objects compiled with a different AMDGPU/NVIDIA combination may be lying
# around from a previous build in this tree; make's mtime-based dependency
# tracking doesn't know CFLAGS changed, so start from a clean slate.
echo "=== Cleaning ==="
make clean >/dev/null

echo "=== Building nvidia_nvml ==="
make nvidia_nvml NVIDIA=1

echo ""
echo "=== Testing nvidia_nvml binary ==="
./nvidia_nvml

echo ""
echo "=== Rebuilding wspy ==="
rm -f wspy
make wspy NVIDIA=1

echo ""
echo "=== Testing wspy without GPU option ==="
./wspy -- /bin/true

echo ""
echo "=== Testing wspy with GPU option ==="
./wspy --gpu-nvidia -- /bin/true

echo ""
echo "=== Testing wspy with GPU option (verbose) ==="
./wspy -v --gpu-nvidia -- /bin/true

echo ""
echo "=== All tests completed successfully ==="
