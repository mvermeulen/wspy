#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
kvazaar
lczero
tscp
webp
uvg266
BENCH_LIST
