#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
mpcbench
n-queens
m-queens
BENCH_LIST
