#!/bin/bash
gnuplot <<PLOTCMD
set terminal png
set output 'ipc0.png'
set title 'CPU 0 IPC'
set datafile separator ","
plot 'perf0.csv' using 1:(\$2/\$3*2) title 'CPU 0 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc1.png'
set title 'CPU 1 IPC'
set datafile separator ","
plot 'perf1.csv' using 1:(\$2/\$3*2) title 'CPU 1 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc2.png'
set title 'CPU 2 IPC'
set datafile separator ","
plot 'perf2.csv' using 1:(\$2/\$3*2) title 'CPU 2 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc3.png'
set title 'CPU 3 IPC'
set datafile separator ","
plot 'perf3.csv' using 1:(\$2/\$3*2) title 'CPU 3 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc4.png'
set title 'CPU 4 IPC'
set datafile separator ","
plot 'perf4.csv' using 1:(\$2/\$3*2) title 'CPU 4 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc5.png'
set title 'CPU 5 IPC'
set datafile separator ","
plot 'perf5.csv' using 1:(\$2/\$3*2) title 'CPU 5 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc6.png'
set title 'CPU 6 IPC'
set datafile separator ","
plot 'perf6.csv' using 1:(\$2/\$3*2) title 'CPU 6 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipc7.png'
set title 'CPU 7 IPC'
set datafile separator ","
plot 'perf7.csv' using 1:(\$2/\$3*2) title 'CPU 7 IPC' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'ipcall.png'
set title 'ALL CPU IPC'
set datafile separator ","
plot 'perf0.csv' using 1:(\$2/\$3*2) title 'CPU 0' with linespoints,'perf1.csv' using 1:(\$2/\$3*2) title 'CPU 1' with linespoints,'perf2.csv' using 1:(\$2/\$3*2) title 'CPU 2' with linespoints,'perf3.csv' using 1:(\$2/\$3*2) title 'CPU 3' with linespoints,'perf4.csv' using 1:(\$2/\$3*2) title 'CPU 4' with linespoints,'perf5.csv' using 1:(\$2/\$3*2) title 'CPU 5' with linespoints,'perf6.csv' using 1:(\$2/\$3*2) title 'CPU 6' with linespoints,'perf7.csv' using 1:(\$2/\$3*2) title 'CPU 7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend0.png'
set title 'Front End - CPU 0'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(\$4/(\$3)) title 'CPU 0' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend1.png'
set title 'Front End - CPU 1'
set yrange [0:1]
set datafile separator ","
plot 'perf1.csv' using 1:(\$4/(\$3)) title 'CPU 1' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend2.png'
set title 'Front End - CPU 2'
set yrange [0:1]
set datafile separator ","
plot 'perf2.csv' using 1:(\$4/(\$3)) title 'CPU 2' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend3.png'
set title 'Front End - CPU 3'
set yrange [0:1]
set datafile separator ","
plot 'perf3.csv' using 1:(\$4/(\$3)) title 'CPU 3' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend4.png'
set title 'Front End - CPU 4'
set yrange [0:1]
set datafile separator ","
plot 'perf4.csv' using 1:(\$4/(\$3)) title 'CPU 4' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend5.png'
set title 'Front End - CPU 5'
set yrange [0:1]
set datafile separator ","
plot 'perf5.csv' using 1:(\$4/(\$3)) title 'CPU 5' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend6.png'
set title 'Front End - CPU 6'
set yrange [0:1]
set datafile separator ","
plot 'perf6.csv' using 1:(\$4/(\$3)) title 'CPU 6' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontend7.png'
set title 'Front End - CPU 7'
set yrange [0:1]
set datafile separator ","
plot 'perf7.csv' using 1:(\$4/(\$3)) title 'CPU 7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'frontendall.png'
set title 'Front End - All Cores'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(\$4/(\$3)) title 'CPU 0' with linespoints,'perf1.csv' using 1:(\$4/(\$3)) title 'CPU 1' with linespoints,'perf2.csv' using 1:(\$4/(\$3)) title 'CPU 2' with linespoints,'perf3.csv' using 1:(\$4/(\$3)) title 'CPU 3' with linespoints,'perf4.csv' using 1:(\$4/(\$3)) title 'CPU 4' with linespoints,'perf5.csv' using 1:(\$4/(\$3)) title 'CPU 5' with linespoints,'perf6.csv' using 1:(\$4/(\$3)) title 'CPU 6' with linespoints,'perf7.csv' using 1:(\$4/(\$3)) title 'CPU 7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring0.png'
set title 'Retiring - CPU 0'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(\$7/(\$3)) title 'CPU 0' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring1.png'
set title 'Retiring - CPU 1'
set yrange [0:1]
set datafile separator ","
plot 'perf1.csv' using 1:(\$7/(\$3)) title 'CPU 1' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring2.png'
set title 'Retiring - CPU 2'
set yrange [0:1]
set datafile separator ","
plot 'perf2.csv' using 1:(\$7/(\$3)) title 'CPU 2' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring3.png'
set title 'Retiring - CPU 3'
set yrange [0:1]
set datafile separator ","
plot 'perf3.csv' using 1:(\$7/(\$3)) title 'CPU 3' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring4.png'
set title 'Retiring - CPU 4'
set yrange [0:1]
set datafile separator ","
plot 'perf4.csv' using 1:(\$7/(\$3)) title 'CPU 4' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring5.png'
set title 'Retiring - CPU 5'
set yrange [0:1]
set datafile separator ","
plot 'perf5.csv' using 1:(\$7/(\$3)) title 'CPU 5' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring6.png'
set title 'Retiring - CPU 6'
set yrange [0:1]
set datafile separator ","
plot 'perf6.csv' using 1:(\$7/(\$3)) title 'CPU 6' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiring7.png'
set title 'Retiring - CPU 7'
set yrange [0:1]
set datafile separator ","
plot 'perf7.csv' using 1:(\$7/(\$3)) title 'CPU 7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'retiringall.png'
set title 'Retiring - All Cores'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(\$7/(\$3)) title 'CPU 0' with linespoints,'perf1.csv' using 1:(\$7/(\$3)) title 'CPU 1' with linespoints,'perf2.csv' using 1:(\$7/(\$3)) title 'CPU 2' with linespoints,'perf3.csv' using 1:(\$7/(\$3)) title 'CPU 3' with linespoints,'perf4.csv' using 1:(\$7/(\$3)) title 'CPU 4' with linespoints,'perf5.csv' using 1:(\$7/(\$3)) title 'CPU 5' with linespoints,'perf6.csv' using 1:(\$7/(\$3)) title 'CPU 6' with linespoints,'perf7.csv' using 1:(\$7/(\$3)) title 'CPU 7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec0.png'
set title 'Speculation - CPU 0'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU0' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec1.png'
set title 'Speculation - CPU 1'
set yrange [0:1]
set datafile separator ","
plot 'perf1.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU1' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec2.png'
set title 'Speculation - CPU 2'
set yrange [0:1]
set datafile separator ","
plot 'perf2.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU2' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec3.png'
set title 'Speculation - CPU 3'
set yrange [0:1]
set datafile separator ","
plot 'perf3.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU3' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec4.png'
set title 'Speculation - CPU 4'
set yrange [0:1]
set datafile separator ","
plot 'perf4.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU4' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec5.png'
set title 'Speculation - CPU 5'
set yrange [0:1]
set datafile separator ","
plot 'perf5.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU5' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec6.png'
set title 'Speculation - CPU 6'
set yrange [0:1]
set datafile separator ","
plot 'perf6.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU6' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'spec7.png'
set title 'Speculation - CPU 7'
set yrange [0:1]
set datafile separator ","
plot 'perf7.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'specall.png'
set title 'Speculation - All Cores'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU0' with linespoints,'perf1.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU1' with linespoints,'perf2.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU2' with linespoints,'perf3.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU3' with linespoints,'perf4.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU4' with linespoints,'perf5.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU5' with linespoints,'perf6.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU6' with linespoints,'perf7.csv' using 1:((\$6 - \$7 + (\$5))/(\$3)) title 'CPU7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend0.png'
set title 'Back End - CPU 0'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU0' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend1.png'
set title 'Back End - CPU 1'
set yrange [0:1]
set datafile separator ","
plot 'perf1.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU1' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend2.png'
set title 'Back End - CPU 2'
set yrange [0:1]
set datafile separator ","
plot 'perf2.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU2' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend3.png'
set title 'Back End - CPU 3'
set yrange [0:1]
set datafile separator ","
plot 'perf3.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU3' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend4.png'
set title 'Back End - CPU 4'
set yrange [0:1]
set datafile separator ","
plot 'perf4.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU4' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend5.png'
set title 'Back End - CPU 5'
set yrange [0:1]
set datafile separator ","
plot 'perf5.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU5' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend6.png'
set title 'Back End - CPU 6'
set yrange [0:1]
set datafile separator ","
plot 'perf6.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU6' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backend7.png'
set title 'Back End - CPU 7'
set yrange [0:1]
set datafile separator ","
plot 'perf7.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'backendall.png'
set title 'Back End - All Cores'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU0' with linespoints,'perf1.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU1' with linespoints,'perf2.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU2' with linespoints,'perf3.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU3' with linespoints,'perf4.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU4' with linespoints,'perf5.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU5' with linespoints,'perf6.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU6' with linespoints,'perf7.csv' using 1:(1 -((\$4 +\$6 +(\$5))/(\$3))) title 'CPU7' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown0.png'
set title 'CPU 0'
set yrange [0:1]
set datafile separator ","
plot 'perf0.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf0.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf0.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf0.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown1.png'
set title 'CPU 1'
set yrange [0:1]
set datafile separator ","
plot 'perf1.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf1.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf1.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf1.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown2.png'
set title 'CPU 2'
set yrange [0:1]
set datafile separator ","
plot 'perf2.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf2.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf2.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf2.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown3.png'
set title 'CPU 3'
set yrange [0:1]
set datafile separator ","
plot 'perf3.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf3.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf3.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf3.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown4.png'
set title 'CPU 4'
set yrange [0:1]
set datafile separator ","
plot 'perf4.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf4.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf4.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf4.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown5.png'
set title 'CPU 5'
set yrange [0:1]
set datafile separator ","
plot 'perf5.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf5.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf5.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf5.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown6.png'
set title 'CPU 6'
set yrange [0:1]
set datafile separator ","
plot 'perf6.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf6.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf6.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf6.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown7.png'
set title 'CPU 7'
set yrange [0:1]
set datafile separator ","
plot 'perf7.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perf7.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perf7.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perf7.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
if [ -f perftotal.csv ];
then
gnuplot <<PLOTCMD
set terminal png
set output 'topdownall.png'
set title 'All CPUs'
set yrange [0:1]
set datafile separator ","
plot 'perftotal.csv' using 1:(\$4/\$3) title 'front end' with linespoints,'perftotal.csv' using 1:(\$7/\$3) title 'retiring' with linespoints,'perftotal.csv' using 1:((\$6 - \$7 + \$5)/\$3) title 'speculation' with linespoints,'perftotal.csv' using 1:(1 -((\$4 +\$6 +\$5)/\$3)) title 'back end' with linespoints
PLOTCMD
fi
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
set output 'topdown04.png'
set title 'CPU0 + CPU4'
set yrange [0:1]
set datafile separator ","
plot 'perf04.csv' using 1:((\$4+\$11)/(\$3+\$10)) title 'front end' with linespoints,'perf04.csv' using 1:((\$7+\$14)/(\$3+\$10)) title 'retiring' with linespoints,'perf04.csv' using 1:(((\$6+\$13) - (\$7+\$14) + (\$5+\$12))/(\$3+\$10)) title 'speculation' with linespoints,'perf04.csv' using 1:(1 -(((\$4+\$11) +(\$6+\$13) +(\$5+\$12))/(\$3+\$10))) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown15.png'
set title 'CPU1 + CPU5'
set yrange [0:1]
set datafile separator ","
plot 'perf15.csv' using 1:((\$4+\$11)/(\$3+\$10)) title 'front end' with linespoints,'perf15.csv' using 1:((\$7+\$14)/(\$3+\$10)) title 'retiring' with linespoints,'perf15.csv' using 1:(((\$6+\$13) - (\$7+\$14) + (\$5+\$12))/(\$3+\$10)) title 'speculation' with linespoints,'perf15.csv' using 1:(1 -(((\$4+\$11) +(\$6+\$13) +(\$5+\$12))/(\$3+\$10))) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown26.png'
set title 'CPU2 + CPU6'
set yrange [0:1]
set datafile separator ","
plot 'perf26.csv' using 1:((\$4+\$11)/(\$3+\$10)) title 'front end' with linespoints,'perf26.csv' using 1:((\$7+\$14)/(\$3+\$10)) title 'retiring' with linespoints,'perf26.csv' using 1:(((\$6+\$13) - (\$7+\$14) + (\$5+\$12))/(\$3+\$10)) title 'speculation' with linespoints,'perf26.csv' using 1:(1 -(((\$4+\$11) +(\$6+\$13) +(\$5+\$12))/(\$3+\$10))) title 'back end' with linespoints
PLOTCMD
gnuplot <<PLOTCMD
set terminal png
set output 'topdown37.png'
set title 'CPU3 + CPU7'
set yrange [0:1]
set datafile separator ","
plot 'perf37.csv' using 1:((\$4+\$11)/(\$3+\$10)) title 'front end' with linespoints,'perf37.csv' using 1:((\$7+\$14)/(\$3+\$10)) title 'retiring' with linespoints,'perf37.csv' using 1:(((\$6+\$13) - (\$7+\$14) + (\$5+\$12))/(\$3+\$10)) title 'speculation' with linespoints,'perf37.csv' using 1:(1 -(((\$4+\$11) +(\$6+\$13) +(\$5+\$12))/(\$3+\$10))) title 'back end' with linespoints
PLOTCMD
