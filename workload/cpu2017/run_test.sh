#!/bin/bash
#
# Run a cpu2017 test
TESTNAME=${TESTNAME:="503.bwaves_r"}
SPECDIR=${SPECDIR:="/home/mev/cpu2017"}
SPECCONFIG=${SPECCONFIG:="mev-aocc-7840.cfg"}

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
runcpu --config ${SPECCONFIG} --action=build --tune base ${TESTNAME}
