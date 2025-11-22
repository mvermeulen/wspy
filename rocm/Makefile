CXX=g++
CXXFLAGS=-g -std=c++17

ROCM_DIR ?= /opt/rocm
ROCM_INCLUDE := $(ROCM_DIR)/include
ROCM_LIB := $(ROCM_DIR)/lib

all: smi_monitor smi_info

smi_monitor:	smi_monitor.o
	$(CXX) -o smi_monitor $(CXXFLAGS) smi_monitor.o -L $(ROCM_LIB) -lamd_smi

smi_monitor.o:	smi_monitor.c
	$(CXX) -c $(CXXFLAGS) -I$(ROCM_INCLUDE) smi_monitor.c

smi_info:	smi_info.o
	$(CXX) -o smi_info $(CXXFLAGS) smi_info.o -L $(ROCM_LIB) -lamd_smi

smi_info.o:	smi_info.c
	$(CXX) -c $(CXXFLAGS) -I$(ROCM_INCLUDE) smi_info.c
