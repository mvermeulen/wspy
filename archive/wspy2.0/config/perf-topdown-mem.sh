#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem0.png'
set title 'CPU 0 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf0.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf0.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf0.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf0.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf0.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem1.png'
set title 'CPU 1 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf1.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf1.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf1.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf1.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf1.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem2.png'
set title 'CPU 2 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf2.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf2.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf2.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf2.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf2.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem3.png'
set title 'CPU 3 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf3.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf3.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf3.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf3.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf3.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem4.png'
set title 'CPU 4 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf4.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf4.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf4.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf4.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf4.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem5.png'
set title 'CPU 5 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf5.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf5.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf5.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf5.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf5.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem6.png'
set title 'CPU 6 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf6.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf6.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf6.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf6.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf6.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem7.png'
set title 'CPU 7 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf7.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf7.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf7.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf7.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf7.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmemall.png'
set title 'Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perftotal.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perftotal.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perftotal.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perftotal.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perftotal.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
cat perf0.csv | while read line
do
    echo $line ,
done > perf0comma.csv
cat perf1.csv | while read line
do
    echo $line ,
done > perf1comma.csv
cat perf2.csv | while read line
do
    echo $line ,
done > perf2comma.csv
cat perf3.csv | while read line
do
    echo $line ,
done > perf3comma.csv
paste perf0comma.csv perf4.csv > perf04.csv
paste perf1comma.csv perf5.csv > perf15.csv
paste perf2comma.csv perf6.csv > perf26.csv
paste perf3comma.csv perf7.csv > perf37.csv
rm perf0comma.csv perf1comma.csv perf2comma.csv perf3comma.csv
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem04.png'
set title 'CPU 0+4 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf0.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf0.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf0.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf0.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf0.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem15.png'
set title 'CPU 1+5 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf1.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf1.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf1.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf1.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf1.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem26.png'
set title 'CPU 2+6 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf2.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf2.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf2.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf2.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf2.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdownmem37.png'
set title 'CPU 3+7 Memory Stalls'
set datafile separator ","
set yrange [0:1]
plot 'perf3.csv' using 1:(\$3/\$2) title 'total stalls' with linespoints,'perf3.csv' using 1:(\$4/\$2) title 'from memory read' with linespoints,'perf3.csv' using 1:(\$7/\$2) title 'from memory write' with linespoints,'perf3.csv' using 1:((\$5+\$6)/\$2) title 'read bandwidth stalls' with linespoints,'perf3.csv' using 1:((\$4-(\$5+\$6))/\$2) title 'read latency stalls' with linespoints,
PLOTCMD
