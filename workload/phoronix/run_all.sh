#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
dacapobench
darktable
hackbench
incompact3d
svt-hevc
vkpeak
blogbench
BENCH_LIST
