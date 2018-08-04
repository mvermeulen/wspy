#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'meminfo.png'
set datafile separator ","
set title 'Memory'
plot 'meminfo.csv' using 1:2 title 'Total' with linespoints, 'meminfo.csv' using 1:3 title 'Free' with linespoints, 'meminfo.csv' using 1:4 title 'Buffers' with linespoints, 'meminfo.csv' using 1:5 title 'Cached' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memfree.png'
set datafile separator ","
set title 'Memory'
plot 'meminfo.csv' using 1:2 title 'Total' with linespoints, 'meminfo.csv' using 1:3 title 'Free' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memactive.png'
set datafile separator ","
set title 'Memory'
plot 'meminfo.csv' using 1:6 title 'Active' with linespoints, 'meminfo.csv' using 1:7 title 'Inactive' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memcommit.png'
set datafile separator ","
set title 'Committed'
plot 'meminfo.csv' using 1:18 title 'Limit' with linespoints, 'meminfo.csv' using 1:19 title 'Committed' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memswap.png'
set datafile separator ","
set title 'Swap Space'
plot 'meminfo.csv' using 1:10 title 'Total' with linespoints, 'meminfo.csv' using 1:11 title 'Free' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memdirty.png'
set datafile separator ","
set title 'Dirty Memory'
plot 'meminfo.csv' using 1:12 title 'Dirty' with linespoints, 'meminfo.csv' using 1:13 title 'Writeback' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memdirty.png'
set datafile separator ","
set title 'Dirty Memory'
plot 'meminfo.csv' using 1:12 title 'Dirty' with linespoints, 'meminfo.csv' using 1:13 title 'Writeback' with linespoints
PLOTCMD
