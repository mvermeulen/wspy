/*
 * Code to inventory and manage CPU capabilities
 */
#define _GNU_SOURCE
#include <stdio.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include "cpu_info.h"
#include "error.h"

struct cpu_info *cpu_info = NULL;

const char *cpu_vendor_name(enum cpu_vendor vendor){
  switch (vendor){
  case VENDOR_AMD: return "AMD";
  case VENDOR_INTEL: return "Intel";
  case VENDOR_ARM: return "ARM";
  default: return "unknown";
  }
}

static int parse_cpu_list_count(const char *value){
  int total = 0;
  const char *p = value;

  while (*p){
    char *endptr;
    long low = strtol(p,&endptr,10);
    long high = low;

    if (endptr == p) break;
    p = endptr;
    if (*p == '-'){
      p++;
      high = strtol(p,&endptr,10);
      if (endptr == p) break;
      p = endptr;
    }
    if (high >= low && low >= 0) total += (int)(high - low + 1);
    if (*p == ',') p++;
    while (*p == ' ' || *p == '\t') p++;
  }

  return total;
}

static int read_cpu_list_count(const char *path){
  FILE *fp;
  char buf[256];

  fp = fopen(path,"r");
  if (!fp) return -1;
  if (!fgets(buf,sizeof(buf),fp)){
    fclose(fp);
    return -1;
  }
  fclose(fp);
  return parse_cpu_list_count(buf);
}

static int read_cpuinfo_hex_field(const char *field,unsigned int *value){
  FILE *fp;
  char line[256];
  size_t field_len = strlen(field);

  fp = fopen("/proc/cpuinfo","r");
  if (!fp) return -1;
  while (fgets(line,sizeof(line),fp)){
    if (!strncmp(line,field,field_len)){
      char *colon = strchr(line,':');
      if (!colon) continue;
      colon++;
      while (*colon == ' ' || *colon == '\t') colon++;
      *value = (unsigned int)strtoul(colon,NULL,0);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  return -1;
}

static int read_cpuinfo_dec_field(const char *field,unsigned int *value){
  FILE *fp;
  char line[256];
  size_t field_len = strlen(field);

  fp = fopen("/proc/cpuinfo","r");
  if (!fp) return -1;
  while (fgets(line,sizeof(line),fp)){
    if (!strncmp(line,field,field_len)){
      char *colon = strchr(line,':');
      if (!colon) continue;
      colon++;
      while (*colon == ' ' || *colon == '\t') colon++;
      *value = (unsigned int)strtoul(colon,NULL,10);
      fclose(fp);
      return 0;
    }
  }
  fclose(fp);
  return -1;
}

int inventory_cpu(void){
#ifdef __x86_64__
  unsigned int eax,ebx,ecx,edx;
#endif
  int i;
  int nwarn = 0;
  struct stat statbuf;
  FILE *fp;
  int core_count = -1;

  cpu_info = calloc(1,sizeof(struct cpu_info));

#ifdef __x86_64__
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
  else {
    cpu_info->vendor = VENDOR_UNKNOWN;
    warning("unknown cpu %16s\n",cpuid_0.vendor);
  }
  
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
  cpu_info->model = ((cpuid_1.family==0x6)||(cpuid_1.family==0xf))?
    (cpuid_1.emodel<<4) + cpuid_1.model:
    cpuid_1.model;
#else
#ifdef __aarch64__
  cpu_info->vendor = VENDOR_ARM;
#else
  cpu_info->vendor = VENDOR_UNKNOWN;
#endif
  if (read_cpuinfo_hex_field("CPU implementer",&cpu_info->family) == -1){
    if (read_cpuinfo_dec_field("CPU architecture",&cpu_info->family) == -1){
      cpu_info->family = 0;
      warning("unable to determine CPU family from /proc/cpuinfo\n");
    }
  }
  if (read_cpuinfo_hex_field("CPU part",&cpu_info->model) == -1){
    cpu_info->model = 0;
    warning("unable to determine CPU model from /proc/cpuinfo\n");
  }
#endif

  // number of cores (prefer sysfs topology fallbacks before libc helpers)
  core_count = read_cpu_list_count("/sys/devices/system/cpu/present");
  if (core_count <= 0) core_count = read_cpu_list_count("/sys/devices/system/cpu/online");
  if (core_count <= 0) core_count = get_nprocs();

  cpu_info->num_cores = core_count;
  cpu_info->coreinfo = calloc(cpu_info->num_cores,
			      sizeof(struct cpu_core_info));

  // specify the core type
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->vendor == VENDOR_ARM){
      cpu_info->coreinfo[i].vendor = CORE_ARM_GENERIC;
    } else if (cpu_info->vendor == VENDOR_AMD){
      if ((cpu_info->family == 0x17) || (cpu_info->family == 0x19)){
	// Zen
	cpu_info->coreinfo[i].vendor = CORE_AMD_ZEN;
      } else if (cpu_info->family == 0x1a) {
	// Zen5
	cpu_info->coreinfo[i].vendor = CORE_AMD_ZEN5;
      } else {
	cpu_info->coreinfo[i].vendor = CORE_AMD_UNKNOWN;
	if (nwarn == 0){
	  warning("unimplemented AMD CPU, family %x, model %x\n",
		  cpu_info->family,cpu_info->model);
	  nwarn++;
	}
      }
    } else if (cpu_info->vendor == VENDOR_INTEL){
      if ((cpu_info->family == 6) &&
	  ((cpu_info->model == 0xba)||(cpu_info->model == 0xb7)||
	   (cpu_info->model == 0x9a)||(cpu_info->model == 0x97)||
	   (cpu_info->model == 0xa7))){
	cpu_info->coreinfo[i].vendor = CORE_INTEL_CORE;
      } else {
	cpu_info->coreinfo[i].vendor = CORE_INTEL_UNKNOWN;
	if (nwarn == 0){
	  warning("unimplemented Intel CPU, family %x, model %x\n",
		  cpu_info->family,cpu_info->model);
	}
	nwarn++;
      }
    } else {
      cpu_info->coreinfo[i].vendor = CORE_UNKNOWN;
    }
    cpu_info->coreinfo[i].is_available = 0;
    cpu_info->coreinfo[i].is_counter_started = 0;
  }
  // fix up hybrid cores for Raptor Lake and Alder Lake
  if (cpu_info->vendor == VENDOR_INTEL &&
      cpu_info->family == 6 &&
      ((cpu_info->model == 0xba)||(cpu_info->model == 0xb7)||
       (cpu_info->model == 0x9a)||(cpu_info->model == 0x97))){
    if (stat("/sys/devices/cpu_atom/cpus",&statbuf) != -1){
      cpu_info->is_hybrid = 1;
      if (fp = fopen("/sys/devices/cpu_atom/cpus","r")){
	int low,high;
	if (fscanf(fp,"%d-%d",&low,&high) == 2){
	  for (i=low;i<=high;i++){
	    cpu_info->coreinfo[i].vendor = CORE_INTEL_ATOM;
	  }
	}
	fclose(fp);
      }
    }
    if (stat("/sys/devices/cpu_core/cpus",&statbuf) != -1){
      if (fp = fopen("/sys/devices/cpu_core/cpus","r")){
	int low,high;
	if (fscanf(fp,"%d-%d",&low,&high) == 2){
	  for (i=low;i<=high;i++){
	    cpu_info->coreinfo[i].vendor = CORE_INTEL_CORE;
	  }
	}
	fclose(fp);
      }
    }    
  }
  // check affinity mask for available CPUs
  cpu_set_t set;
  cpu_info->num_cores_available;
  CPU_ZERO(&set);
  if (sched_getaffinity(getpid(),sizeof(set),&set) == -1){
    fatal("unable to get CPU affinity\n");
  }
  for (i=0;i<cpu_info->num_cores;i++){
    if (CPU_ISSET(i,&set)){
      cpu_info->coreinfo[i].is_available = 1;
      cpu_info->num_cores_available++;
    }
  }
  
  return 0;
}

#if TEST_CPU_INFO
int main(void){
  int i;
  inventory_cpu();

  printf("CPU information:\n");
  switch(cpu_info->vendor){
  case VENDOR_AMD:
    printf("\tAMD family %x model %x\n",cpu_info->family,cpu_info->model);
    break;
  case VENDOR_INTEL:
    printf("\tIntel family %x model %x\n",cpu_info->family,cpu_info->model);
    break;
  case VENDOR_ARM:
    printf("\tARM family %x model %x\n",cpu_info->family,cpu_info->model);
    break;
  default:
    printf("Unknown CPU\n");
    return 0;
  }

  for (i=0;i<cpu_info->num_cores;i++){
    printf("\t   ");
    printf("%c %d ",(cpu_info->coreinfo[i].is_available)?'*':' ',i);
    switch(cpu_info->coreinfo[i].vendor){
    case CORE_ARM_GENERIC:
      printf("ARM");
      break;
    case CORE_AMD_ZEN:
      printf("Zen");
      break;
    case CORE_AMD_ZEN5:
      printf("Zen5");
      break;
    case CORE_INTEL_ATOM:
      printf("Atom");
      break;
    case CORE_INTEL_CORE:
      printf("Core");
      break;
    case CORE_AMD_UNKNOWN:
      printf("AMD?");
      break;
    case CORE_INTEL_UNKNOWN:
      printf("Intel?");
      break;
    default:
      printf("??");
      break;
    }
    printf("\n");
  }
}
#endif
