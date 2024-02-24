#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
clickhouse
openvkl
pennant
pyhpc-perf
renaissance
rocksdb
spark
BENCH_LIST
