#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
neatbench
litert
openvino-genai
rustls
BENCH_LIST
