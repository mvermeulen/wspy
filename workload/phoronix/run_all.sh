#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
hackbench
libxsmm
BENCH_LIST
