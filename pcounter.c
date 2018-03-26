/*
 * pcounter.c - performance counter subsystem
 *
 * Implementation of a periodic sampling of performance counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <linux/perf_event.h>
#include "wspy.h"
#include "error.h"

FILE *perfctrfile = NULL;

#define MAX_COUNTERS_PER_CORE 4
struct perf_config {
  char *label;
  uint32_t type;
  uint64_t config;
};
struct core_perf_config {
  int corenum;
  int ncount;
  struct perf_config counter[MAX_COUNTERS_PER_CORE];
} default_config[] = {
  { 0, 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "cacheref", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES },
     { "cachemiss", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES }}},
  { 1, 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "branch", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
     { "branchmiss", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES }}},
  { 2, 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "stallfe", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_FRONTEND },
     { "stallbe", PERF_TYPE_HARDWARE, PERF_COUNT_HW_STALLED_CYCLES_BACKEND }}},
  { 3, 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "l1dref", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_L1D)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16) },
     { "l1dmiss", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_L1D)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_MISS<<16) }}}
};

struct perfctr_info {
  int fd;
  int corenum;
  struct perf_config pconfig;
  long value;
  struct perfctr_info *prev;
  struct perfctr_info *next;  
};

static struct perfctr_info *performance_counters = NULL;
static int num_procs = 0;

// syscall wrapper since not part of glibc
int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		    int cpu, int group_fd, unsigned long flags){
  int ret;
  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}

void init_perf_counters(){
  int i,j,confignum;
  int status;
  struct perf_event_attr pe;
  struct perfctr_info *pi;
  struct perfctr_info *last_pi = NULL, *first_pi = NULL;
  perfctrfile = tmpfile();
  num_procs = get_nprocs();
  for (i=0;i<num_procs;i++){
    confignum = i % (sizeof(default_config)/sizeof(default_config[0]));
    for (j=0;j < default_config[confignum].ncount;j++){
      memset(&pe,0,sizeof(pe));
      pe.type = default_config[confignum].counter[j].type;
      pe.config = default_config[confignum].counter[j].config;
      pe.size = sizeof(struct perf_event_attr);
      pe.disabled = 1;
      pe.exclude_kernel = 0;
      pe.exclude_hv = 0;
      pe.exclude_idle = 1;
      pi = calloc(1,sizeof(struct perfctr_info));
      pi->corenum = i;
      pi->pconfig = default_config[confignum].counter[j];
      pi->value = 0;
      status = perf_event_open(&pe,-1,i,-1,0);
      if (status == -1){
	error("unable to open performance counter pid=%d cpu=%d type=%d config=%d errno=%d %s\n",
	      -1,i,pe.type,pe.config,errno,strerror(errno));
	free(pi);
      } else {
	pi->fd = status;
	if (first_pi == NULL) first_pi = pi;
	if (last_pi != NULL){
	  pi->prev = last_pi;
	  last_pi->next = pi;
	}
	last_pi = pi;
      }
    }
  }
  performance_counters = first_pi;
  // walking through and start all the counters
  for (pi = performance_counters;pi;pi = pi->next){
    status = ioctl(pi->fd,PERF_EVENT_IOC_RESET,0);
    if (status == -1){
      error("reset of %s_%d returns -1, errno = %d %s\n",
	    pi->pconfig.label,
	    pi->corenum,
	    errno,
	    strerror(errno));
    }
    status = ioctl(pi->fd,PERF_EVENT_IOC_ENABLE,0);
  }
}

void read_perf_counters(double time){
  struct perfctr_info *pi;
  int status;
  
  fprintf(perfctrfile,"time %f\n",time);
  for (pi = performance_counters;pi;pi = pi->next){
    status = read(pi->fd,&pi->value,sizeof(pi->value));
    fprintf(perfctrfile,"%s_%d %ld\n",pi->pconfig.label,pi->corenum,pi->value);
    debug("%s_%d %ld\n",pi->pconfig.label,pi->corenum,pi->value);
  }
}
