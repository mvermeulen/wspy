#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
speedb
stargate
stockfish
svt-hevc
tnn
uvg266
vkpeak
wireguard
BENCH_LIST
