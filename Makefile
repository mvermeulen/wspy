CC=gcc
CFLAGS=-g
PROG = wspy
SRCS = wspy.c ktrace.c proctable.c timer.c cpustatus.c error.c
OBJS = wspy.o ktrace.o proctable.o timer.o cpustatus.o error.o
LIBS = -lpthread -lm

wspy:	$(OBJS)
	$(CC) -o $(PROG) $(CFLAGS) $(OBJS) $(LIBS)

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy

# DO NOT DELETE

wspy.o: wspy.h error.h
ktrace.o: wspy.h error.h
proctable.o: wspy.h error.h
timer.o: wspy.h error.h
cpustatus.o: wspy.h error.h
error.o: error.h
