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
  { "ex_ret_ops",                        0xc1, 0,    0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.no_ops_from_frontend",
                                         0x1a0,1,    0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.backend_stalls",
                                         0x1a0,0x1e, 0,    0,  0,    USE_L1  },
  { "de_src_op_disp.all",                0xaa, 0x7,  0,    0,  0,    USE_L1  },
  { "de_no_dispatch_per_slot.smt_contention",
                                         0x1a0,0x60, 0,    0,  0,    USE_L1  },
  { "cpu-cycles",                        0x76, 0,    0,    0,  0,    USE_IPC|USE_L1 },
  { "instructions",                      0xc0, 0,    0,    0,  0,    USE_IPC },
  
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
  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  
  debug2("perf_event_open(event=<type=%d, config=%lx, format=%x, size=%d, disabled=%d>, pid=%d, cpu=%d, group_fd=%d, flags=%x) = %d\n",
	hw_event->type,hw_event->config, hw_event->read_format, hw_event->size, hw_event->disabled,
	 pid,cpu,group_fd,flags,ret);

  return ret;
}

void setup_counters(void){
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
	debug2("counter: fd=%d, name=%s, value=%lu\n",cinfo->fd,cinfo->cdef->name,cinfo->value);
	    
      }
    }
  }
  dump_cpu_info();
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
      unsigned long int smt_cont = sum_counters("de_no_dispatch_per_slot.smt_contention");
      debug("-> slots     %llu\n", slots);
      debug("-> retire    %llu\n", retiring);
      debug("-> frontend  %llu\n", fe_bound);
      debug("-> backend   %llu\n", be_bound);
      debug("-> speculate %llu\n", bad_spec);
      debug("-> smt       %llu\n", smt_cont);
      fprintf(outfile,"retire         %4.3f\n",
	      (double) retiring / slots);
      fprintf(outfile,"speculation    %4.3f\n",
	      (double) bad_spec / slots);
      fprintf(outfile,"frontend       %4.3f\n",
	      (double) fe_bound / slots);
      fprintf(outfile,"backend        %4.3f\n",
	      (double) be_bound / slots);
      fprintf(outfile,"smt contend    %4.3f\n",
	      (double) smt_cont / slots);
    }
    break;
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

  setup_counters();

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
  }
  
  if (oflag) fclose(outfile);
  return 0;
}
