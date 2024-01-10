CC=gcc
CFLAGS=-g
PROG = wspy
SRCS = cpu_info.c error.c proctree.c topdown.c
OBJS = cpu_info.o error.o proctree.o topdown.o
LIBS = -lpthread -lm

all:	wspy cpu_info proctree

wspy:	topdown.o error.o cpu_info.c cpu_info.h
	$(CC) -o wspy $(CFLAGS) topdown.o cpu_info.c error.o

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info -DTEST_CPU_INFO cpu_info.c error.o

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy wspy cpu_info proctree

# DO NOT DELETE

cpu_info.o: cpu_info.h error.h
error.o: error.h
proctree.o: error.h
topdown.o: error.h cpu_info.h
