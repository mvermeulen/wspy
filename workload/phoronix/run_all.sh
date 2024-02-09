#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
john-the-ripper
kvazaar
lczero
BENCH_LIST
