#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
x264
polyhedron
tungsten
BENCH_LIST
