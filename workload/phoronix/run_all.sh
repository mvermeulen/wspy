#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
liquid-dsp
lzbench
minibude
BENCH_LIST
