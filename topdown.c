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

int num_procs;
int aflag = 0;
int oflag = 0;
int xflag = 1;
int vflag = 0;

#define COUNTER_IPC         0x1
#define COUNTER_TOPDOWN     0x2
#define COUNTER_SOFTWARE    0x10
unsigned int counter_mask = COUNTER_IPC;

struct timespec start_time,finish_time;

// Counter definitions for supported cores
struct counter_def *counters;
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
 */
struct counter_def amd_zen_counters[] = {
  // name                                event umask cmask any scale mask
  { "cpu-cycles",                        0x76, 0,    0,    0,  0,    COUNTER_IPC | COUNTER_TOPDOWN },
  { "instructions",                      0xc0, 0,    0,    0,  0,    COUNTER_IPC },
  { "ex_ret_ops",                        0xc1, 0,    0,    0,  0,    COUNTER_IPC  },
  { "branch-instructions",               0xc2, 0,    0,    0,  0,    COUNTER_IPC },
  { "de_no_dispatch_per_slot.no_ops_from_frontend",
                                         0x1a0,1,    0,    0,  0,    COUNTER_TOPDOWN  },
  { "de_no_dispatch_per_slot.backend_stalls",
                                         0x1a0,0x1e, 0,    0,  0,    COUNTER_TOPDOWN  },
  { "de_src_op_disp.all",                0xaa, 0x7,  0,    0,  0,    COUNTER_TOPDOWN  },
};

struct counter_def amd_unknown_counters[] = {
  // name                                event umask cmask any scale use
  { "cpu-cycles",                        0x76, 0,    0,    0,  0,    COUNTER_IPC | COUNTER_IPC },
  { "instructions",                      0xc0, 0,    0,    0,  0,    COUNTER_IPC },
  { "ex_ret_ops",                        0xc1, 0,    0,    0,  0,    COUNTER_IPC  },
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
struct counter_def intel_atom_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    COUNTER_IPC },
  { "cpu-cycles",                        0x3c, 0,    0,    0,  0,    COUNTER_IPC|COUNTER_TOPDOWN },  
  { "topdown-retiring",                  0xc2, 0,    0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-fe-bound",                  0x71, 0,    0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-be-bound",                  0x74, 0,    0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-bad-spec",                  0x73, 0,    0,    0,  0,    COUNTER_TOPDOWN  },
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
struct counter_def intel_core_counters[] = {
  // name                                event umask cmask any scale use
  { "instructions",                      0xc0, 0,    0,    0,  0,    COUNTER_IPC },
  { "cpu-cycles",                        0x3c, 0,    0,    0,  0,    COUNTER_IPC },
  { "slots",                             0x00, 4,    0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-retiring",                  0x00, 0x80, 0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-fe-bound",                  0x00, 0x82, 0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-be-bound",                  0x00, 0x83, 0,    0,  0,    COUNTER_TOPDOWN  },
  { "topdown-bad-spec",                  0x00, 0x81, 0,    0,  0,    COUNTER_TOPDOWN  },
};

#if 0
struct counter_def intel_unknown_counters[] = {
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
#endif

int command_line_argc;
char **command_line_argv;
pid_t child_pid = 0;

FILE *outfile;

int parse_options(int argc,char *const argv[]){
  FILE *fp;
  int opt;
  int i;
  unsigned int lev;
  while ((opt = getopt(argc,argv,"+AaIio:SsTtvXx")) != -1){
    switch (opt){
    case 'a':
      aflag = 0;
      break;
    case 'A':
      aflag = 1;
      break;
    case 'I':
      counter_mask |= COUNTER_IPC;
      break;
    case 'i':
      counter_mask &= (~COUNTER_IPC);
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
    case 'S':
      counter_mask |= COUNTER_SOFTWARE;
      break;      
    case 's':
      counter_mask &= (~COUNTER_SOFTWARE);
      break;
    case 'T':
      counter_mask |= COUNTER_TOPDOWN;
      break;
    case 't':
      counter_mask &= (~COUNTER_TOPDOWN);
      break;
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 'X':
      xflag = 1;
      break;
    case 'x':
      xflag = 0;
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
  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  
  debug2("perf_event_open(event=<type=%d, config=%lx, format=%x, size=%d, disabled=%d>, pid=%d, cpu=%d, group_fd=%d, flags=%x) = %d\n",
	hw_event->type,hw_event->config, hw_event->read_format, hw_event->size, hw_event->disabled,
	 pid,cpu,group_fd,flags,ret);

  return ret;
}

// creates and allocates a hardware counter group
struct counter_group *hardware_counter_group(char *name,unsigned int mask){
  int count,i;
  struct counter_group *cgroup = NULL;
  struct counter_def *hw_counter_table = NULL;
  int num_hw_counters = 0;
  
  if (!cpu_info) inventory_cpu();
  
  // set up counter groups
  switch(cpu_info->vendor){
  case VENDOR_AMD:
    if (cpu_info->family == 0x17 || cpu_info->family == 0x19){
      // zen core
      hw_counter_table = amd_zen_counters;
      num_hw_counters = sizeof(amd_zen_counters)/sizeof(amd_zen_counters[0]);
    } else {
      return NULL;
    }
    break;
  case VENDOR_INTEL:
    if (cpu_info->family == 0x6 &&
	((cpu_info->model == 0xba)||(cpu_info->model == 0xb7)|| // raptor lake
	 (cpu_info->model == 0x9a)||(cpu_info->model == 0x97)|| // alder lake
	 (cpu_info->model == 0xa7))){ // rocket lake
      hw_counter_table = intel_core_counters;
      num_hw_counters = sizeof(intel_core_counters)/sizeof(intel_core_counters[0]);
    } else {
      return NULL;
    }
    break;
  }

  // find counters that match
  if ((counter_mask & mask) && num_hw_counters){
    count = 0;
    for (i=0;i<num_hw_counters;i++){
      if (hw_counter_table[i].use & mask) count++;
    }
    cgroup = calloc(1,sizeof(struct counter_group));
    cgroup->label = strdup(name);
    cgroup->type_id = PERF_TYPE_RAW;
    cgroup->mask = mask;
    cgroup->ncounters = count;
    cgroup->counter = calloc(count,sizeof(struct counter_def *));
    count = 0;
    for (i=0;i<num_hw_counters;i++){
      if (hw_counter_table[i].use & mask){
	cgroup->counter[count] = &hw_counter_table[i];
	count++;
      }
    }
  }
  
  return cgroup;
}

// creates and allocates a group for software performance counters
struct counter_group *software_counter_group(char *name){
  int i;
  
  struct software_counters {
    char *label;
    unsigned int config;
  } sw_counters[] = {
    { "cpu-clock", PERF_COUNT_SW_CPU_CLOCK },
    { "task-clock", PERF_COUNT_SW_TASK_CLOCK },    
    { "page faults", PERF_COUNT_SW_PAGE_FAULTS },
    { "context switches", PERF_COUNT_SW_CONTEXT_SWITCHES },
    { "cpu migrations", PERF_COUNT_SW_CPU_MIGRATIONS },
    { "major page faults", PERF_COUNT_SW_PAGE_FAULTS_MAJ },
    { "minor page faults", PERF_COUNT_SW_PAGE_FAULTS_MIN },
    { "alignment faults", PERF_COUNT_SW_ALIGNMENT_FAULTS },
    { "emulation faults", PERF_COUNT_SW_EMULATION_FAULTS },
  };
  
  struct counter_group *cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  cgroup->type_id = PERF_TYPE_SOFTWARE;
  cgroup->ncounters = sizeof(sw_counters)/sizeof(sw_counters[0]);
  cgroup->cinfo = calloc(cgroup->ncounters,sizeof(struct counter_info));
  for (i=0;i<cgroup->ncounters;i++){
    cgroup->cinfo[i].label = strdup(sw_counters[i].label);
    cgroup->cinfo[i].config = sw_counters[i].config;
  }
  return cgroup;
}

// creates and allocates a group for generic hardware performance counters
struct counter_group *generic_hardware_counter_group(char *name){
  int i;
  
  struct hardware_counters {
    char *label;
    unsigned int config;
  } hw_counters[] = {
    { "cpu-cycles", PERF_COUNT_HW_CPU_CYCLES },
    { "instructions", PERF_COUNT_HW_INSTRUCTIONS },
    { "branches", PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
    { "branch-misses", PERF_COUNT_HW_BRANCH_MISSES },
  };
  
  struct counter_group *cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  cgroup->type_id = PERF_TYPE_HARDWARE;
  cgroup->ncounters = sizeof(hw_counters)/sizeof(hw_counters[0]);
  cgroup->cinfo = calloc(cgroup->ncounters,sizeof(struct counter_info));
  for (i=0;i<cgroup->ncounters;i++){
    cgroup->cinfo[i].label = strdup(hw_counters[i].label);
    cgroup->cinfo[i].config = hw_counters[i].config;
  }
  return cgroup;
}


void setup_counter_groups(struct counter_group **counter_group_list){

  int i,count;
  struct counter_group *cgroup;

  // note: These get pushed onto a linked list, so last listed is first printed
  if (counter_mask & COUNTER_TOPDOWN){
    if (cgroup = hardware_counter_group("topdown",COUNTER_TOPDOWN)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }
  }  

  if (counter_mask & COUNTER_IPC){
    if (cgroup = generic_hardware_counter_group("generic hardware")){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;		
    }
  }

}

void setup_counters(struct counter_group *counter_group_list){
  unsigned int mask = 0;
  int i,j,k;
  int count;
  int nerror = 0;
  int status;
  int groupid;
  struct counter_group *cgroup;
  struct perf_event_attr pe;

  struct cpu_core_info *coreinfo;
  struct counter_def *counter_def;
  int ncounters;

  // create system-wide counters
  for (cgroup = counter_group_list;cgroup;cgroup = cgroup->next){
    debug("Setting up %s counters\n",cgroup->label);
    groupid = -1;
    for (i=0;i<cgroup->ncounters;i++){
      memset(&pe,0,sizeof(pe));
      pe.type = cgroup->type_id;
      pe.config = cgroup->cinfo[i].config;
      pe.sample_type = PERF_SAMPLE_IDENTIFIER; // is this needed?
      pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
      pe.size = sizeof(struct perf_event_attr);
      pe.exclude_guest = 1; // is this needed
      pe.inherit = 1;
      pe.disabled = 1;

      status = perf_event_open(&pe,0,-1,groupid,PERF_FLAG_FD_CLOEXEC);
      if (status == -1){
	error("unable to create %s performance counter, name=%s, errno=%d - %s\n",
	      cgroup->label,cgroup->cinfo[i].label,errno,strerror(errno));
	cgroup->cinfo[i].fd = -1;
	nerror++;
      } else {
	cgroup->cinfo[i].fd = status;
	ioctl(cgroup->cinfo[i].fd,PERF_EVENT_IOC_ENABLE,0);
	debug("   create %s performance counter, name=%s\n",cgroup->label,cgroup->cinfo[i].label);
	if (groupid == -1) groupid = cgroup->cinfo[i].fd;
      }
    }
  }
  if (nerror) fatal("unable to open performance counters\n");
}

void start_counters(struct counter_group *counter_group_list){
  int i,j;
  int status;
  struct counter_group *cgroup;

  for (cgroup = counter_group_list;cgroup;cgroup=cgroup->next){
    debug("Starting %s counters\n",cgroup->label);  
    for (i=0;i<cgroup->ncounters;i++){
      if (cgroup->cinfo[i].fd != -1){
	status = ioctl(cgroup->cinfo[i].fd,PERF_EVENT_IOC_ENABLE,0);
	if (status != 0){
	  error("unable to start %s counter %s, errno=%d,%s\n",
		cgroup->label,cgroup->cinfo[i].label,errno,strerror(errno));
	} else {
	  debug("   started %s counter, name=%s\n",cgroup->label,cgroup->cinfo[i].label);
	}
      }
    }
  }
}

void stop_counters(struct counter_group *counter_group_list){
  int i,j;
  int status;
  struct counter_group *cgroup;
  
  struct read_format { uint64_t value, time_enabled, time_running,id; } rf;

  for (cgroup = counter_group_list;cgroup;cgroup = cgroup->next){
    debug("Stopping %s counters\n",cgroup->label);
    for (i=0;i<cgroup->ncounters;i++){
      if (cgroup->cinfo[i].fd != -1){
	status = read(cgroup->cinfo[i].fd,&rf,sizeof(rf));
	if (status == -1){
	  error("unable to read %s counter %s, errno=%d - %s\n",
		cgroup->label,cgroup->cinfo[i].label,errno,strerror(errno));
	} else {
	  cgroup->cinfo[i].value = rf.value;
	  cgroup->cinfo[i].time_running = rf.time_running;
	  cgroup->cinfo[i].time_enabled = rf.time_enabled;
	  // TODO: add scaling for performance counters...
	  
	  status = ioctl(cgroup->cinfo[i].fd,PERF_EVENT_IOC_DISABLE,0);
	  
	  if (status != 0){
	    error("unable to stop %s counter %s, errno=%d,%s\n",
		  cgroup->label,cgroup->cinfo[i].label,errno,strerror(errno));
	  } else {
	    debug("   stopped %s counter, name=%s, value=%lu enabled=%lu running=%lu\n",
		  cgroup->label,cgroup->cinfo[i].label,cgroup->cinfo[i].value,
		  rf.time_enabled,rf.time_running);
	  }
	}
      }
    }
  }
  //  dump_cpu_info();
}

void print_usage(struct rusage *rusage){
  double elapsed;
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
  fprintf(outfile,"elapsed              %4.3f\n",elapsed);
  fprintf(outfile,"on_cpu               %4.3f          # %4.2f / %d cores\n",
	  (rusage->ru_utime.tv_sec+rusage->ru_utime.tv_usec/1000000.0+
	   rusage->ru_stime.tv_sec+rusage->ru_stime.tv_usec/1000000.0)/
	  elapsed / num_procs,
	  (rusage->ru_utime.tv_sec+rusage->ru_utime.tv_usec/1000000.0+
	   rusage->ru_stime.tv_sec+rusage->ru_stime.tv_usec/1000000.0)/
	  elapsed,
	  cpu_info->num_cores_available
	  );
  fprintf(outfile,"utime                %4.3f\n",
	  (double) rusage->ru_utime.tv_sec +
	  rusage->ru_utime.tv_usec / 1000000.0);
  fprintf(outfile,"stime                %4.3f\n",
	  (double) rusage->ru_stime.tv_sec +
	  rusage->ru_stime.tv_usec / 1000000.0);
  fprintf(outfile,"nvcsw                %-15lu# %4.2f%%\n",
	  rusage->ru_nvcsw,(double) rusage->ru_nvcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"nivcsw               %-15lu# %4.2f%%\n",
	  rusage->ru_nivcsw,(double) rusage->ru_nivcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"inblock              %lu\n",rusage->ru_inblock);
  fprintf(outfile,"onblock              %lu\n",rusage->ru_oublock);  
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

void print_topdown(){
  switch (cpu_info->vendor){
  case VENDOR_INTEL:
    if (cpu_info->family == 6 &&
	((cpu_info->model == 0xba)||(cpu_info->model == 0xb7)|| // raptor lake
	 (cpu_info->model == 0x9a)||(cpu_info->model == 0x97)|| // alder lake
	 (cpu_info->model == 0xa7))){ // rocket lake
      unsigned long int slots = sum_counters("slots");
      unsigned long int retiring = sum_counters("topdown-retiring");
      unsigned long int fe_bound = sum_counters("topdown-fe-bound");
      unsigned long int be_bound = sum_counters("topdown-be-bound");
      unsigned long int bad_spec = sum_counters("topdown-bad-spec");
      fprintf(outfile,"retire         %4.3f\n",
	      (double) retiring / slots);
      fprintf(outfile,"speculation    %4.3f\n",
	      (double) bad_spec / slots);
      fprintf(outfile,"frontend       %4.3f\n",
	      (double) fe_bound / slots);
      fprintf(outfile,"backend        %4.3f\n",
	      (double) be_bound / slots);
    }
    break;
  case VENDOR_AMD:
    if (cpu_info->family == 0x17 || cpu_info->family == 0x19){
      // Zen
      unsigned long int slots = sum_counters("cpu-cycles") * 6;
      unsigned long int retiring = sum_counters("ex_ret_ops");
      unsigned long int fe_bound = sum_counters("de_no_dispatch_per_slot.no_ops_from_frontend");
      unsigned long int be_bound = sum_counters("de_no_dispatch_per_slot.backend_stalls");
      unsigned long int bad_spec = sum_counters("de_src_op_disp.all") - retiring;
      debug("-> slots     %llu\n", slots);
      debug("-> retire    %llu\n", retiring);
      debug("-> frontend  %llu\n", fe_bound);
      debug("-> backend   %llu\n", be_bound);
      debug("-> speculate %llu\n", bad_spec);
      //      debug("-> smt       %llu\n", smt_cont);
      fprintf(outfile,"retire         %4.3f\n",
	      (double) retiring / slots);
      fprintf(outfile,"speculation    %4.3f\n",
	      (double) bad_spec / slots);
      fprintf(outfile,"frontend       %4.3f\n",
	      (double) fe_bound / slots);
      fprintf(outfile,"backend        %4.3f\n",
	      (double) be_bound / slots);
    }
    break;
  }
}

struct counter_info *find_ci_label(struct counter_group *cgroup,char *label){
  int i;
  for (i=0;i<cgroup->ncounters;i++){
    if (!strcmp(cgroup->cinfo[i].label,label)){
      return &cgroup->cinfo[i];
    }
  }
  return NULL;
}

void print_ipc(struct counter_group *cgroup){
  int i;
  unsigned long cpu_cycles=0,scaled_cpu_cycles=0;
  unsigned long instructions=0;
  unsigned long branches=0;
  unsigned long branch_misses=0;
  double elapsed;
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;  

  for (i=0;i<cgroup->ncounters;i++){
    if (!strcmp(cgroup->cinfo[i].label,"cpu-cycles")){
      cpu_cycles = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
      scaled_cpu_cycles = (double) cpu_cycles * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"instructions")){
      instructions = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"branches")){
      branches = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"branch-misses")){
      branch_misses = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    }
  }

  if (cpu_cycles){
    printf("cpu-cycles           %-14lu # %4.2fGHz\n",cpu_cycles,(double) scaled_cpu_cycles / elapsed / 1000000000.0 / cpu_info->num_cores_available / (aflag?cpu_info->num_cores_available:1));
    printf("instructions         %-14lu # %4.2f IPC\n",instructions,(double) instructions / cpu_cycles);
    if (instructions){
      printf("branches             %-14lu # %4.2f%%\n",branches,(double) branches / instructions * 100.0);
      printf("branch-misses        %-14lu # %4.2f%%\n",branch_misses,(double) branch_misses / branches * 100.0);
    }
  }
}



void print_software(struct counter_group *cgroup){
  int i;
  struct counter_info *task_info = find_ci_label(cgroup,"task-clock");
  double task_time = (double) task_info->value / 1000000000.0;
  struct counter_info *cinfo;
  for (i=0;i<cgroup->ncounters;i++){
    fprintf(outfile,"%-20s %-12lu",cgroup->cinfo[i].label,cgroup->cinfo[i].value);
    if (!strcmp(cgroup->cinfo[i].label,"task-clock") ||
	!strcmp(cgroup->cinfo[i].label,"cpu-clock")){
      fprintf(outfile,"   # %4.3f seconds",
	     (double) cgroup->cinfo[i].value / 1000000000.0);
    } else {
      fprintf(outfile,"   # %4.3f/sec",cgroup->cinfo[i].value / task_time);
	     
    }
    fprintf(outfile,"\n");
  }
}

void print_metrics(struct counter_group *counter_group_list){
  struct counter_group *cgroup;
  for (cgroup = counter_group_list;cgroup;cgroup = cgroup->next){
    if (!strcmp(cgroup->label,"software")){
      print_software(cgroup);
    } else if (!strcmp(cgroup->label,"generic hardware")){
      print_ipc(cgroup);      
    }
  }
  
  if (counter_mask & COUNTER_TOPDOWN){
    print_topdown();
  }
}

int main(int argc,char *const argv[],char *const envp[]){
  int i;
  int status;
  struct rusage rusage;
  struct counter_group *cgroup;
  outfile = stdout;
  num_procs = get_nprocs();
  if (parse_options(argc,argv)){
      fatal("usage: %s -[Aaivx][-o <file>] <cmd><args>...\n"
	    "\t-A         - create per-cpu counters\n"
	    "\t-a         - create overall counters\n"
	    "\t-I         - turn on IPC metrics\n"
	    "\t-i         - turn off IPC metrics\n"
	    "\t-o <file>  - send output to <file>\n"
	    "\t-S         - turn on software metrics\n"
	    "\t-s         - turn off software metrics\n"
	    "\t-T         - turn on topdown metrics\n"
	    "\t-t         - turn off topdown metrics\n"
	    "\t-v         - print verbose information\n"
	    "\t-X	  - turn on system rusage info\n"
	    "\t-x         - turn off system rusage info\n"

	    ,argv[0]);
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  // set up either system-wide or core-specific counters
  if (aflag){
    for (i=0;i<cpu_info->num_cores;i++){
      if (cpu_info->coreinfo[i].is_available &&
	  ((cpu_info->coreinfo[i].vendor == CORE_AMD_ZEN)||(cpu_info->coreinfo[i].vendor == CORE_INTEL_CORE))){
	setup_counter_groups(&cpu_info->coreinfo[i].core_specific_counters);
      }
    }
  } else {
    setup_counter_groups(&cpu_info->systemwide_counters);
  }
  // software counters are only system-wide
  if (counter_mask & COUNTER_SOFTWARE){
    if (cgroup = software_counter_group("software")){
      cgroup->next = cpu_info->systemwide_counters;
      cpu_info->systemwide_counters = cgroup;
    }
  }

  // set up core-specific and system-wide counters
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters)
      setup_counters(cpu_info->coreinfo[i].core_specific_counters);
  }
  setup_counters(cpu_info->systemwide_counters);

  // start core-specific and system-wide counters
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters)
      start_counters(cpu_info->coreinfo[i].core_specific_counters);
  }
  start_counters(cpu_info->systemwide_counters);  

  clock_gettime(CLOCK_REALTIME,&start_time);
  if (launch_child(command_line_argc,command_line_argv,envp)){
    fatal("unable to launch %s\n",command_line_argv[0]);
  }
  wait4(child_pid,&status,0,&rusage);

  // start core-specific and system-wide counters
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters)
      stop_counters(cpu_info->coreinfo[i].core_specific_counters);
  }
  stop_counters(cpu_info->systemwide_counters);  

  clock_gettime(CLOCK_REALTIME,&finish_time);  

  if (xflag){
    print_usage(&rusage);
  }

  //  dump_counters();

  print_metrics(cpu_info->systemwide_counters);
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters){
      fprintf(outfile,"##### core %2d #######################\n",i);
      print_metrics(cpu_info->coreinfo[i].core_specific_counters);
    }
  }

  if (oflag) fclose(outfile);
  return 0;
}
