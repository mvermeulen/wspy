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
#include <dirent.h>
#include <sched.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <linux/perf_event.h>
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

static int read_u32_file(const char *path,unsigned int *value){
  FILE *fp;
  unsigned int tmp;

  fp = fopen(path,"r");
  if (!fp) return -1;
  if (fscanf(fp,"%u",&tmp) != 1){
    fclose(fp);
    return -1;
  }
  fclose(fp);
  *value = tmp;
  return 0;
}

static void mark_cpus_for_pmu(const char *cpulist,unsigned int pmu_type,int pmu_cluster){
  const char *p = cpulist;

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

    if (low < 0) low = 0;
    if (high >= (long)cpu_info->num_cores) high = (long)cpu_info->num_cores - 1;
    while (low <= high){
      cpu_info->coreinfo[low].pmu_type = pmu_type;
      cpu_info->coreinfo[low].pmu_cluster = pmu_cluster;
      low++;
    }

    if (*p == ',') p++;
    while (*p == ' ' || *p == '\t') p++;
  }
}

static void discover_arm_pmu_topology(void){
  DIR *dir;
  struct dirent *de;
  unsigned int cluster_count = 0;
  int i;

  for (i=0;i<cpu_info->num_cores;i++){
    cpu_info->coreinfo[i].pmu_type = PERF_TYPE_RAW;
    cpu_info->coreinfo[i].pmu_cluster = -1;
  }

  dir = opendir("/sys/bus/event_source/devices");
  if (!dir){
    warning("unable to read PMU topology from /sys/bus/event_source/devices\n");
    return;
  }

  while ((de = readdir(dir)) != NULL){
    char path[512];
    char cpus_path[512];
    char cpus[256];
    unsigned int pmu_type;
    FILE *fp;

    if (strncmp(de->d_name,"armv8_pmuv3_",12) != 0) continue;

    snprintf(path,sizeof(path),"/sys/bus/event_source/devices/%s/type",de->d_name);
    if (read_u32_file(path,&pmu_type) != 0) continue;

    snprintf(cpus_path,sizeof(cpus_path),"/sys/bus/event_source/devices/%s/cpus",de->d_name);
    fp = fopen(cpus_path,"r");
    if (!fp) continue;
    if (!fgets(cpus,sizeof(cpus),fp)){
      fclose(fp);
      continue;
    }
    fclose(fp);

    mark_cpus_for_pmu(cpus,pmu_type,(int)cluster_count);
    cluster_count++;
  }

  closedir(dir);
  cpu_info->num_pmu_clusters = cluster_count;
}

void print_cpu_pmu_report(FILE *fp){
  int cluster;
  int i;

  if (!cpu_info || cpu_info->vendor != VENDOR_ARM) return;

  fprintf(fp,"ARM PMU topology: %u cluster(s)\n",cpu_info->num_pmu_clusters);
  for (cluster = 0; cluster < (int)cpu_info->num_pmu_clusters; cluster++){
    int printed = 0;
    unsigned int pmu_type = PERF_TYPE_RAW;
    fprintf(fp,"  cluster %d: cpus ",cluster);
    for (i=0;i<cpu_info->num_cores;i++){
      if (cpu_info->coreinfo[i].pmu_cluster != cluster) continue;
      if (!printed){
        pmu_type = cpu_info->coreinfo[i].pmu_type;
      } else {
        fprintf(fp,",");
      }
      fprintf(fp,"%d",i);
      printed = 1;
    }
    if (!printed){
      fprintf(fp,"(none)");
    }
    fprintf(fp," (pmu_type=%u)\n",pmu_type);
  }
  if (cpu_info->mixed_pmu_types){
    fprintf(fp,"  note: available cores span multiple PMU types (big.LITTLE)\n");
  }
}

// AMD's cpuid family 0x1a covers both full-size "Zen5" cores and the
// physically compact "Zen5c" cores used on hybrid parts -- cpuid alone
// can't tell them apart (same family/model), but Zen5c cores are built for
// density over clock speed and so carry a lower max non-boost frequency
// cap than their Zen5 siblings on the same die. This mirrors the
// frequency-clustering heuristic scripts/map_cpu_hierarchy.py already uses
// to label the same host's cores "Zen 5"/"Zen 5c": every family-0x1a core
// starts out CORE_AMD_ZEN5 (the main loop above), then any core whose
// cpuinfo_max_freq reads strictly below the highest value seen among those
// cores is reclassified CORE_AMD_ZEN5C. A host where every Zen5 core
// reports the same max frequency (no Zen5c present, or the kernel doesn't
// expose per-core cpufreq at all) leaves every core CORE_AMD_ZEN5 --
// degrade to "can't distinguish" rather than guess, same idiom as every
// other best-effort sysfs probe in this codebase.
static void resolve_amd_zen5_dense_cores(void){
  unsigned int *freqs;
  unsigned int max_freq = 0;
  unsigned int i;

  freqs = calloc(cpu_info->num_cores,sizeof(unsigned int));
  if (!freqs) return;

  for (i=0;i<cpu_info->num_cores;i++){
    char path[128];

    if (cpu_info->coreinfo[i].vendor != CORE_AMD_ZEN5) continue;
    snprintf(path,sizeof(path),
	     "/sys/devices/system/cpu/cpu%u/cpufreq/cpuinfo_max_freq",i);
    if (read_u32_file(path,&freqs[i]) != 0){
      snprintf(path,sizeof(path),
	       "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_max_freq",i);
      if (read_u32_file(path,&freqs[i]) != 0){
	freqs[i] = 0; // unreadable -- 0 sentinel, never the max
	continue;
      }
    }
    if (freqs[i] > max_freq) max_freq = freqs[i];
  }

  if (max_freq == 0){
    free(freqs);
    return; // no readable frequency anywhere on this family -- can't distinguish
  }

  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].vendor != CORE_AMD_ZEN5) continue;
    if (freqs[i] != 0 && freqs[i] < max_freq){
      cpu_info->coreinfo[i].vendor = CORE_AMD_ZEN5C;
    }
  }
  free(freqs);
}

static enum cpu_core_type resolve_arm_core_type(unsigned int implementer, unsigned int part) {
  if (implementer == 0x41) { /* ARM Ltd */
    switch (part) {
      case 0xd03: return CORE_ARM_CORTEX_A53;
      case 0xd07: return CORE_ARM_CORTEX_A57;
      case 0xd08: return CORE_ARM_CORTEX_A72;
      case 0xd0c: return CORE_ARM_NEOVERSE_N1;
      case 0xd40: return CORE_ARM_NEOVERSE_V1;
      case 0xd49: return CORE_ARM_NEOVERSE_N2;
      case 0xd4f: return CORE_ARM_NEOVERSE_V2;
      case 0xd41: return CORE_ARM_CORTEX_A78;
      case 0xd44: return CORE_ARM_CORTEX_X1;
      case 0xd47: return CORE_ARM_CORTEX_A710;
      case 0xd48: return CORE_ARM_CORTEX_X2;
      case 0xd4a: return CORE_ARM_CORTEX_A510;
      case 0xd80: return CORE_ARM_CORTEX_A520;
      case 0xd81: return CORE_ARM_CORTEX_A720;
      case 0xd82: return CORE_ARM_CORTEX_X4;
    }
  }
  return CORE_ARM_GENERIC;
}

static int read_cpuinfo_core_fields(int corenum, unsigned int *implementer, unsigned int *part) {
  FILE *fp = fopen("/proc/cpuinfo", "r");
  if (!fp) return -1;
  char line[256];
  int current_processor = -1;
  unsigned int found_impl = 0;
  unsigned int found_part = 0;
  int impl_seen = 0;
  int part_seen = 0;

  while (fgets(line, sizeof(line), fp)) {
    if (!strncmp(line, "processor", 9)) {
      char *colon = strchr(line, ':');
      if (colon) {
        current_processor = atoi(colon + 1);
        impl_seen = 0;
        part_seen = 0;
      }
    }
    if (current_processor == corenum) {
      if (!strncmp(line, "CPU implementer", 15)) {
        char *colon = strchr(line, ':');
        if (colon) {
          colon++;
          while (*colon == ' ' || *colon == '\t') colon++;
          found_impl = (unsigned int)strtoul(colon, NULL, 0);
          impl_seen = 1;
        }
      } else if (!strncmp(line, "CPU part", 8)) {
        char *colon = strchr(line, ':');
        if (colon) {
          colon++;
          while (*colon == ' ' || *colon == '\t') colon++;
          found_part = (unsigned int)strtoul(colon, NULL, 0);
          part_seen = 1;
        }
      }
      if (impl_seen && part_seen) {
        *implementer = found_impl;
        *part = found_part;
        fclose(fp);
        return 0;
      }
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
      unsigned int impl = 0, part = 0;
      if (read_cpuinfo_core_fields(i, &impl, &part) == 0) {
        cpu_info->coreinfo[i].vendor = resolve_arm_core_type(impl, part);
      } else {
        cpu_info->coreinfo[i].vendor = CORE_ARM_GENERIC;
      }
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
    cpu_info->coreinfo[i].pmu_type = PERF_TYPE_RAW;
    cpu_info->coreinfo[i].pmu_cluster = -1;
  }

  if (cpu_info->vendor == VENDOR_ARM){
    discover_arm_pmu_topology();
  }
  // fix up Zen5/Zen5c: family 0x1a alone can't distinguish AMD's compact
  // hybrid cores, so a second frequency-based pass reclassifies whichever
  // of the CORE_AMD_ZEN5 cores just assigned above are actually Zen5c --
  // see resolve_amd_zen5_dense_cores()'s own comment for the heuristic.
  if (cpu_info->vendor == VENDOR_AMD && cpu_info->family == 0x1a){
    resolve_amd_zen5_dense_cores();
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

  if (cpu_info->vendor == VENDOR_ARM){
    unsigned int first_type = 0;
    int have_first = 0;
    for (i=0;i<cpu_info->num_cores;i++){
      if (!cpu_info->coreinfo[i].is_available) continue;
      if (!have_first){
        first_type = cpu_info->coreinfo[i].pmu_type;
        have_first = 1;
      } else if (cpu_info->coreinfo[i].pmu_type != first_type){
        cpu_info->mixed_pmu_types = 1;
        break;
      }
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
    case CORE_ARM_CORTEX_A53:
      printf("Cortex-A53");
      break;
    case CORE_ARM_CORTEX_A57:
      printf("Cortex-A57");
      break;
    case CORE_ARM_CORTEX_A72:
      printf("Cortex-A72");
      break;
    case CORE_ARM_NEOVERSE_N1:
      printf("Neoverse-N1");
      break;
    case CORE_ARM_NEOVERSE_V1:
      printf("Neoverse-V1");
      break;
    case CORE_ARM_NEOVERSE_N2:
      printf("Neoverse-N2");
      break;
    case CORE_ARM_NEOVERSE_V2:
      printf("Neoverse-V2");
      break;
    case CORE_ARM_CORTEX_A78:
      printf("Cortex-A78");
      break;
    case CORE_ARM_CORTEX_X1:
      printf("Cortex-X1");
      break;
    case CORE_ARM_CORTEX_A710:
      printf("Cortex-A710");
      break;
    case CORE_ARM_CORTEX_X2:
      printf("Cortex-X2");
      break;
    case CORE_ARM_CORTEX_A510:
      printf("Cortex-A510");
      break;
    case CORE_ARM_CORTEX_A520:
      printf("Cortex-A520");
      break;
    case CORE_ARM_CORTEX_A720:
      printf("Cortex-A720");
      break;
    case CORE_ARM_CORTEX_X4:
      printf("Cortex-X4");
      break;
    case CORE_AMD_ZEN:
      printf("Zen");
      break;
    case CORE_AMD_ZEN5:
      printf("Zen5");
      break;
    case CORE_AMD_ZEN5C:
      printf("Zen5c");
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
    if (cpu_info->vendor == VENDOR_ARM){
      printf(" (pmu_type=%u,cluster=%d)",
             cpu_info->coreinfo[i].pmu_type,
             cpu_info->coreinfo[i].pmu_cluster);
    }
    printf("\n");
  }

  if (cpu_info->vendor == VENDOR_ARM){
    print_cpu_pmu_report(stdout);
  }
}
#endif
