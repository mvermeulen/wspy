#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
deepsparse
appleseed
build-godot
BENCH_LIST
