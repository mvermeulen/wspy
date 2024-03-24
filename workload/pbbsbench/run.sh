#!/bin/bash
PBBSBENCH=${PBBSBENCH:="/home/mev/source/pbbsbench"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}

LOGDIR=`pwd`/`date '+%Y%m%d-%H%M'`
mkdir -p $LOGDIR

cd ${PBBSBENCH}

./runall -small 1> ${LOGDIR}/bench.out 2> ${LOGDIR}/bench.err
${WSPY} -o ${LOGDIR}/systemtime.csv --csv --interval 1 --no-rusage --no-software --system --no-ipc ./runall -small  2>&1 | tee bench.out
${WSPY} -o ${LOGDIR}/software.branch.txt --rusage --software --branch --no-ipc ./runall -small 
${WSPY} -o ${LOGDIR}/ipc.topdown.txt --ipc --topdown2 --no-rusage --no-software ./runall -small 
${WSPY} -o ${LOGDIR}/l2.float.txt --cache2 --float --no-rusage --no-software ./runall -small 
${WSPY} -o ${LOGDIR}/amdtopdown.csv --csv --interval 1 --topdown --no-rusage --no-software --no-ipc ./runall -small 
${WSPY} -o ${LOGDIR}/frontend.txt --topdown-frontend --no-ipc --no-software --no-rusage ./runall -small 
${WSPY} -o ${LOGDIR}/opcache.txt --topdown-optlb --no-ipc --no-software --no-rusage ./runall -small 
${WSPY} -o ${LOGDIR}/treerun.txt --tree process.tree.txt --tree-cmdline --software --no-ipc ./runall -small
cd ${LOGDIR}
cat software.branch.txt ipc.topdown.txt l2.float.txt opcache.txt frontend.txt > amdtopdown.txt
