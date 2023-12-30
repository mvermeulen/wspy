#!/bin/bash
/home/mev/source/wspy/topdown -o software.branch.txt --rusage --software --no-ipc --branch phoronix-test-suite batch-run stream
/home/mev/source/wspy/topdown -o ipc.topdown.txt --ipc  --topdown2 --no-rusage --no-software phoronix-test-suite batch-run stream
/home/mev/source/wspy/topdown -o l2.float.txt --no-software --no-ipc --no-rusage --cache2 --float phoronix-test-suite batch-run stream

cat software.branch.txt ipc.topdown.txt l2.float.txt > topdown.txt
