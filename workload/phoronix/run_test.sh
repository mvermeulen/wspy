#!/bin/bash
#
# Run a phoronix test suite test..
TESTNAME=${TESTNAME:="coremark"}

if [ ! -d $TESTNAME ]; then
    mkdir $TESTNAME
fi

cd $TESTNAME
if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    /home/mev/source/wspy/wspy -o software.branch.txt --rusage --software --branch --no-ipc phoronix-test-suite batch-run $TESTNAME 2>&1 | tee intel.${TESTNAME}.out
    /home/mev/source/wspy/wspy -o topdown.txt --topdown2 --no-rusage --no-software --no-ipc phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o ipc.l2.txt --ipc --cache2 --no-rusage --no-software phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o backend.txt --no-ipc --no-rusage --topdown-backend phoronix-test-suite batch-run $TESTNAME
    cat software.branch.txt topdown.txt ipc.l2.txt backend.txt > inteltopdown.txt
else
    /home/mev/source/wspy/wspy -o systemtime.csv --csv --interval 1 --no-rusage --no-software --system --no-ipc phoronix-test-suite batch-run $TESTNAME 2>&1 | tee amd.${TESTNAME}.out    
    /home/mev/source/wspy/wspy -o software.branch.txt --rusage --software --branch --no-ipc phoronix-test-suite batch-run $TESTNAME 2>&1 | tee amd.${TESTNAME}.out2
    /home/mev/source/wspy/wspy -o ipc.topdown.txt --ipc --topdown2 --no-rusage --no-software phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o l2.float.txt --cache2 --float --no-rusage --no-software phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o amdtopdown.csv --csv --interval 1 --topdown --no-rusage --no-software --no-ipc phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o frontend.txt --topdown-frontend --no-ipc --no-software --no-rusage phoronix-test-suite batch-run $TESTNAME
    /home/mev/source/wspy/wspy -o opcache.txt --topdown-optlb --no-ipc --no-software --no-rusage phoronix-test-suite batch-run $TESTNAME    
    timeout 3600 /home/mev/source/wspy/wspy -o treerun.txt --tree process.tree.txt --tree-cmdline --software --no-ipc phoronix-test-suite batch-run $TESTNAME
    cat software.branch.txt ipc.topdown.txt l2.float.txt opcache.txt frontend.txt > amdtopdown.txt
    /home/mev/source/wspy/workload/phoronix/gnuplot.sh
fi
cd ..
