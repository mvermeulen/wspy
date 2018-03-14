CC=gcc
CFLAGS=-g

wspy:	wspy.o ktrace.o proctable.o timer.o cpustatus.o error.o
	$(CC) -o wspy $(CFLAGS) wspy.o ktrace.o proctable.o timer.o cpustatus.o error.o -lpthread -lm

wspy.o:	wspy.c wspy.h
	$(CC) -c $(CFLAGS) wspy.c

cpustatus.o:	cpustatus.c wspy.h error.h
	$(CC) -c $(CFLAGS) cpustatus.c

error.o:	error.c error.h
	$(CC) -c $(CFLAGS) error.c

proctable.o:	proctable.c wspy.h error.h
	$(CC) -c $(CFLAGS) proctable.c

ktrace.o:	ktrace.c wspy.h error.h
	$(CC) -c $(CFLAGS) ktrace.c

timer.o:	timer.c wspy.h error.h
	$(CC) -c $(CFLAGS) timer.c

clean:
	-rm *~ *.o

clobber:	clean
	-rm wspy
