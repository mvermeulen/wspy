#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
cp2k
gcrypt
ipc-benchmark
BENCH_LIST
