#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
apache
build-mplayer
compress-7zip
compress-rar
coremark
crafty
dacapobench
BENCH_LIST
