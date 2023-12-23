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
#define COUNTER_TOPDOWN_E   0x4
#define COUNTER_SOFTWARE    0x10
unsigned int counter_mask = COUNTER_IPC;

struct timespec start_time,finish_time;

// Counter definitions for RAW performance counters
struct raw_event intel_raw_events[] = {
  { "instructions","event=0xc0",COUNTER_IPC,0 },
  { "cpu-cycles","event=0x3c",COUNTER_IPC,0 },
  { "atom.topdown-bad-spec","event=0x73,umask=0x0",COUNTER_TOPDOWN_E,0},
  { "atom.topdown-be-bound","event=0x74,umask=0x0",COUNTER_TOPDOWN_E,0 },
  { "atom.topdown-fe-bound","event=0x71,umask=0x0",COUNTER_TOPDOWN_E,0 },
  { "atom.topdown-retiring","event=0xc2,umask=0x0",COUNTER_TOPDOWN_E,0 },
  { "slots","event=0x00,umask=0x4",COUNTER_TOPDOWN,0 },
  { "core.topdown-bad-spec","event=0x00,umask=0x81",COUNTER_TOPDOWN,0 },
  { "core.topdown-be-bound","event=0x00,umask=0x83",COUNTER_TOPDOWN,0 },
  { "core.topdown-fe-bound","event=0x00,umask=0x82",COUNTER_TOPDOWN,0 },
  { "core.topdown-retiring","event=0x00,umask=0x80",COUNTER_TOPDOWN,0 },        
};

struct raw_event amd_raw_events[] = {
  { "instructions","event=0xc0",COUNTER_IPC,0 },
  { "cpu-cycles","event=0x76",COUNTER_IPC|COUNTER_TOPDOWN,0 },
  { "ex_ret_ops","event=0xc1",COUNTER_TOPDOWN,0 },
  { "de_no_dispatch_per_slot.no_ops_from_frontend","event=0x1a0,umask=0x1",COUNTER_TOPDOWN,0 },
  { "de_no_dispatch_per_slot.backend_stalls","event=0x1a0,umask=0x1e",COUNTER_TOPDOWN,0 },
  { "de_src_op_disp.all","event=0xaa,umask=0x7",COUNTER_TOPDOWN,0 },
  { "de_no_dispatch_per_slot.smt_contention","event=0x1a0,umask=0x60",0 /* COUNTER_TOPDOWN*/,0 }, // six counters results in a read of 0? So comment this out of topdown metrics
};

unsigned long parse_intel_event(char *description){
  char *desc = strdup(description);
  char *name,*val;
  unsigned long value;
  union intel_raw_cpu_format result;
  result.config = 0;
  
  for (name = strtok(desc,",\n");name;name = strtok(NULL,",\n")){
    if (val = strchr(name,'=')){ // expected format "desc=value"
      *val = 0; // null terminator for name
      val++;
      value = strtol(val,NULL,16);
      
      if (!strcmp(name,"event")){
	result.event = value;
      } else if (!strcmp(name,"umask")){
	result.umask = value;
      } else {
	fatal("unimplemented %s in parse_intel_event\n",name);
      }
    }
  }
  debug2("parse_event(\"%s\") = %lx\n",description,result.config);
  
  return result.config;
};

unsigned long parse_amd_event(char *description){
  char *desc = strdup(description);
  char *name,*val;
  unsigned long value;
  union amd_raw_cpu_format result;
  result.config = 0;
  
  for (name = strtok(desc,",\n");name;name = strtok(NULL,",\n")){
    if (val = strchr(name,'=')){ // expected format "desc=value"
      *val = 0; // null terminator for name
      val++;
      value = strtol(val,NULL,16);
      
      if (!strcmp(name,"event")){
	result.event = value;
	result.event2 = value>>8;
      } else if (!strcmp(name,"umask")){
	result.umask = value;
      } else {
	fatal("unimplemented %s in parse_intel_event\n",name);
      }
    }
  }
  debug2("parse_event(\"%s\") = %lx\n",description,result.config);
  
  return result.config;
};

int setup_events(void){
  int i;
  struct raw_event *events;
  unsigned int num_events;
  switch(cpu_info->vendor){
  case VENDOR_AMD:
    events = amd_raw_events;
    num_events = sizeof(amd_raw_events)/sizeof(amd_raw_events[0]);
    for (i=0;i<num_events;i++){
      events[i].raw.config = parse_amd_event(events[i].description);
    }    
    break;
  case VENDOR_INTEL:
    events = intel_raw_events;
    num_events = sizeof(intel_raw_events)/sizeof(intel_raw_events[0]);
    for (i=0;i<num_events;i++){
      events[i].raw.config = parse_intel_event(events[i].description);
    }        
    break;
  default:
    warning("Unknown CPU, no events parsed\n");
    break;
  }
}


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

#if 0  
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
  }
  
  return cgroup;
}
#endif

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

// creates and allocates a group for raw hardware performance counters
struct counter_group *raw_counter_group(char *name,unsigned int mask){
  int i;
  
  struct raw_event *events;
  unsigned int num_events;
  switch(cpu_info->vendor){
  case VENDOR_AMD:
    events = amd_raw_events;
    num_events = sizeof(amd_raw_events)/sizeof(amd_raw_events[0]);
    break;
  case VENDOR_INTEL:
    events = intel_raw_events;
    num_events = sizeof(intel_raw_events)/sizeof(intel_raw_events[0]);
    break;
  default:
    return NULL;
  }

  int num_counters = 0;
  for (i=0;i<num_events;i++){
    if (events[i].use & mask) num_counters++;
  }
  
  struct counter_group *cgroup = calloc(1,sizeof(struct counter_group));  
  cgroup->label = strdup(name);
  cgroup->type_id = PERF_TYPE_RAW;
  cgroup->ncounters = num_counters;
  cgroup->cinfo = calloc(cgroup->ncounters,sizeof(struct counter_info));
  cgroup->mask = mask;
  int count = 0;
  for (i=0;i<num_events;i++){
    if (events[i].use & mask){
      cgroup->cinfo[count].label = events[i].name;
      cgroup->cinfo[count].config = events[i].raw.config;
      count++;
    }
  }
  
  return cgroup;
}

void setup_counter_groups(struct counter_group **counter_group_list){

  int i,count;
  struct counter_group *cgroup;

  // note: These get pushed onto a linked list, so last listed is first printed
  if (counter_mask & COUNTER_TOPDOWN){
    if (cgroup = raw_counter_group("topdown",COUNTER_TOPDOWN)){
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
    printf("cpu-cycles           %-14lu # %4.2f GHz\n",cpu_cycles,(double) scaled_cpu_cycles / elapsed / 1000000000.0 / cpu_info->num_cores_available / (aflag?cpu_info->num_cores_available:1));
    printf("instructions         %-14lu # %4.2f IPC\n",instructions,(double) instructions / cpu_cycles);
    if (instructions){
      printf("branches             %-14lu # %4.2f%%\n",branches,(double) branches / instructions * 100.0);
      printf("branch-misses        %-14lu # %4.2f%%\n",branch_misses,(double) branch_misses / branches * 100.0);
    }
  }
}

void print_topdown(struct counter_group *cgroup){
  unsigned long slots=0;
  unsigned long retiring=0;
  unsigned long frontend=0;
  unsigned long backend=0;
  unsigned long speculation=0;
  struct counter_info *cinfo;

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    if (cinfo = find_ci_label(cgroup,"slots")) slots = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-retiring")) retiring = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-fe-bound")) frontend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-be-bound")) backend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-bad-spec")) speculation = cinfo->value;
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"cpu-cycles")) slots = cinfo->value * 6;
    if (cinfo = find_ci_label(cgroup,"ex_ret_ops")) retiring = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.no_ops_from_frontend")) frontend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.backend_stalls")) backend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_src_op_disp.all")) speculation = cinfo->value - retiring;
    break;
  default:
    return;
  }
  if (slots){
    fprintf(outfile,"slots                %-14lu #\n",slots);
    fprintf(outfile,"retiring             %-14lu # %4.1f%%\n",retiring,(double) retiring/slots*100);
    fprintf(outfile,"frontend             %-14lu # %4.1f%%\n",frontend,(double) frontend/slots*100);
    fprintf(outfile,"backend              %-14lu # %4.1f%%\n",backend,(double) backend/slots*100);
    fprintf(outfile,"speculation          %-14lu # %4.1f%%\n",speculation,(double) speculation/slots*100);
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
    } else if (counter_mask & COUNTER_TOPDOWN){
      print_topdown(cgroup);
    }
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

  // parse the event tables
  setup_events();

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
