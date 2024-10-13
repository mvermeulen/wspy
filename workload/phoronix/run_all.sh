#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
ffmpeg
compress-gzip
povray
pytorch
tensorflow
darktable
compress-7zip
lulesh
minibude
ebizzy
openssl
build-php
pybench
apache
indigobench
lczero
gromacs
BENCH_LIST
