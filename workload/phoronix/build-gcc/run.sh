#!/bin/bash
if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    /home/mev/source/wspy/topdown -o software.branch.txt --rusage --software --no-ipc --branch phoronix-test-suite batch-run build-gcc 2>&1 | tee build-gcc.out
    /home/mev/source/wspy/topdown -o topdownx.txt --topdown2 --no-rusage --no-software --no-ipc phoronix-test-suite batch-run build-gcc
    /home/mev/source/wspy/topdown -o ipc.l2.txt --ipc --cache2 --no-software --no-rusage phoronix-test-suite batch-run build-gcc
    cat software.branch.txt topdownx.txt ipc.l2.txt > inteltopdown.txt
else
    /home/mev/source/wspy/topdown -o software.branch.txt --rusage --software --branch --no-ipc phoronix-test-suite batch-run build-gcc 2>&1 | tee build-gcc.out
    /home/mev/source/wspy/topdown -o ipc.topdown.txt --ipc --topdown2 --no-rusage --no-software phoronix-test-suite batch-run build-gcc
    /home/mev/source/wspy/topdown -o l2.float.txt --cache2 --float --no-software --no-ipc --no-rusage phoronix-test-suite batch-run build-gcc
    /home/mev/source/wspy/topdown -o amdtopdown.csv --csv --interval 1 --topdown --no-ipc --no-software --no-rusage phoronix-test-suite batch-run build-gcc
    /home/mev/source/wspy/topdown -o treerun.txt --tree process.tree.txt --software --no-ipc phoronix-test-suite batch-run build-gcc 2>&1 | tee build-gcc-tree.out
    cat software.branch.txt ipc.topdown.txt l2.float.txt > amdtopdown.txt
    ./gnuplot.sh
fi
    

    




