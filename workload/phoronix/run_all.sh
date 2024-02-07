#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
build-nodejs
build-php
compress-7zip
c-ray
duckdb
openfoam-perf
BENCH_LIST
