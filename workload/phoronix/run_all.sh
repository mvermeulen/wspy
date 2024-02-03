#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
askap
asmfish
astcenc
basis
blake2
blosc
bork
botan
brl-cad
BENCH_LIST
