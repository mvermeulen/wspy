#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
draco
srsran
compress-pbzip2
BENCH_LIST
