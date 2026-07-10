CC=gcc
CFLAGS=-g
PROG = wspy cpu_info amd_smi
SRCS = wspy.c cpu_info.c error.c json_util.c json_reader.c manifest.c run_index.c coverage.c provenance.c proctree.c system.c topdown.c amd_smi.c amd_sysfs.c validate.c
OBJS = wspy.o cpu_info.o error.o json_util.o json_reader.o manifest.o run_index.o coverage.o provenance.o proctree.o system.o topdown.o amd_smi.o
LIBS = -lpthread -lm

# ROCm/AMDGPU defaults (can be overridden on the make command line):
#   make AMDGPU=1 ROCM_DIR=/path/to/rocm
# ROCm was historically only installed under /opt/rocm, but distro packages
# (e.g. Debian/Ubuntu's rocm-smi-lib) may instead put amd_smi/amdsmi.h and
# libamd_smi under /usr. Auto-detect between the two unless ROCM_DIR is set
# explicitly; /opt/rocm wins if both are present, for backwards compatibility.
ifdef AMDGPU
ROCM_DIR ?= $(shell \
  if [ -e /opt/rocm/include/amd_smi/amdsmi.h ]; then echo /opt/rocm; \
  elif [ -e /usr/include/amd_smi/amdsmi.h ]; then echo /usr; \
  else echo /opt/rocm; fi)
ROCM_INCLUDE := $(ROCM_DIR)/include
ROCM_LIB := $(ROCM_DIR)/lib
CFLAGS += -DAMDGPU=1 -I$(ROCM_INCLUDE)
LIBS += -L$(ROCM_LIB) -lamd_smi
endif

ifdef AMDGPU
all:	wspy cpu_info proctree wspy-validate wspy-ledger amd_smi amd_sysfs
else
all:	wspy cpu_info proctree wspy-validate wspy-ledger
endif

wspy:	wspy.o topdown.o error.o system.o json_util.o manifest.o run_index.o coverage.o provenance.o cpu_info.c cpu_info.h
ifdef AMDGPU
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c amd_smi.c amd_sysfs.c error.o system.o json_util.o manifest.o run_index.o coverage.o provenance.o $(LIBS)
else
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c error.o system.o json_util.o manifest.o run_index.o coverage.o provenance.o $(LIBS)
endif

proctree:	proctree.o error.o
	$(CC) -o proctree proctree.o error.o

wspy-validate:	validate.o json_reader.o
	$(CC) -o wspy-validate $(CFLAGS) validate.o json_reader.o -lm

wspy-ledger:	ledger.o json_reader.o
	$(CC) -o wspy-ledger $(CFLAGS) ledger.o json_reader.o

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info $(CFLAGS) -DTEST_CPU_INFO cpu_info.c error.o

amd_smi: amd_smi.c error.o amd_smi.h
	$(CC) -o amd_smi $(CFLAGS) $(GPUFLAGS) -DTEST_AMD_SMI amd_smi.c error.o $(LIBS)

amd_sysfs: amd_sysfs.c error.o amd_sysfs.h
	$(CC) -o amd_sysfs $(CFLAGS) $(GPUFLAGS) -DTEST_AMD_SYSFS amd_sysfs.c error.o $(LIBS)

topdown.o:	topdown.c
	$(CC) -c $(CFLAGS) topdown.c

proctree.o:	proctree.c
	$(CC) -c $(CFLAGS) proctree.c

depend:
	-makedepend -Y -- $(CFLAGS) -- $(SRCS)

# Regenerate compile_commands.json for editor tooling (VSCode C/C++ extension,
# clangd, Cline). Paths in it are absolute to this checkout, so it's
# gitignored and each contributor generates their own.
compile_commands.json: scripts/gen_compile_commands.py Makefile
	./scripts/gen_compile_commands.py

clean:
	-rm *~ *.o *.bak

clobber:	clean
	-rm wspy cpu_info amd_smi amd_sysfs proctree wspy-validate wspy-ledger test_hip_init test_hip_kernel test_proctree test_wspy test_validate test_ledger libwspy_profiler.so

# DO NOT DELETE

wspy.o: wspy.h cpu_info.h error.h manifest.h run_index.h coverage.h provenance.h
cpu_info.o: cpu_info.h error.h
error.o: error.h
json_util.o: json_util.h
json_reader.o: json_reader.h
manifest.o: manifest.h wspy.h cpu_info.h error.h json_util.h provenance.h
run_index.o: run_index.h manifest.h wspy.h cpu_info.h error.h json_util.h provenance.h
coverage.o: coverage.h wspy.h cpu_info.h
provenance.o: provenance.h
proctree.o: error.h
topdown.o: error.h wspy.h cpu_info.h coverage.h
validate.o: json_reader.h manifest.h provenance.h
ledger.o: json_reader.h

# Always built GPU-disabled (test_wspy.c forces AMDGPU=0 to stub out main() and
# skip GPU code), using its own objects so it never picks up a topdown.o/system.o
# etc. left over from an `AMDGPU=1` build of wspy in the same tree, which would
# reference gpu_busy_requested/gpu_metrics_requested symbols that this build
# doesn't define and fail to link.
test_wspy: test_wspy.c wspy.c wspy.h cpu_info.h error.h manifest.h manifest.c run_index.h run_index.c json_util.h json_util.c coverage.h coverage.c provenance.h provenance.c
	$(CC) -g -DAMDGPU=0 -c -o test_error.o error.c
	$(CC) -g -DAMDGPU=0 -c -o test_cpu_info.o cpu_info.c
	$(CC) -g -DAMDGPU=0 -c -o test_system.o system.c
	$(CC) -g -DAMDGPU=0 -c -o test_topdown.o topdown.c
	$(CC) -g -DAMDGPU=0 -c -o test_json_util.o json_util.c
	$(CC) -g -DAMDGPU=0 -c -o test_manifest.o manifest.c
	$(CC) -g -DAMDGPU=0 -c -o test_run_index.o run_index.c
	$(CC) -g -DAMDGPU=0 -c -o test_coverage.o coverage.c
	$(CC) -g -DAMDGPU=0 -c -o test_provenance.o provenance.c
	$(CC) -o test_wspy -g -DAMDGPU=0 -DTEST_WSPY test_wspy.c test_error.o test_cpu_info.o test_system.o test_topdown.o test_json_util.o test_manifest.o test_run_index.o test_coverage.o test_provenance.o -lpthread -lm

test_proctree: test_proctree.c proctree.c error.h error.o
	$(CC) -o test_proctree $(CFLAGS) -DTEST_PROCTREE test_proctree.c error.o $(LIBS)

test_validate: test_validate.c validate.c json_reader.c json_reader.h manifest.h
	$(CC) -o test_validate $(CFLAGS) -DTEST_VALIDATE test_validate.c json_reader.c -lm

test_ledger: test_ledger.c ledger.c json_reader.c json_reader.h
	$(CC) -o test_ledger $(CFLAGS) -DTEST_LEDGER test_ledger.c json_reader.c

test: test_wspy test_proctree test_validate test_ledger
	./test_wspy
	./test_proctree
	./test_validate
	./test_ledger
