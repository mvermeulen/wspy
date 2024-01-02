#!/bin/bash
gnuplot <<PLOTCMD
set terminal png size 1280,960
set output 'amdtopdown.png'
set title 'Topdown metrics'
set datafile separator ','
set key autotitle columnhead
plot 'amdtopdown.csv' using 1:2, 'amdtopdown.csv' using 1:3, 'amdtopdown.csv' using 1:4, 'amdtopdown.csv' using 1:5
PLOTCMD



