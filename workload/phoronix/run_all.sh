#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
gmpbench
gnupg
himeno
libreoffice
scimark2
BENCH_LIST
