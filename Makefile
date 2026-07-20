CC=gcc
CFLAGS=-g
PROG = wspy cpu_info amd_smi
SRCS = wspy.c cpu_info.c error.c json_util.c json_reader.c manifest.c run_index.c coverage.c provenance.c ibs.c power.c preflight.c phase.c multipass.c affinity.c gpu_fusion.c cgroup.c proctree.c system.c topdown.c amd_smi.c amd_sysfs.c nvidia_nvml.c validate.c core_report.c
OBJS = wspy.o cpu_info.o error.o json_util.o json_reader.o manifest.o run_index.o coverage.o provenance.o ibs.o power.o preflight.o phase.o multipass.o affinity.o gpu_fusion.o cgroup.o proctree.o system.o topdown.o amd_smi.o
LIBS = -lpthread -lm
STORE_LIBS = -lsqlite3

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

# NVIDIA GPU support (make NVIDIA=1): NVML (libnvidia-ml.so.1) is dlopen()'d
# at runtime, not linked at build time, so unlike AMDGPU there's no header/
# lib dependency to auto-detect here -- NVIDIA=1 just enables the code path
# and links -ldl. AMDGPU and NVIDIA are independent axes; either, both, or
# neither may be set.
ifdef NVIDIA
CFLAGS += -DNVIDIA=1
LIBS += -ldl
endif

# GPU_SRCS/GPU_BINS accumulate across both independent GPU axes so wspy's
# link line and `all`'s binary list don't need to be duplicated per axis
# combination (AMDGPU-only, NVIDIA-only, both, or neither).
GPU_SRCS =
GPU_BINS =
ifdef AMDGPU
GPU_SRCS += amd_smi.c amd_sysfs.c
GPU_BINS += amd_smi amd_sysfs
endif
ifdef NVIDIA
GPU_SRCS += nvidia_nvml.c
GPU_BINS += nvidia_nvml
endif

all:	wspy cpu_info proctree wspy-validate wspy-ledger wspy-store wspy-summary wspy-plot wspy-core-report $(GPU_BINS)

wspy:	wspy.o topdown.o error.o system.o json_util.o manifest.o run_index.o coverage.o provenance.o ibs.o power.o preflight.o phase.o multipass.o affinity.o gpu_fusion.o cgroup.o cpu_info.c cpu_info.h
	$(CC) -o wspy $(CFLAGS) wspy.o topdown.o cpu_info.c $(GPU_SRCS) error.o system.o json_util.o manifest.o run_index.o coverage.o provenance.o ibs.o power.o preflight.o phase.o multipass.o affinity.o gpu_fusion.o cgroup.o $(LIBS)

proctree:	proctree.o error.o json_util.o json_reader.o
	$(CC) -o proctree proctree.o error.o json_util.o json_reader.o -lm

wspy-validate:	validate.o json_reader.o
	$(CC) -o wspy-validate $(CFLAGS) validate.o json_reader.o -lm

wspy-ledger:	ledger.o json_reader.o
	$(CC) -o wspy-ledger $(CFLAGS) ledger.o json_reader.o

wspy-store:	store.o json_reader.o
	$(CC) -o wspy-store $(CFLAGS) store.o json_reader.o $(STORE_LIBS)

wspy-summary:	summary.o
	$(CC) -o wspy-summary $(CFLAGS) summary.o $(STORE_LIBS) -lm

wspy-plot:	plot.o
	$(CC) -o wspy-plot $(CFLAGS) plot.o

wspy-core-report:	core_report.o error.o cpu_info.c cpu_info.h
	$(CC) -o wspy-core-report $(CFLAGS) core_report.o cpu_info.c error.o -lm

cpu_info:	cpu_info.c error.o cpu_info.h
	$(CC) -o cpu_info $(CFLAGS) -DTEST_CPU_INFO cpu_info.c error.o

amd_smi: amd_smi.c error.o amd_smi.h
	$(CC) -o amd_smi $(CFLAGS) $(GPUFLAGS) -DTEST_AMD_SMI amd_smi.c error.o $(LIBS)

amd_sysfs: amd_sysfs.c error.o amd_sysfs.h
	$(CC) -o amd_sysfs $(CFLAGS) $(GPUFLAGS) -DTEST_AMD_SYSFS amd_sysfs.c error.o $(LIBS)

nvidia_nvml: nvidia_nvml.c error.o nvidia_nvml.h
	$(CC) -o nvidia_nvml $(CFLAGS) -DTEST_NVIDIA_NVML nvidia_nvml.c error.o $(LIBS)

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
	-rm wspy cpu_info amd_smi amd_sysfs nvidia_nvml proctree wspy-validate wspy-ledger wspy-store wspy-summary wspy-plot wspy-core-report test_hip_init test_hip_kernel test_proctree test_wspy test_validate test_ledger test_ibs test_power test_phase test_store test_summary test_plot test_affinity test_gpu_fusion test_core_report test_cgroup libwspy_profiler.so

# DO NOT DELETE

wspy.o: wspy.h cpu_info.h error.h manifest.h run_index.h coverage.h provenance.h ibs.h power.h preflight.h phase.h affinity.h nvidia_nvml.h
cpu_info.o: cpu_info.h error.h
error.o: error.h
json_util.o: json_util.h
json_reader.o: json_reader.h
manifest.o: manifest.h wspy.h cpu_info.h error.h json_util.h provenance.h
run_index.o: run_index.h manifest.h wspy.h cpu_info.h error.h json_util.h provenance.h
coverage.o: coverage.h wspy.h cpu_info.h
provenance.o: provenance.h
ibs.o: ibs.h wspy.h cpu_info.h error.h
power.o: power.h wspy.h cpu_info.h error.h
preflight.o: preflight.h wspy.h cpu_info.h error.h
phase.o: phase.h wspy.h cpu_info.h
multipass.o: multipass.h wspy.h cpu_info.h error.h preflight.h
affinity.o: affinity.h wspy.h cpu_info.h error.h
proctree.o: error.h json_util.h json_reader.h
topdown.o: error.h wspy.h cpu_info.h coverage.h ptrace_arch.h phase.h affinity.h power.h
validate.o: json_reader.h manifest.h provenance.h
ledger.o: json_reader.h run_index.h manifest.h
store.o: json_reader.h run_index.h manifest.h
plot.o: plot.c

# Always built GPU-disabled (test_wspy.c forces AMDGPU=0 to stub out main() and
# skip GPU code), using its own objects so it never picks up a topdown.o/system.o
# etc. left over from an `AMDGPU=1` build of wspy in the same tree, which would
# reference gpu_busy_requested/gpu_metrics_requested symbols that this build
# doesn't define and fail to link.
test_wspy: test_wspy.c wspy.c wspy.h cpu_info.h error.h manifest.h manifest.c run_index.h run_index.c json_util.h json_util.c coverage.h coverage.c provenance.h provenance.c ibs.h ibs.c power.h power.c preflight.h preflight.c phase.h phase.c multipass.h multipass.c affinity.h affinity.c cgroup.h cgroup.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_error.o error.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_cpu_info.o cpu_info.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_system.o system.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_topdown.o topdown.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_json_util.o json_util.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_manifest.o manifest.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_run_index.o run_index.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_coverage.o coverage.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_provenance.o provenance.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_ibs_probe.o ibs.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_power_probe.o power.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_preflight_probe.o preflight.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_phase_probe.o phase.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_multipass_probe.o multipass.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_affinity_probe.o affinity.c
	$(CC) -g -DAMDGPU=0 -DNVIDIA=0 -c -o test_cgroup_probe.o cgroup.c
	$(CC) -o test_wspy -g -DAMDGPU=0 -DNVIDIA=0 -DTEST_WSPY test_wspy.c test_error.o test_cpu_info.o test_system.o test_topdown.o test_json_util.o test_manifest.o test_run_index.o test_coverage.o test_provenance.o test_ibs_probe.o test_power_probe.o test_preflight_probe.o test_phase_probe.o test_multipass_probe.o test_affinity_probe.o test_cgroup_probe.o -lpthread -lm

test_proctree: test_proctree.c proctree.c error.h error.o json_util.c json_reader.c json_util.h json_reader.h
	$(CC) -o test_proctree $(CFLAGS) -DTEST_PROCTREE test_proctree.c error.o json_util.c json_reader.c $(LIBS)

test_validate: test_validate.c validate.c json_reader.c json_reader.h manifest.h
	$(CC) -o test_validate $(CFLAGS) -DTEST_VALIDATE test_validate.c json_reader.c -lm

test_ledger: test_ledger.c ledger.c json_reader.c json_reader.h run_index.h manifest.h
	$(CC) -o test_ledger $(CFLAGS) -DTEST_LEDGER test_ledger.c json_reader.c

test_store: test_store.c store.c json_reader.c json_reader.h run_index.h manifest.h
	$(CC) -o test_store $(CFLAGS) -DTEST_STORE test_store.c json_reader.c $(STORE_LIBS)

test_summary: test_summary.c summary.c
	$(CC) -o test_summary $(CFLAGS) -DTEST_SUMMARY test_summary.c $(STORE_LIBS) -lm

test_plot: test_plot.c plot.c
	$(CC) -o test_plot $(CFLAGS) -DTEST_PLOT test_plot.c

test_ibs: test_ibs.c ibs.c ibs.h error.c error.h
	$(CC) -o test_ibs $(CFLAGS) test_ibs.c error.c

test_power: test_power.c power.c power.h error.c error.h
	$(CC) -o test_power $(CFLAGS) test_power.c error.c

test_phase: test_phase.c phase.c phase.h wspy.h cpu_info.h
	$(CC) -o test_phase $(CFLAGS) test_phase.c -lm

test_affinity: test_affinity.c affinity.c affinity.h error.c error.h
	$(CC) -o test_affinity $(CFLAGS) test_affinity.c error.c

test_gpu_fusion: test_gpu_fusion.c gpu_fusion.c gpu_fusion.h
	$(CC) -o test_gpu_fusion $(CFLAGS) test_gpu_fusion.c

test_core_report: test_core_report.c core_report.c cpu_info.h error.c error.h
	$(CC) -o test_core_report $(CFLAGS) -DTEST_CORE_REPORT test_core_report.c error.c -lm

test_cgroup: test_cgroup.c cgroup.c cgroup.h
	$(CC) -o test_cgroup $(CFLAGS) test_cgroup.c

test: test_wspy test_proctree test_validate test_ledger test_ibs test_power test_phase test_store test_summary test_plot test_affinity test_gpu_fusion test_core_report test_cgroup
	./test_wspy
	./test_proctree
	./test_validate
	./test_ledger
	./test_ibs
	./test_power
	./test_phase
	./test_store
	./test_summary
	./test_plot
	./test_affinity
	./test_gpu_fusion
	./test_core_report
	./test_cgroup
