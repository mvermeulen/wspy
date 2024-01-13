#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
encode-flac
encode-mp3
encode-opus
encode-wavpack
BENCH_LIST
