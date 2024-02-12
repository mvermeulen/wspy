#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
mpcbench
m-queens
mrbayes
namd
node-web-tooling
n-queens
octave-benchmark
openscad
perl-benchmark
povray
pybench
quantlib
rawtherapee
rbenchmark
scimark2
selenium-perf
sqlite-perf
stream
svt-av1
svt-vp9
v-ray
x264
BENCH_LIST
