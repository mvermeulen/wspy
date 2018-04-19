#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'memory.png'
set datafile separator ","
set title 'Memory'
plot 'perf0.csv' using 1:(\$2*64) title 'reads' with linespoints, 'perf0.csv' using 1:(\$3*64) title 'writes' with linespoints
PLOTCMD
