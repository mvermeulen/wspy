#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
furmark-perf
pjsip
quadray
schbench
synthmark
go-benchmark
BENCH_LIST
