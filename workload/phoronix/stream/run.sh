#!/bin/bash
if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    /home/mev/source/wspy/topdown -o software.branch.txt --rusage --software --no-ipc --branch phoronix-test-suite batch-run stream
    /home/mev/source/wspy/topdown -o topdownx.txt --topdown2 --no-rusage --no-software --no-ipc phoronix-test-suite batch-run stream
    /home/mev/source/wspy/topdown -o ipc.l2.txt --ipc --cache2 --no-software --no-rusage phoronix-test-suite batch-run stream
    cat software.branch.txt topdownx.txt ipc.l2.txt > topdown.txt
else
    /home/mev/source/wspy/topdown -o software.branch.txt --rusage --software --branch --no-ipc phoronix-test-suite batch-run stream
    /home/mev/source/wspy/topdown -o ipc.topdown.txt --ipc --topdown2 --no-rusage --no-software phoronix-test-suite batch-run stream
    /home/mev/source/wspy/topdown -o l2.float.txt --cache2 --float --no-software --no-ipc --no-rusage phoronix-test-suite batch-run stream
    cat software.branch.txt ipc.topdown.txt l2.float.txt > topdown.txt
fi
    

    




