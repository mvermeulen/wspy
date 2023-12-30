#!/bin/bash
while read bench
do
    cd $bench
    ./run.sh
    cd ..
done <<-BENCH_LIST
stream
coremark
build-gcc
BENCH_LIST
