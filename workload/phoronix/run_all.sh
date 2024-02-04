#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
apache-iotdb-perf
appleseed
avifenc
BENCH_LIST
