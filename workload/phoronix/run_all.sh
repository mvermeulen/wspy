#!/bin/bash
while read bench
do
    cd $bench
    TESTNAME=$bench ./run.sh
    cd ..
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
