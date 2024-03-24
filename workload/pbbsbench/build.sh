#!/bin/bash
PBBSBENCH=${PBBSBENCH:="/home/mev/source/pbbsbench"}

cd $PBBSBENCH
./runall -small -notime
