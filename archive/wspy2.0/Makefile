CC=gcc
CFLAGS=-g
PROG = wspy
SRCS = wspy.c ftrace.c ptrace.c ptrace2.c tracecmd.c proctable.c timer.c cpustats.c diskstats.c memstats.c netstats.c pcounter.c config.c error.c vendor.c
OBJS = wspy.o ftrace.o ptrace.o ptrace2.o tracecmd.o proctable.o timer.o cpustats.o diskstats.o memstats.o netstats.o pcounter.o config.c error.o vendor.o
LIBS = -lpthread -lm

all:	wspy process-csv topdown cpu_info proctree

wspy:	$(OBJS)
	$(CC) -o $(PROG) $(CFLAGS) $(OBJS) $(LIBS)

process-csv:	process_csv.o error.o
	$(CC) -o process-csv $(CFLAGS) process_csv.o error.o

topdown:	topdown.o error.o vendor.o cpu_info.c cpu_info.h
	$(CC) -o topdown $(CFLAGS) topdown.o vendor.o cpu_info.c error.o

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info -DTEST_CPU_INFO cpu_info.c error.o

process-csv.o:	process_csv.c
	$(CC) -c $(CFLAGS) process_csv.c

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy process-csv topdown cpu_info proctree

# DO NOT DELETE

wspy.o: wspy.h error.h
ftrace.o: wspy.h error.h
ptrace.o: wspy.h error.h
tracecmd.o: wspy.h error.h
proctable.o: wspy.h error.h
timer.o: wspy.h error.h
cpustats.o: wspy.h error.h
diskstats.o: wspy.h error.h
memstats.o: wspy.h error.h
netstats.o: wspy.h error.h
pcounter.o: wspy.h error.h
config.o: wspy.h error.h
error.o: error.h
vendor.o: wspy.h
