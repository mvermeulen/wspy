#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
cpuminer-opt
cryptopp
ctx-clock
cyclictest
daphne
BENCH_LIST
