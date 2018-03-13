CC=gcc
CFLAGS=-g -I../mevlib

wspy:	wspy.o ktrace.o proctable.o timer.o cpustatus.o
	$(CC) -o wspy $(CFLAGS) wspy.o ktrace.o proctable.o timer.o cpustatus.o -L../mevlib -lmev -lpthread -lm

wspy.o:	wspy.c wspy.h
	$(CC) -c $(CFLAGS) wspy.c

ktrace.o:	ktrace.c wspy.h
	$(CC) -c $(CFLAGS) ktrace.c

timer.o:	timer.c wspy.h
	$(CC) -c $(CFLAGS) timer.c

cpustatus.o:	cpustatus.c wspy.h
	$(CC) -c $(CFLAGS) cpustatus.c

proctable.o:	proctable.c wspy.h
	$(CC) -c $(CFLAGS) proctable.c

clean:
	-rm *~ *.o

clobber:	clean
	-rm wspy
