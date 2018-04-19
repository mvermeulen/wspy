#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'ipc0.png'
set title 'CPU 0 IPC'
set datafile separator ","
plot 'perf0.csv' using 1:(\$2/\$3) title 'CPU 0 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc4.png'
set title 'CPU 4 IPC'
set datafile separator ","
plot 'perf4.csv' using 1:(\$2/\$3) title 'CPU 4 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc5.png'
set title 'CPU 5 IPC'
set datafile separator ","
plot 'perf5.csv' using 1:(\$2/\$3) title 'CPU 5 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc6.png'
set title 'CPU 6 IPC'
set datafile separator ","
plot 'perf6.csv' using 1:(\$2/\$3) title 'CPU 6 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc7.png'
set title 'CPU 7 IPC'
set datafile separator ","
plot 'perf7.csv' using 1:(\$2/\$3) title 'CPU 7 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipcall.png'
set title 'ALL CPU IPC'
set datafile separator ","
plot 'perf0.csv' using 1:(\$2/\$3) title 'CPU 0' with linespoints, 'perf4.csv' using 1:(\$2/\$3) title 'CPU 4' with linespoints, 'perf5.csv' using 1:(\$2/\$3) title 'CPU 5' with linespoints, 'perf6.csv' using 1:(\$2/\$3) title 'CPU 6' with linespoints, 'perf7.csv' using 1:(\$2/\$3) title 'CPU 7' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'cache0.png'
set datafile separator ","
set title 'CPU 0 Cache'
plot 'perf0.csv' using 1:(\$5/\$4) title 'Miss Ratio' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'cacherate0.png'
set datafile separator ","
set title 'CPU 0 Cache'
plot 'perf0.csv' using 1:(\$4/\$2) title 'References' with linespoints, 'perf0.csv' using 1:(\$5/\$2) title 'Misses' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'branch1.png'
set datafile separator ","
set title 'CPU 1 Branch'
plot 'perf1.csv' using 1:(\$5/\$4) title 'Miss Ratio' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'branchrate1.png'
set datafile separator ","
set title 'CPU 1 Branch'
plot 'perf1.csv' using 1:(\$4/\$2) title 'Branches' with linespoints, 'perf1.csv' using 1:(\$5/\$2) title 'Misses' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'memory.png'
set datafile separator ","
set title 'Memory'
plot 'perf2.csv' using 1:(\$2*64) title 'reads' with linespoints, 'perf2.csv' using 1:(\$3*64) title 'writes' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'pagefault3.png'
set datafile separator ","
set title 'Page Faults'
plot 'perf3.csv' using 1:2 title 'page faults' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'context3.png'
set datafile separator ","
set title 'Context Switches'
plot 'perf3.csv' using 1:3 title 'context switches' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'migrate3.png'
set datafile separator ","
set title 'Migrations'
plot 'perf3.csv' using 1:4 title 'CPU migrations' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'cache4.png'
set datafile separator ","
set title 'CPU 4 L1D'
plot 'perf4.csv' using 1:(\$4/\$5) title 'Miss Ratio' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'cacherate4.png'
set datafile separator ","
set title 'CPU 4 L1D'
plot 'perf4.csv' using 1:(\$5/\$2) title 'References' with linespoints, 'perf4.csv' using 1:(\$4/\$2) title 'Misses' with linespoints
PLOTCMD

