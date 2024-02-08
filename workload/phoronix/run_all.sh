#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
graph500
helsing
hpcg-perf
mt-dgemm
primesieve
BENCH_LIST
