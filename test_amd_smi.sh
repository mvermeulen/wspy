#!/bin/bash
# Test script for amd_smi module

set -e

echo "=== Building amd_smi ==="
make amd_smi AMDGPU=1

echo ""
echo "=== Testing amd_smi binary ==="
./amd_smi

echo ""
echo "=== Rebuilding wspy ==="
rm -f wspy
make wspy AMDGPU=1

echo ""
echo "=== Testing wspy without GPU option ==="
./wspy -- /bin/true

echo ""
echo "=== Testing wspy with GPU option ==="
./wspy --gpu-smi -- /bin/true

echo ""
echo "=== Testing wspy with GPU option (verbose) ==="
./wspy -v --gpu-smi -- /bin/true

echo ""
echo "=== All tests completed successfully ==="
