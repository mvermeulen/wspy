#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
perl-benchmark
phpbench
pybench
pyperformance
r-benchmark
BENCH_LIST
