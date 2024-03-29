#!/bin/bash
gnuplot <<PLOTCMD
set terminal png size 1280,960
set output 'amdtopdown.png'
set title 'Topdown metrics'
set datafile separator ','
set key autotitle columnhead
plot 'amdtopdown.csv' using 1:2, 'amdtopdown.csv' using 1:3, 'amdtopdown.csv' using 1:4, 'amdtopdown.csv' using 1:5
PLOTCMD

if [ -f systemtime.csv ]; then
    gnuplot <<PLOTCMD2
set terminal png size 1280,960
set output 'systemtime.png'                                                     
set title 'Time Overview'                                                     
set datafile separator ','                                                      
set key autotitle columnhead
unset ytics
set y2tics nomirror                                                  
plot 'systemtime.csv' using 1:4 with lines, \
   'systemtime.csv'   using 1:5 with lines, \
   'systemtime.csv'   using 1:6 with lines, \
   'systemtime.csv'   using 1:7 with lines, \
   'systemtime.csv'   using 1:3 axis x1y2 lc "blue"
PLOTCMD2

fi
