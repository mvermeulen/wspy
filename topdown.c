/*
 * topdown.c - topdown performance counter program
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <linux/perf_event.h>
#include <errno.h>
#include "error.h"
#include "cpu_info.h"

extern char *lookup_vendor();
int num_procs;
int cflag = 0;
int oflag = 0;
int xflag = 0;
int vflag = 0;
struct timespec start_time,finish_time;
enum areamode {
  AREA_ALL,
  AREA_RETIRE,
  AREA_SPEC,
  AREA_FRONTEND,
  AREA_BACKEND,
  AREA_IPC
} area = AREA_ALL;

#define USE_IPC   0x1
#define USE_L1    0x2
#define USE_L2r   0x10
#define USE_L2s   0x20
#define USE_L2f   0x40
#define USE_L2b   0x80
#define USE_L3f   0x400
#define USE_L3b   0x800


// Counter definitions for supported cores
struct counterdef *counters;
/*                                                                              
 * Zen4                                                                         
 *                                                                              
 * instructions = <cpu/instructions>                                            
 *                                                                              
 * cycles = <cpu/cpu-cycles>                                                    
 * slots = 6 * cycles                                                           
 * retire = <ex_ret_ops> / slots                                                
 * frontend = <de_no_dispatch_per_slot.no_ops_from_frontend> / slots            
 * backend = <de_no_dispatch_per_slot.backend_stalls / slots                    
 * speculation = (<de_src_op_disp.all> - <ex_ret_ops>) / slots                  
 * smt = <de_no_dispatch_per_slot.smt_contention> / slots                       
 */
struct counterdef amd_zen_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  { "cpu-cycles",                        0x76, 0,    0,    0,  0,    USE_IPC|USE_L1 },
  { "ex_ret_ops",                        0xc1, 0,    0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.no_ops_from_frontend",
                                         0x1a0,1,    0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.backend_stalls",
                                         0x1a0,0x1e, 0,    0,  0,    USE_L1  },
  { "de_src_op_disp.all",                0xaa, 0x7,  0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.smt_contention",
                                         0x1a0,0x60, 0,    0,  0,    USE_L1  },
};

struct counterdef amd_unknown_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  { "cpu-cycles",                        0x76, 0,    0,    0,  0,    USE_IPC },
  { "ex_ret_ops",                        0xc1, 0,    0,    0,  0,    USE_L1  },
};

/*
 *  Atom                                                                         
 *
 * Note: While there are /sys/devices/cpu_atom entries for ATOM values below,
 *       not able to open counters for Atom cores and perf(1) also says this
 *       isn't allowed, so ignore these on a per-core basis.  Can later try
 *       on a cpu-wide basis...
 * 
 * instructions = <cpu_atom/instructions>                                       
 * slots = retire+frontend+backend+speculation                                  
 * retire = <cpu_atom/topdown-retiring>                                         
 * frontend = <cpu_atom/topdown-fe-bound>                                       
 * backend = <cpu_atom/topdown-be-bound>                                        
 * speculation = <cpu_atom/topdown-bad-spec>
 *
 */
struct counterdef intel_atom_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  { "cpu-cycles",                        0x3c, 0,    0,    0,  0,    USE_IPC },  
  { "topdown-retiring",                  0xc2, 0,    0,    0,  0,    USE_L1  },
  { "topdown-fe-bound",                  0x71, 0,    0,    0,  0,    USE_L1  },
  { "topdown-be-bound",                  0x74, 0,    0,    0,  0,    USE_L1  },
  { "topdown-bad-spec",                  0x73, 0,    0,    0,  0,    USE_L1  },
};

/*
 * Core                                                                         
 * instructions = <cpu_core/instructions>                                       
 * slots = <cpu_core/slots>                                                     
 * retire = <cpu_core/topdown-retiring>                                         
 * frontend = <cpu_core/topdown-fe-bound>                                       
 * backend = <cpu_core/topdown-be-bound>                                        
 * speculation = <cpu_core/topdown-bad-spec>
 */
struct counterdef intel_core_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  { "cpu-cycles",                        0x3c, 0,    0,    0,  0,    USE_IPC },
  { "slots",                             0x00, 4,    0,    0,  0,    USE_L1  },
  { "topdown-retiring",                  0x00, 0x80, 0,    0,  0,    USE_L1  },
  { "topdown-fe-bound",                  0x00, 0x82, 0,    0,  0,    USE_L1  },
  { "topdown-be-bound",                  0x00, 0x83, 0,    0,  0,    USE_L1  },
  { "topdown-bad-spec",                  0x00, 0x81, 0,    0,  0,    USE_L1  },
};

struct counterdef intel_unknown_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  { "cpu-cycles",                        0x3c, 0,    0,    0,  0,    USE_IPC },
  { "topdown-total-slots",               0x3c, 0x0,  0,    1,  2,    USE_L1  },
  { "topdown-fetch-bubbles",             0x9c, 0x1,  0,    0,  0,    USE_L1  },
  { "topdown-recovery-bubbles",          0xd,  0x3,  0x1,  1,  2,    USE_L1  },
  { "topdown-slots-issued",              0xe,  0x1,  0,    0,  0,    USE_L1  },
  { "topdown-slots-retired",             0xc2, 0x2,  0,    0,  0,    USE_L1  },
  { "resource-stalls.sb",                0xa2, 0x8,  0,    0,  0,    USE_L2b },
  { "cycle-activity.stalls-ldm-pending", 0xa3, 0x6,  0x6,  0,  0,    USE_L2b },
  { "idq_uops_not_delivered.0_uops",     0x9c, 0x1,  0x4,  0,  0,    USE_L2f },
  { "idq_uops_not_delivered.1_uops",     0x9c, 0x1,  0x3,  0,  0,    USE_L2f },
  { "idq_uops_not_delivered.2_uops",     0x9c, 0x1,  0x2,  0,  0,    USE_L2f },
  { "idq_uops_not_delivered.3_uops",     0x9c, 0x1,  0x1,  0,  0,    USE_L2f },
  { "branch-misses",                     0xc5, 0x1,  0,    0,  0,    USE_L2s },
  { "machine_clears.count",              0xc3, 0x1,  0x1,  0,  0,    USE_L2s },
  { "idq.ms_uops",                       0x79, 0x30, 0,    0,  0,    USE_L2r },
  { "icache.ifdata_stall",               0x80, 0x4,  0,    0,  0,    USE_L3f },
  { "itlb_misses.stlb_hit",              0x85, 0x60, 0,    0,  0,    USE_L3f },
  { "itlb_misses.walk_duration",         0x85, 0x10, 0,    0,  0,    USE_L3f },
  { "idq.dsb_uops",                      0x79, 0x8,  0,    0,  0,    USE_L3f },
  { "l2_rqsts.reference" ,               0x24, 0xff, 0,    0,  0,    USE_L3b },
  { "l2_rqsts.miss",                     0x24, 0x3f, 0,    0,  0,    USE_L3b },
  { "longest_lat_cache.reference",       0x2e, 0x4f, 0,    0,  0,    USE_L3b },
  { "longest_lat_cache.miss",            0x2e, 0x41, 0,    0,  0,    USE_L3b },
};

struct counterdata {
  unsigned long int value;
  int fd;
  int corenum;
  struct counterdef *definition;
};
struct counterdata *app_counters = NULL;
int num_core_counters = 0;
int num_total_counters = 0;
  
int command_line_argc;
char **command_line_argv;
pid_t child_pid = 0;

int level = 1;
FILE *outfile;

int parse_options(int argc,char *const argv[]){
  FILE *fp;
  int opt;
  int i;
  unsigned int lev;
  while ((opt = getopt(argc,argv,"+abcfil:o:rsvx")) != -1){
    switch (opt){
    case 'a':
      area = AREA_ALL;
      break;
    case 'b':
      area = AREA_BACKEND;
      break;
    case 'c':
      cflag = 1;
      break;
    case 'f':
      area = AREA_FRONTEND;
      break;
    case 'i':
      area = AREA_IPC;
      break;
    case 'l':
      if (sscanf(optarg,"%u",&lev) == 1){
	if (lev >= 1 && lev <= 4){
	  level = lev;
	} else {
	  error("incorrect option to -l: %u, ignored\n",lev);
	}
      } else {
	error("incorrect option to -l: %s, ignored\n",optarg);	
      }
      break;
    case 'o':
      fp = fopen(optarg,"w");
      if (!fp){
	error("can not open file: %s\n",optarg);
      } else {
	outfile = fp;
	oflag = 1;
      }
      break;
    case 'r':
      area = AREA_RETIRE;
      break;
    case 's':
      area = AREA_SPEC;
      break;
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 'x':
      xflag = 1;
      break;
    default:
      warning("unknown option: %c\n",opt);
      return 1;
    }
  }
  if (optind >= argc){
    warning("missing command after options\n");
    return 1;
  }
  command_line_argv = calloc(argc-optind+1,sizeof(char *));
  command_line_argc = argc - optind;
  for (i=0;i<command_line_argc;i++){
    command_line_argv[i] = argv[i+optind];
  }
  return 0;
}

int launch_child(int argc,char *const argv[],char *const envp[]){
  pid_t child;
  int len;
  char *p,*path;
  char pathbuf[1024];
  
  switch(child = fork()){
  case 0: // child
    execve(argv[0],argv,envp);
    // if argv[0] fails, look in path
    path = strdup(getenv("PATH"));
    len = strlen(argv[0]);
    p = strtok(path,":\n");
    if (p){
      do {
	strncpy(pathbuf,p,sizeof(pathbuf)-len-2);
	strcat(pathbuf,"/");
	strcat(pathbuf,argv[0]);
	execve(pathbuf,argv,envp);
	p = strtok(NULL,":\n");
      } while (p);
      // exec failed
      return 1;
    }
    break;
  case -1:
    fatal("fork failed\n");
    return 1;
  default:
    // parent
    child_pid = child;
    break;
  }
  return 0;
}

// syscall wrapper since not part of glibc
int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		    int cpu, int group_fd, unsigned long flags){
  int ret;
  debug2("perf_event_open(event=<type=%d, config=%x, format=%x, size=%d, disabled=%d>, pid=%d, cpu=%d, group_fd=%d, flags=%x)\n",
	hw_event->type,hw_event->config, hw_event->read_format, hw_event->size, hw_event->disabled,
	pid,cpu,group_fd,flags);
  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}

void setup_counters(char *vendor){
  unsigned int mask = 0;
  int i,j,k;
  int count;
  int nerror = 0;
  int status;
  int groupid;
  struct perf_event_attr pe;
  // set the mask
  switch(area){
  case AREA_ALL:
    mask = USE_L1;
    if (level > 1)
      mask = mask | USE_L2r | USE_L2s | USE_L2f | USE_L2b;
    if (level > 2)
      mask = mask | USE_L3f | USE_L3b;
    break;
  case AREA_RETIRE:
    mask = USE_L1;
    if (level > 1)
      mask = mask | USE_L2r;
    break;
  case AREA_SPEC:
    mask = USE_L1;
    if (level > 1)
      mask = mask | USE_L2s;
    break;
  case AREA_FRONTEND:
    mask = USE_L1;
    if (level > 1)
      mask = mask | USE_L2f;
    if (level > 2)
      mask = mask | USE_L3f;
    break;
  case AREA_BACKEND:
    mask = USE_L1;
    if (level > 1)
      mask = mask | USE_L2b;
    if (level > 2)
      mask = mask | USE_L3b;
    break;
  case AREA_IPC:
    mask = USE_IPC;
    break;
  }

  struct cpu_core_info *coreinfo;
  struct counterdef *counterdef;
  int ncounters;
  if (!cpu_info) inventory_cpu();
  
  for (i=0;i< cpu_info->num_cores;i++){
    coreinfo = &cpu_info->coreinfo[i];
    if (!coreinfo->is_available){
      coreinfo->ncounters = 0;
      continue;
    }
    // pick the list of counters
    switch(coreinfo->vendor){
    case CORE_AMD_ZEN:
      counterdef = amd_zen_counters;
      ncounters = sizeof(amd_zen_counters)/sizeof(amd_zen_counters[0]);
      break;
    case CORE_INTEL_ATOM:
      // Ignore Atom cores since perf_event_open(2) doesn't seem to allow this
      //      counterdef = intel_atom_counters;
      //      ncounters = sizeof(amd_zen_counters)/sizeof(amd_zen_counters[0]);
      continue;
    case CORE_INTEL_CORE:
      counterdef = intel_core_counters;
      ncounters = sizeof(amd_zen_counters)/sizeof(amd_zen_counters[0]);
      break;
    default:
      continue;
    }
    // count the # of performance counters
    count = 0;
    groupid = -1;
    for (j=0;j<ncounters;j++){
      if (mask & counterdef[j].use) count++;
    }
    coreinfo->ncounters = count;
    // allocate space
    coreinfo->counters = calloc(count,sizeof(struct counter_info));

    // set up counter
    count = 0;
    for (j=0;j<ncounters;j++){
      if (mask & counterdef[j].use){
	coreinfo->counters[count].corenum = i;
	coreinfo->counters[count].cdef = &counterdef[j];
	// set up the counter and leave it disabled
	debug("core %d creating counter %s\n",i,counterdef[j].name);
	memset(&pe,0,sizeof(pe));
	pe.type = PERF_TYPE_RAW;
	pe.config = (counterdef[j].event&0xff) |
	  (counterdef[j].umask<<8) |
	  (counterdef[j].any<<21) |
	  (counterdef[j].cmask<<24) |
	  (counterdef[j].event&0xf00)<<24;
	pe.sample_type = PERF_SAMPLE_IDENTIFIER;
	pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
	pe.size = sizeof(struct perf_event_attr);
	pe.exclude_guest=1;
	pe.disabled = 1;
	status = perf_event_open(&pe,-1,i,groupid,PERF_FLAG_FD_CLOEXEC);
	if (status == -1){
	  error("unable to open performance counter cpu=%d, name=%s, errno=%d - %s\n",i, counterdef[j].name, errno,strerror(errno));
	  nerror++;
	} else {
	  if (groupid==-1) groupid = status;
	  coreinfo->counters[count].fd = status;
	  ioctl(coreinfo->counters[count].fd,PERF_EVENT_IOC_ENABLE,0);
	}
	count++;
      }
    }
  }
  if (nerror) fatal("unable to open performance counters\n");
}

void start_counters(void){
  int i,j;
  int status;
  struct counter_info *cinfo;
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].ncounters == 0) continue;
    for (j=0;j<cpu_info->coreinfo[i].ncounters;j++){
      cinfo = &cpu_info->coreinfo[i].counters[j];
      status = ioctl(cinfo->fd,PERF_EVENT_IOC_ENABLE,0);
      if (status != 0)
	error("unable to start counter %s on core %d\n",cinfo->cdef->name,cinfo->corenum);
    }
  }
}

void stop_counters(void){
  int i,j;
  int status;
  struct read_format { uint64_t value, time_enabled, time_running,id; } rf;
  
  struct counter_info *cinfo;
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].ncounters == 0) continue;
    for (j=0;j<cpu_info->coreinfo[i].ncounters;j++){
      cinfo = &cpu_info->coreinfo[i].counters[j];
      status = read(cinfo->fd,&rf,sizeof(rf));
      
      if (status == -1){
	error("unable to read counter %s on core %d, fd=%d errno=%d - %s\n",cinfo->cdef->name,cinfo->corenum,
	      cinfo->fd, errno, strerror(errno));	
      } else {
	cinfo->value = rf.value * ((double) rf.time_enabled / rf.time_running);
	if (cinfo->cdef->scale){
	  cinfo->value *= cinfo->cdef->scale;
	}
	status = ioctl(cinfo->fd,PERF_EVENT_IOC_DISABLE,0);
      }
    }
  }
}

void dump_counters(void){
  int i;
  for (i=0;i<num_total_counters;i++){
    notice("core=%d, counter=%s: \t%lu\n",
	   app_counters[i].corenum,app_counters[i].definition->name,
	   app_counters[i].value);
  }
}

void print_usage(struct rusage *rusage){
  double elapsed;
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
  fprintf(outfile,"on_cpu         %4.3f\n",
	  (rusage->ru_utime.tv_sec+rusage->ru_utime.tv_usec/1000000.0+
	   rusage->ru_stime.tv_sec+rusage->ru_stime.tv_usec/1000000.0)/
	  elapsed / num_procs);
  fprintf(outfile,"elapsed        %4.3f\n",elapsed);
  fprintf(outfile,"utime          %4.3f\n",
	  (double) rusage->ru_utime.tv_sec +
	  rusage->ru_utime.tv_usec / 1000000.0);
  fprintf(outfile,"stime          %4.3f\n",
	  (double) rusage->ru_stime.tv_sec +
	  rusage->ru_stime.tv_usec / 1000000.0);
  fprintf(outfile,"nvcsw          %lu (%4.2f%%)\n",
	  rusage->ru_nvcsw,(double) rusage->ru_nvcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"nivcsw         %lu (%4.2f%%)\n",
	  rusage->ru_nivcsw,(double) rusage->ru_nivcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"inblock        %lu\n",rusage->ru_inblock);
  fprintf(outfile,"onblock        %lu\n",rusage->ru_oublock);  
}

unsigned long int sum_counters(char *cname){
  int i,j;
  int found=0;
  unsigned long int total = 0;
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].ncounters == 0) continue;
    for (j=0;j<cpu_info->coreinfo[i].ncounters;j++){
      if (!strcmp(cname,cpu_info->coreinfo[i].counters[j].cdef->name)){
	// assumes it was aleady read by stop_counters
	total += cpu_info->coreinfo[i].counters[j].value;
	found = 1;
      }
    }
  }
  if (!found)
    warning("no counters named %s found\n",cname);
  return total;
}

void print_ipc(){
  unsigned long int total_instructions = sum_counters("instructions");
  unsigned long int total_cpu_cycles = sum_counters("cpu-cycles");
  fprintf(outfile,"IPC\t%4.3f\n",
	  (double) total_instructions / total_cpu_cycles);
}

void print_topdown(){
  switch (cpu_info->vendor){
  case VENDOR_INTEL:
    if (cpu_info->family == 6 && cpu_info->model == 0xba){
      // Raptor Lake
      unsigned long int slots = sum_counters("slots");
      unsigned long int retiring = sum_counters("topdown-retiring");
      unsigned long int fe_bound = sum_counters("topdown-fe-bound");
      unsigned long int be_bound = sum_counters("topdown-be-bound");
      unsigned long int bad_spec = sum_counters("topdown-bad-spec");
      fprintf(outfile,"retire       %4.3f\n",
	      (double) retiring / slots);
      fprintf(outfile,"speculation  %4.3f\n",
	      (double) bad_spec / slots);
      fprintf(outfile,"frontend     %4.3f\n",
	      (double) fe_bound / slots);
      fprintf(outfile,"backend      %4.3f\n",
	      (double) be_bound / slots);
    }
    break;
  case VENDOR_AMD:
    if (cpu_info->family == 0x17 || cpu_info->family == 0x19){
      // Zen
    }
    break;
  }
}

void print_amd_topdown(void){
}

void print_intel_topdown(int ncpu){
  // Note: Assumes two hyperthreads per core with numbering of core 0 first and core 1 second  
  int i;  
  unsigned long int topdown_total_slots[ncpu/2];
  unsigned long int topdown_fetch_bubbles[ncpu/2];
  unsigned long int topdown_recovery_bubbles[ncpu/2];
  unsigned long int topdown_slots_issued[ncpu/2];
  unsigned long int topdown_slots_retired[ncpu/2];
  unsigned long int resource_stalls_sb[ncpu/2];
  unsigned long int stalls_ldm_pending[ncpu/2];
  unsigned long int uops0_delivered[ncpu/2],uops1_delivered[ncpu/2],uops2_delivered[ncpu/2],uops3_delivered[ncpu/2];
  unsigned long int branch_misses[ncpu/2],machine_clears[ncpu/2],ms_uops[ncpu/2];
  unsigned long int icache_stall[ncpu/2],itlb_stlb_hit[ncpu/2],itlb_walk_duration[ncpu/2],dsb_uops[ncpu/2];
  unsigned long int l2_refs[ncpu/2],l2_misses[ncpu/2],l3_refs[ncpu/2],l3_misses[ncpu/2];
  unsigned long int total_topdown_total_slots=0,total_topdown_fetch_bubbles=0,
    total_topdown_recovery_bubbles=0,total_topdown_slots_issued=0,total_topdown_slots_retired=0,
    total_resource_stalls_sb=0,total_stalls_ldm_pending=0,
    total_uops0_delivered=0,total_uops1_delivered=0,total_uops2_delivered=0,total_uops3_delivered=0,
    total_branch_misses=0,total_machine_clears=0,total_ms_uops=0,
    total_icache_stall=0,total_itlb_stlb_hit=0,total_itlb_walk_duration=0,total_dsb_uops = 0,
    total_l2_refs=0,total_l2_misses=0,total_l3_refs=0,total_l3_misses=0;
  double frontend_bound,retiring,speculation,backend_bound;
  for (i=0;i<ncpu/2;i++){
    topdown_total_slots[i] = 0;
    topdown_fetch_bubbles[i] = 0;
    topdown_recovery_bubbles[i] = 0;
    topdown_slots_issued[i] = 0;
    topdown_slots_retired[i] = 0;
    resource_stalls_sb[i] = 0;
    stalls_ldm_pending[i] = 0;
    uops0_delivered[i] = 0;
    branch_misses[i] = 0;
    machine_clears[i] = 0;
    icache_stall[i] = 0;
    itlb_stlb_hit[i] = 0;
    itlb_walk_duration[i] = 0;
    dsb_uops[i] = 0;
    l2_refs[i] = 0;
    l2_misses[i] = 0;
    l3_refs[i] = 0;
    l3_misses[i] = 0;
  }
  
  for (i=0;i<num_total_counters;i++){
    if (!strcmp(app_counters[i].definition->name,"topdown-total-slots")){
      topdown_total_slots[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_topdown_total_slots += app_counters[i].value;
    } else if (!strcmp(app_counters[i].definition->name,"topdown-fetch-bubbles")){
      topdown_fetch_bubbles[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_topdown_fetch_bubbles += app_counters[i].value;      
    } else if (!strcmp(app_counters[i].definition->name,"topdown-recovery-bubbles")){
      topdown_recovery_bubbles[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_topdown_recovery_bubbles += app_counters[i].value;            
    } else if (!strcmp(app_counters[i].definition->name,"topdown-slots-issued")){
      topdown_slots_issued[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_topdown_slots_issued += app_counters[i].value;                  
    } else if (!strcmp(app_counters[i].definition->name,"topdown-slots-retired")){
      topdown_slots_retired[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_topdown_slots_retired += app_counters[i].value;                        
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"resource-stalls.sb")){
      resource_stalls_sb[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_resource_stalls_sb += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"cycle-activity.stalls-ldm-pending")){
      stalls_ldm_pending[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_stalls_ldm_pending += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.0_uops")){
      uops0_delivered[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_uops0_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.1_uops")){
      uops1_delivered[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_uops1_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.2_uops")){
      uops2_delivered[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_uops2_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.3_uops")){
      uops3_delivered[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_uops3_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"branch-misses")){
      branch_misses[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_branch_misses += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"machine_clears.count")){
      machine_clears[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_machine_clears += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq.ms_uops")){
      ms_uops[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_ms_uops += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"icache.ifdata_stall")){
      icache_stall[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_icache_stall += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"itlb_misses.stlb_hit")){
      itlb_stlb_hit[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_itlb_stlb_hit += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"itlb_misses.walk_duration")){
      itlb_walk_duration[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_itlb_walk_duration += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"idq.dsb_uops")){
      dsb_uops[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_dsb_uops += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"l2_rqsts.reference")){
      l2_refs[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_l2_refs += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"l2_rqsts.miss")){
      l2_misses[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_l2_misses += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"longest_lat_cache.reference")){
      l3_refs[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_l3_refs += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"longest_lat_cache.miss")){
      l3_misses[app_counters[i].corenum % (ncpu/2)] += app_counters[i].value;
      total_l3_misses += app_counters[i].value;
    }
  } 
  frontend_bound = (double) total_topdown_fetch_bubbles / total_topdown_total_slots;
  retiring = (double) total_topdown_slots_retired / total_topdown_total_slots;
  speculation = (double) (total_topdown_slots_issued - total_topdown_slots_retired + total_topdown_recovery_bubbles)/ total_topdown_total_slots;
  backend_bound = 1 - (frontend_bound + retiring + speculation);
  fprintf(outfile,"retire         %4.3f\n",retiring);
  if ((level > 1) && total_ms_uops){
    fprintf(outfile,"ms_uops                %4.3f\n",(double) total_ms_uops / total_topdown_total_slots);
  }
  fprintf(outfile,"speculation    %4.3f\n",speculation);
  if ((level > 1) && (total_machine_clears + total_branch_misses)){
    fprintf(outfile,"branch_misses          %2.2f%%\n",(double) total_branch_misses / (total_machine_clears + total_branch_misses)*100);
    fprintf(outfile,"machine_clears         %2.2f%%\n",(double) total_machine_clears / (total_machine_clears + total_branch_misses)*100);    
  }
  fprintf(outfile,"frontend       %4.3f\n",frontend_bound);
  if ((level > 1) && total_uops0_delivered){
    fprintf(outfile,"idq_uops_delivered_0   %4.3f\n",(double) total_uops0_delivered * 2 / total_topdown_total_slots);
  }
  if ((level > 2) && total_icache_stall){
    fprintf(outfile,"icache_stall               %4.3f\n",(double) total_icache_stall * 2 / total_topdown_total_slots);    
  }
  if ((level > 2) && (total_itlb_stlb_hit+total_itlb_walk_duration)){
    fprintf(outfile,"itlb_misses                %4.3f\n",
	    (double) (total_itlb_stlb_hit * 14 + total_itlb_walk_duration) * 2 / total_topdown_total_slots);    
  }
  if ((level > 1) && total_uops1_delivered){
    fprintf(outfile,"idq_uops_delivered_1   %4.3f\n",(double) total_uops1_delivered * 2 / total_topdown_total_slots);
  }
  if ((level > 1) && total_uops2_delivered){
    fprintf(outfile,"idq_uops_delivered_2   %4.3f\n",(double) total_uops2_delivered * 2 / total_topdown_total_slots);
  }
  if ((level > 1) && total_uops3_delivered){
    fprintf(outfile,"idq_uops_delivered_3   %4.3f\n",(double) total_uops3_delivered * 2 / total_topdown_total_slots);
  }
  if ((level > 2) && total_dsb_uops){
    fprintf(outfile,"dsb_ops                    %2.2f%%\n",
	    (double) total_dsb_uops / total_topdown_slots_issued * 100.0);
  }
  fprintf(outfile,"backend        %4.3f\n",backend_bound);
  if ((level > 1) && total_resource_stalls_sb){
    fprintf(outfile,"resource_stalls.sb     %4.3f\n",(double) total_resource_stalls_sb * 2 / total_topdown_total_slots);
  }
  if ((level > 1) && total_stalls_ldm_pending){
    fprintf(outfile,"stalls_ldm_pending     %4.3f\n",(double) total_stalls_ldm_pending * 2 / total_topdown_total_slots);    
  }
  if ((level > 2) && total_l2_refs){
    fprintf(outfile,"l2_refs                    %4.3f\n",
	    (double) total_l2_refs * 2 / total_topdown_total_slots);
  }
  if ((level > 2) && total_l2_misses){
    fprintf(outfile,"l2_misses                  %4.3f\n",
	    (double) total_l2_misses * 2 / total_topdown_total_slots);
    if (total_l2_refs)
      fprintf(outfile,"l2_miss_ratio              %2.2f%%\n",
	      (double) total_l2_misses / total_l2_refs * 100.0);            
  }
  if ((level > 2) && total_l3_refs){
    fprintf(outfile,"l3_refs                    %4.3f\n",
	    (double) total_l3_refs * 2 / total_topdown_total_slots);
  }
  if ((level > 2) && total_l3_misses){
    fprintf(outfile,"l3_misses                  %4.3f\n",
	    (double) total_l3_misses * 2 / total_topdown_total_slots);
    if (total_l3_refs)
      fprintf(outfile,"l3_miss_ratio              %2.2f%%\n",
	      (double) total_l3_misses / total_l3_refs * 100.0);      
  }  
  if (cflag){
    for (i=0;i<ncpu/2;i++){
      frontend_bound = (double) topdown_fetch_bubbles[i] / topdown_total_slots[i];
      retiring = (double) topdown_slots_retired[i] / topdown_total_slots[i];
      speculation = (double) (topdown_slots_issued[i] - topdown_slots_retired[i] + topdown_recovery_bubbles[i])/ topdown_total_slots[i];
      backend_bound = 1 - (frontend_bound + retiring + speculation);
      fprintf(outfile,"%d.retire       %4.3f\n",i,retiring);
      if ((level > 1) && ms_uops[i]){
	fprintf(outfile,"%d.ms_uops                %4.3f\n",i,
		(double) ms_uops[i] / topdown_total_slots[i]);
      }      
      fprintf(outfile,"%d.speculation  %4.3f\n",i,speculation);
      if ((level > 1) && (machine_clears[i] + branch_misses[i])){
	fprintf(outfile,"%d.branch_misses          %2.2f%%\n",i,
		(double) branch_misses[i] / (machine_clears[i] + branch_misses[i])*100);
	fprintf(outfile,"%d.machine_clears         %2.2f%%\n",i,
		(double) machine_clears[i] / (machine_clears[i] + branch_misses[i])*100);    
      }      
      fprintf(outfile,"%d.frontend     %4.3f\n",i,frontend_bound);
      if ((level > 1) && uops0_delivered[i]){
	fprintf(outfile,"%d.idq_uops_delivered_0   %4.3f\n",i,
		(double) uops0_delivered[i] * 2 / topdown_total_slots[i]);
      }
      if ((level > 2) && icache_stall[i]){
	fprintf(outfile,"%d.icache_stall               %4.3f\n",i,
		(double) icache_stall[i] * 2 / topdown_total_slots[i]);	
      }
      if ((level > 2) && (itlb_stlb_hit[i] + itlb_walk_duration[i])){
	fprintf(outfile,"%d.itlb_miss                  %4.3f\n",i,
		(double) (itlb_stlb_hit[i]*14 + itlb_walk_duration[i]) * 2 / topdown_total_slots[i]);		
      }
      if ((level > 1) && uops1_delivered[i]){
	fprintf(outfile,"%d.idq_uops_delivered_1   %4.3f\n",i,
		(double) uops1_delivered[i] * 2 / topdown_total_slots[i]);
      }      
      if ((level > 1) && uops2_delivered[i]){
	fprintf(outfile,"%d.idq_uops_delivered_2   %4.3f\n",i,
		(double) uops2_delivered[i] * 2 / topdown_total_slots[i]);
      }      
      if ((level > 1) && uops3_delivered[i]){
	fprintf(outfile,"%d.idq_uops_delivered_3   %4.3f\n",i,
		(double) uops3_delivered[i] * 2 / topdown_total_slots[i]);
      }
      if ((level > 2) && dsb_uops[i]){
	fprintf(outfile,"%d.dsb_uops                   %2.2f%%\n",i,
		(double) dsb_uops[i] / topdown_slots_issued[i] * 100.0);
      }
      fprintf(outfile,"%d.backend      %4.3f\n",i,backend_bound);
      if ((level > 1) && resource_stalls_sb[i]){
	fprintf(outfile,"%d.resource_stalls.sb     %4.3f\n",i,
		(double) resource_stalls_sb[i] * 2 / topdown_total_slots[i]);
      }
      if ((level > 1) && stalls_ldm_pending[i]){
	fprintf(outfile,"%d.stalls_ldm_pending     %4.3f\n",i,
		(double) stalls_ldm_pending[i] * 2 / topdown_total_slots[i]);    
  }      
    }
  }
}

int main(int argc,char *const argv[],char *const envp[]){
  int status;
  struct rusage rusage;
  outfile = stdout;
  num_procs = get_nprocs();
  if (parse_options(argc,argv)){
      fatal("usage: %s -[abcfrsx][-l <1|2|3|4>][-o <file>] <cmd><args>...\n"
	    "\t-l <level> - expand out <level> levels (default 1)\n"
	    "\t-c         - show cores as separate\n"
	    "\t-o <file>  - send output to <file>\n"
	    "\t-a         - expand all areas\n"
	    "\t-b         - expand backend stalls area\n"
	    "\t-f         - expand frontend stalls area\n"
	    "\t-r         - expand retiring area\n"
	    "\t-s         - expand speculation area\n"
	    "\t-i         - print IPC\n"
	    "\t-x	  - print system info\n"
	    "\t-v         - print verbose information\n"
	    ,argv[0]);
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  char *vendor = lookup_vendor();

  setup_counters(vendor);

  start_counters();

  clock_gettime(CLOCK_REALTIME,&start_time);
  if (launch_child(command_line_argc,command_line_argv,envp)){
    fatal("unable to launch %s\n",command_line_argv[0]);
  }
  wait4(child_pid,&status,0,&rusage);

  stop_counters();
  clock_gettime(CLOCK_REALTIME,&finish_time);  

  if (xflag){
    print_usage(&rusage);
  }

  //  dump_counters();

  if (area == AREA_IPC){
    print_ipc(num_procs);
  } else {
    print_topdown();
#if 0
    if (vendor && !strcmp(vendor,"GenuineIntel")){
      print_intel_topdown(num_procs);
    } else if (vendor && !strcmp(vendor,"AuthenticAMD")){
      print_amd_topdown();
    }
#endif
  }
  
  if (oflag) fclose(outfile);
  return 0;
}
