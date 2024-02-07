#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
compress-lz4
compress-zstd
encode-flac
encode-mp3
encode-opus
BENCH_LIST
