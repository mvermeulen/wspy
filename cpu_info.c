/*
 * Code to inventory and manage CPU capabilities
 */
#define TEST 1
#include <stdio.h>
#include <cpuid.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <sys/sysinfo.h>
#include "cpu_info.h"
#include "error.h"

struct cpu_info *cpu_info = NULL;

int inventory_cpu(void){
  unsigned int eax,ebx,ecx,edx;
  int i;

  cpu_info = calloc(1,sizeof(struct cpu_info));

  // vendor string
  union cpuid0_vendor {
    struct { unsigned int ebx, edx, ecx; };
    char vendor[12];
  } cpuid_0;
  
  __cpuid(0,eax,ebx,ecx,edx);
  cpuid_0.ebx = ebx;
  cpuid_0.ecx = ecx;  
  cpuid_0.edx = edx;
  if (!strncmp(cpuid_0.vendor,"AuthenticAMD",12))
    cpu_info->vendor = VENDOR_AMD;
  else if (!strncmp(cpuid_0.vendor,"GenuineIntel",12))
    cpu_info->vendor = VENDOR_INTEL;
  else
    cpu_info->vendor = VENDOR_UNKNOWN;
  
  // model and family
  union cpuid1_modelinfo {
    struct {
      unsigned int step: 4;
      unsigned int model: 4;
      unsigned int family: 4;
      unsigned int ptype: 2;
      unsigned int pad1: 2;
      unsigned int emodel: 4;
      unsigned int efamily: 8;
      unsigned int pad2: 4;
    };
    unsigned int eax;
  } cpuid_1;

  __cpuid(1,eax,ebx,ecx,edx);
  cpuid_1.eax = eax;
  cpu_info->family = (cpuid_1.family==0xf)?
    cpuid_1.family+cpuid_1.efamily:
    cpuid_1.family;
  cpu_info->model = cpuid_1.model;
  printf("family = %x, model = %x\n",
	 cpu_info->family, cpu_info->model);

  // number of cores
  cpu_info->num_cores = get_nprocs();
  cpu_info->coreinfo = calloc(cpu_info->num_cores,
			      sizeof(struct cpu_core_info));

  // specify the core type
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->vendor == VENDOR_AMD){
      if (cpu_info->model == 0x17 || cpu_info->model == 0x19){
	// Zen
	cpu_info->coreinfo[i].vendor = CORE_AMD_ZEN;
      } else {
	cpu_info->coreinfo[i].vendor = CORE_AMD_UNKNOWN;
      }
    } else if (cpu_info->vendor == VENDOR_INTEL){
      cpu_info->coreinfo[i].vendor = CORE_INTEL_UNKNOWN;
    }
    cpu_info->coreinfo->is_available = 0;
    cpu_info->coreinfo->is_counter_started = 0;
  }
  return 0;
}

#if TEST
int main(void){
  inventory_cpu();
}
#endif
