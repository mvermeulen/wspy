#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
nginx
ngspice
openradioss
pgbench-perf
BENCH_LIST
