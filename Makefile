CC=gcc
CFLAGS=-g
PROG = wspy cpu_info amd_smi
SRCS = wspy.c cpu_info.c error.c proctree.c system.c topdown.c amd_smi.c
OBJS = wspy.o cpu_info.o error.o proctree.o system.o topdown.o amd_smi.o
LIBS = -lpthread -lm

# ROCm/AMDGPU defaults (can be overridden on the make command line):
#   make AMDGPU=1 ROCM_DIR=/path/to/rocm
ROCM_DIR ?= /opt/rocm
ROCM_INCLUDE := $(ROCM_DIR)/include
ROCM_LIB := $(ROCM_DIR)/lib

ifdef AMDGPU
CFLAGS += -DAMDGPU=1 -I$(ROCM_INCLUDE)
LIBS += -L$(ROCM_LIB) -lamd_smi
endif

ifdef AMDGPU
all:	wspy cpu_info proctree amd_smi
else
all:	wspy cpu_info proctree
endif

wspy:	wspy.o topdown.o error.o system.o cpu_info.c cpu_info.h
ifdef AMDGPU
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c amd_smi.c error.o system.o $(LIBS)
else
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c error.o system.o $(LIBS)
endif

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info $(CFLAGS) -DTEST_CPU_INFO cpu_info.c error.o

amd_smi: amd_smi.c error.o amd_smi.h
	$(CC) -o amd_smi $(CFLAGS) $(GPUFLAGS) -DTEST_AMD_SMI amd_smi.c error.o $(LIBS)

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy cpu_info amd_smi proctree

# DO NOT DELETE

wspy.o: wspy.h cpu_info.h error.h
cpu_info.o: cpu_info.h error.h
error.o: error.h
proctree.o: error.h
topdown.o: error.h wspy.h cpu_info.h
