#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
coremark
build-gcc
build-ffmpeg
build-godot
build-llvm
compress-7zip
gimp
john-the-ripper
openscad
phpbench
pybench
pytorch
rawtherapee
stream
tensorflow
tensorflow-lite
BENCH_LIST
