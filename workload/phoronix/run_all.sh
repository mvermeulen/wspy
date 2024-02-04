#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
build-clash
build-eigen
build-erlang
build-gdb
build-gem5
build-imagemagick
build-mesa
build-python
build-wasmer
build2
bullet
byte
plaidml
BENCH_LIST
