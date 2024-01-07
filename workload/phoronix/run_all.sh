#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
compress-7zip
compress-gzip
compress-lz4
compress-rar
compress-xz
compress-zstd
BENCH_LIST
