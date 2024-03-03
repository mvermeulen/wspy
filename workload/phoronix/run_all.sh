#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
deeprec
deepspeech
neat
BENCH_LIST
