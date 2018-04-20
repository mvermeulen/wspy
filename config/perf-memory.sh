#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'memory.png'
set datafile separator ","
set title 'Memory'
plot 'perf0.csv' using 1:(\$6*64) title 'reads' with linespoints, 'perf0.csv' using 1:(\$7*64) title 'writes' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memorycnt.png'
set datafile separator ","
set title 'Memory'
plot 'perftotal.csv' using 1:2 title 'L3 read access' with linespoints, 'perftotal.csv' using 1:3 title 'L3 read miss' with linespoints, 'perftotal.csv' using 1:4 title 'L3 write access' with linespoints, 'perftotal.csv' using 1:5 title 'L3 write miss' with linespoints, 'perf0.csv' using 1:6 title 'memory read' with linespoints, 'perf0.csv' using 1:7 title 'memory write' with linespoints
PLOTCMD
