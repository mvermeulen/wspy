#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
z3
x265
webp
vvenc
tscp
tensorflow
onednn
numpy
BENCH_LIST
