#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
ospray
compress-xz
quicksilver
x265
coremark
build-eigen
build-gcc
phpbench
lzbench
compress-zstd
simdjson
perl-benchmark
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
