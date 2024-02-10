#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
hpcg-perf
primesieve
mt-dgemm
spark-tpcds
BENCH_LIST
