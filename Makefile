CC=gcc
CFLAGS=-g
PROG = wspy cpu_info gpu_info
SRCS = wspy.c cpu_info.c error.c proctree.c system.c topdown.c
OBJS = wspy.o cpu_info.o error.o proctree.o system.o topdown.o
LIBS = -lpthread -lm

ifdef AMDGPU
CFLAGS += -DAMDGPU=1 -I/opt/rocm/include
LIBS += -L /opt/rocm/lib -lamd_smi
endif

ifdef AMDGPU
all:	wspy cpu_info proctree gpu_info
else
all:	wspy cpu_info proctree
endif

wspy:	wspy.o topdown.o error.o system.o cpu_info.c cpu_info.h
ifdef AMDGPU
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c gpu_info.c error.o system.o $(LIBS)
else
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c error.o system.o $(LIBS)
endif

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info $(CFLAGS) -DTEST_CPU_INFO cpu_info.c error.o

gpu_info:	gpu_info.c error.c gpu_info.h
	$(CC) -o gpu_info $(CFLAGS) $(GPUFLAGS) -DTEST_GPU_INFO gpu_info.c error.o $(LIBS)

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy wspy cpu_info gpu_info proctree

# DO NOT DELETE

wspy.o: wspy.h cpu_info.h error.h
cpu_info.o: cpu_info.h error.h
error.o: error.h
proctree.o: error.h
topdown.o: error.h wspy.h cpu_info.h
