#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
easywave
embree-perf
gimp
gromacs
java-scimark2
llama-cpp-perf
openjpeg
phpbench
BENCH_LIST
