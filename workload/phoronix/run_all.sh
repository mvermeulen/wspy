#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
sudokut
vpxenc
aircrack-ng
BENCH_LIST
