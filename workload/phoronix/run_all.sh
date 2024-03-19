#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
gpaw
minife
qmcpack
toybrot
webp2
BENCH_LIST
