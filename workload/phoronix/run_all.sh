#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
build-apache
build-nodegs
build-php
BENCH_LIST
