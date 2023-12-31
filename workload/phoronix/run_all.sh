#!/bin/bash
while read bench
do
    cd $bench
    ./run.sh
    cd ..
done <<-BENCH_LIST
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
tensorflow
tensorflow-lite
stream
coremark
build-gcc
BENCH_LIST
