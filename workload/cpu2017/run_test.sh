#!/bin/bash
#
# Run a cpu2017 test
TESTNAME=${TESTNAME:="503.bwaves_r"}
SPECDIR=${SPECDIR:="/home/mev/cpu2017"}
SPECCONFIG=${SPECCONFIG:="mev-aocc-7840.cfg"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}

if [ ! -d $TESTNAME ]; then
    mkdir $TESTNAME
fi

cd $TESTNAME
wspy_logdir=`pwd`

pushd $SPECDIR
source shrc
ulimit -s unlimited
popd

# build
runcpu --config ${SPECCONFIG} --action=build --tune base ${TESTNAME} 2>&1 | tee amd.${TESTNAME}.build.out

if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    echo "Intel not supported"
    exit 0
else
    $WSPY -o systemtime.csv --csv --interval 1 --no-rusage --no-software --system --no-ipc runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME 2>&1 | tee amd.${TESTNAME}.out    
    $WSPY -o software.branch.txt --rusage --software --branch --no-ipc runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME 2>&1 | tee amd.${TESTNAME}.out2
    $WSPY -o ipc.topdown.txt --ipc --topdown2 --no-rusage --no-software runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
    $WSPY -o l2.float.txt --cache2 --float --no-rusage --no-software runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
    $WSPY -o amdtopdown.csv --csv --interval 1 --topdown --no-rusage --no-software --no-ipc runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
    $WSPY -o frontend.txt --topdown-frontend --no-ipc --no-software --no-rusage runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
    $WSPY -o opcache.txt --topdown-optlb --no-ipc --no-software --no-rusage runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME    
    timeout 3600 $WSPY -o treerun.txt --tree process.tree.txt --tree-cmdline --software --no-ipc runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME
    cat software.branch.txt ipc.topdown.txt l2.float.txt opcache.txt frontend.txt > amdtopdown.txt
    /home/mev/source/wspy/workload/phoronix/gnuplot.sh
fi
