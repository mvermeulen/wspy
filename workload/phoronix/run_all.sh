#!/bin/bash
while read bench
do
    TESTNAME=$bench ./run_test.sh
done <<-BENCH_LIST
compress-gzip
darktable
dav1d
dolfyn
encode-wavpack
glibc-bench
gmpbench
gnupg
himeno
hmmer
incompact3d
libreoffice
lulesh
mnn
tensorflow
BENCH_LIST
