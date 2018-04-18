CC=gcc
CFLAGS=-g
PROG = wspy
SRCS = wspy.c ftrace.c ptrace.c proctable.c timer.c cpustats.c diskstats.c memstats.c netstats.c pcounter.c config.c error.c
OBJS = wspy.o ftrace.o ptrace.o proctable.o timer.o cpustats.o diskstats.o memstats.o netstats.o pcounter.o config.c error.o
LIBS = -lpthread -lm

all:	wspy process-csv

wspy:	$(OBJS)
	$(CC) -o $(PROG) $(CFLAGS) $(OBJS) $(LIBS)

process-csv:	process_csv.o error.o
	$(CC) -o process-csv $(CFLAGS) process_csv.o error.o

process-csv.o:	process_csv.c
	$(CC) -c $(CFLAGS) process_csv.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy process-csv

# DO NOT DELETE

wspy.o: wspy.h error.h
ftrace.o: wspy.h error.h
ptrace.o: wspy.h error.h
proctable.o: wspy.h error.h
timer.o: wspy.h error.h
cpustats.o: wspy.h error.h
diskstats.o: wspy.h error.h
memstats.o: wspy.h error.h
netstats.o: wspy.h error.h
pcounter.o: wspy.h error.h
config.o: wspy.h error.h
error.o: error.h
