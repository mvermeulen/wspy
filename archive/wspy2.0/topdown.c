/*
 * topdown.c - topdown performance counter program
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>
#include <time.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/user.h>
#include <sys/stat.h>
#include <sys/ptrace.h>
#include <linux/perf_event.h>
#include <errno.h>
#include "error.h"
#include "cpu_info.h"

volatile int is_still_running;
int num_procs;
int aflag = 0;
int oflag = 0;
int xflag = 1;
int vflag = 0;
int nmi_running = 0;
int interval = 0;
enum output_format { PRINT_NORMAL, PRINT_CSV, PRINT_CSV_HEADER };
int csvflag = 0;
int treeflag = 0;
int tree_cmdline = 0;
FILE *treefile = NULL;
int dummy = 0;

#define COUNTER_IPC         0x1
#define COUNTER_TOPDOWN     0x2
#define COUNTER_TOPDOWN2    0x4
#define COUNTER_BRANCH      0x8
#define COUNTER_DCACHE      0x10
#define COUNTER_ICACHE      0x20
#define COUNTER_L2CACHE     0x40
#define COUNTER_L3CACHE     0x80
#define COUNTER_MEMORY      0x100
#define COUNTER_TLB         0x200
#define COUNTER_OPCACHE     0x400
#define COUNTER_SOFTWARE    0x800
#define COUNTER_FLOAT       0x1000


unsigned int counter_mask = COUNTER_IPC;

struct timespec start_time,finish_time;

// Counter definitions for RAW performance counters
static int intel_group_id = -1;
struct raw_event intel_raw_events[] = {
  { "instructions","event=0xc0",PERF_TYPE_RAW,COUNTER_IPC|COUNTER_BRANCH|COUNTER_L2CACHE,0 },
  { "cpu-cycles","event=0x3c",PERF_TYPE_RAW,COUNTER_IPC,0 },
  { "slots","event=0x00,umask=0x4",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "core.topdown-retiring","event=0x00,umask=0x80",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "core.topdown-bad-spec","event=0x00,umask=0x81",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "core.topdown-fe-bound","event=0x00,umask=0x82",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "core.topdown-be-bound","event=0x00,umask=0x83",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "core.topdown-heavy-ops","event=0x00,umask=0x84",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "core.topdown-br-mispredict","event=0x00,umask=0x85",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "core.topdown-fetch-lat","event=0x00,umask=0x86",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "core.topdown-mem-bound","event=0x00,umask=0x87",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "br_inst_retired.all_branches","event=0xc4,period=0x61a89",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "br_misp_retired.all_branches","event=0xc5,period=0x61a89",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "br_inst_retired.cond","event=0xc4,period=0x61a89",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "br_inst_retired.indirect","event=0xc4,period=0x186a3,umask=0x80",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "l2_request.all","event=0x24,period=0x30d43,umask=0xff",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "l2_request.miss","event=0x24,period=0x30d43,umask=0x3f",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },  
};

struct raw_event amd_raw_events[] = {
  { "instructions","event=0xc0",PERF_TYPE_RAW,COUNTER_IPC|COUNTER_BRANCH|COUNTER_OPCACHE|COUNTER_TLB|COUNTER_L2CACHE|COUNTER_L3CACHE|COUNTER_FLOAT,0 },
  { "cpu-cycles","event=0x76",PERF_TYPE_RAW,COUNTER_IPC|COUNTER_BRANCH|COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "ex_ret_ops","event=0xc1",PERF_TYPE_RAW,COUNTER_IPC|COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "de_no_dispatch_per_slot.no_ops_from_frontend","event=0x1a0,umask=0x1",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "de_no_dispatch_per_slot.backend_stalls","event=0x1a0,umask=0x1e",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "de_src_op_disp.all","event=0xaa,umask=0x7",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "de_no_dispatch_per_slot.smt_contention","event=0x1a0,umask=0x60",PERF_TYPE_RAW,COUNTER_TOPDOWN|COUNTER_TOPDOWN2,0 },
  { "ex_no_retire.load_not_complete","event=0xd6,umask=0xa2",PERF_TYPE_RAW,COUNTER_TOPDOWN2, 0 },
  { "ex_no_retire.not_complete","event=0xd6,umask=0x2",PERF_TYPE_RAW,COUNTER_TOPDOWN2, 0 },
  { "ex_ret_brn_misp","event=0xc3",PERF_TYPE_RAW,COUNTER_TOPDOWN2, 0 },
  { "resyncs_or_nc_redirects","event=0x96",PERF_TYPE_RAW,COUNTER_TOPDOWN2, 0 },
  { "de_no_dispatch_per_slot.no_ops_from_frontend.cmask_0x6","event=0x1a0,umask=0x1,cmask=0x6",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "ex_ret_ucode_ops","event=0x1c1",PERF_TYPE_RAW,COUNTER_TOPDOWN2,0 },
  { "branch-instructions","event=0xc2",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "branch-misses","event=0xc3",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "conditional-branches","event=0xd1",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "indirect-branches","event=0xcc",PERF_TYPE_RAW,COUNTER_BRANCH,0 },
  { "op_cache_hit_miss.all_op_cache_accesses","event=0x28f,umask=0x7",PERF_TYPE_RAW,COUNTER_OPCACHE,0 },
  { "op_cache_hit_miss.op_cache_miss","event=0x28f,umask=0x4",PERF_TYPE_RAW,COUNTER_OPCACHE,0 },
  { "l2_request_g1.all_no_prefetch","event=0x60,umask=0xf9",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "l2_pf_hit_l2","event=0x70,umask=0x1f",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "l2_pf_miss_l2_hit_l3", "event=0x71,umask=0x1f",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "l2_pf_miss_l2_l3","event=0x72,umask=0x1f",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "l2_cache_req_stat.ic_dc_miss_in_l2","event=0x64,umask=0x9",PERF_TYPE_RAW,COUNTER_L2CACHE,0 },
  { "ls_data_cache_refills.local_all","event=0x43,umask=0xf",PERF_TYPE_RAW,COUNTER_MEMORY,0 },
  { "ls_data_cache_refills.remote_all","event=0x43,umask=0x50",PERF_TYPE_RAW,COUNTER_MEMORY,0 },
  { "ls_hwpref_data_cache_refills.local_all","event=0x50,umask=0xf",PERF_TYPE_RAW,COUNTER_MEMORY,0 },
  { "ls_hwpref_data_cache_refills.remote_all","event=0x50,umask=0x50",PERF_TYPE_RAW,COUNTER_MEMORY,0 },
  { "fp_ret_fops_AVX512","event=0x20,umask=0x8",PERF_TYPE_RAW,COUNTER_FLOAT,0},
  { "fp_ret_fops_AVX256","event=0x10,umask=0x8",PERF_TYPE_RAW,COUNTER_FLOAT,0},
  { "fp_ret_fops_AVX128","event=0x8,umask=0x8",PERF_TYPE_RAW,COUNTER_FLOAT,0},
  { "fp_ret_fops_MMX","event=0x2,umask=0x8",PERF_TYPE_RAW,COUNTER_FLOAT,0},
  { "fp_ret_fops_scalar","event=0x5,umask=0x8",PERF_TYPE_RAW,COUNTER_FLOAT,0},
  // l3 events, need to have /sys/devices/amd_l3/type available...
  { "l3_lookup_state.all_coherent_accesses_to_l3","event=0x4,umask=0xff,requires=/sys/devices/amd_l3/type",PERF_TYPE_L3,COUNTER_L3CACHE, 0 },
  { "l3_lookup_state.l3_miss","event=0x4,umask=0x1,requires=/sys/devices/amd_l3/type",PERF_TYPE_L3,COUNTER_L3CACHE, 0 },  
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
      } else if (!strcmp(name,"period")){
	// appears in some Intel perf list output, but not sure of purpose, ignore...
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
  struct stat statbuf;
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
      } else if (!strcmp(name,"cmask")){
	result.cmask = value;
      } else if (!strcmp(name,"requires")){
	// the value encodes the name of the type field...
	debug("checking for %s\n",val);
	if (stat(val,&statbuf) ==-1){
	  fatal("%s not found, is the perf module for this device loaded in the kernel?\n");
	}
      } else {
	fatal("unimplemented %s in parse_amd_event\n",name);
      }
    }
  }
  debug2("parse_event(\"%s\") = %lx\n",description,result.config);
  
  return result.config;
};

int setup_raw_events(void){
  int i;
  struct raw_event *events;
  unsigned int num_events;
  switch(cpu_info->vendor){
  case VENDOR_AMD:
    events = amd_raw_events;
    num_events = sizeof(amd_raw_events)/sizeof(amd_raw_events[0]);
    for (i=0;i<num_events;i++){
      if (events[i].use & counter_mask)
	events[i].raw.config = parse_amd_event(events[i].description);
    }    
    break;
  case VENDOR_INTEL:
    events = intel_raw_events;
    num_events = sizeof(intel_raw_events)/sizeof(intel_raw_events[0]);
    for (i=0;i<num_events;i++){
      if (events[i].use & counter_mask)
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
  int value;
  unsigned int lev;
  static struct option long_options[] = {
    { "branch", no_argument, 0, 4 }, 
    { "no-branch", no_argument, 0, 5 },
    { "csv", no_argument, 0, 3 },
    { "cache2", no_argument, 0, 6 }, 
    { "no-cache2", no_argument, 0, 7 },
    { "cache3", no_argument, 0, 8 }, 
    { "no-cache3", no_argument, 0, 9 },
    { "dcache", no_argument, 0, 10 },
    { "no-cache", no_argument, 0, 11 },
    { "float",no_argument,0,33 },
    { "icache", no_argument, 0, 12 },
    { "no-icache", no_argument, 0, 13 },
    { "interval", required_argument, 0, 34 },
    { "ipc", no_argument, 0, 14 }, 
    { "no-ipc", no_argument, 0, 15 },
    { "memory", no_argument, 0, 16 }, 
    { "no-memory", no_argument, 0, 17 },
    { "opcache", no_argument, 0, 18 }, 
    { "no-opcache", no_argument, 0, 19 },
    { "per-core", no_argument, 0, 20 },
    { "rusage", no_argument, 0, 21 },
    { "no-rusage", no_argument, 0, 22 },
    { "software", no_argument, 0, 23 },
    { "no-software", no_argument, 0, 24 },
    { "tlb", no_argument, 0, 25 }, 
    { "no-tlb", no_argument, 0, 26 },
    { "topdown", no_argument, 0, 27 }, // (t)
    { "no-topdown", no_argument, 0, 28 },
    { "topdown2", no_argument, 0, 29 }, //
    { "no-topdown2", no_argument, 0, 30 },
    { "tree", required_argument, 0, 31 }, //
    { "tree-cmdline",no_argument,0,35 },
    { "verbose", no_argument, 0, 32 },
    { 0,0,0,0 },
  };
  while ((opt = getopt_long(argc,argv,"+abcio:rstv",long_options,NULL)) != -1){
    switch (opt){
    case 3: //--csv
      csvflag = 1;
      break;
    case 4: // --branch
    case 'b':
      counter_mask |= COUNTER_BRANCH;
      break;
    case 5: // --no-branch
      counter_mask &= (~COUNTER_BRANCH);
      break;
    case 6: // --cache2
    case 'c':
      counter_mask |= COUNTER_L2CACHE;
      break;
    case 7: // --no-cache2
      counter_mask &= (~COUNTER_L2CACHE);
      break;
    case 8: // --cache3
      counter_mask |= COUNTER_L3CACHE;
      break;
    case 9: // --no-cache3
      counter_mask &= (~COUNTER_L3CACHE);
      break;
    case 10: // --dcache
      counter_mask |= COUNTER_DCACHE;
      break;
    case 11: // --no-dcache
      counter_mask &= (~COUNTER_DCACHE);
      break;
    case 12: // --icache
      counter_mask |= COUNTER_ICACHE;
      break;
    case 13: // --no-icache
      counter_mask &= (~COUNTER_ICACHE);
      break;
    case 14: // --ipc
    case 'i':
      counter_mask |= COUNTER_IPC;
      break;
    case 15: // --no-ipc
      counter_mask &= (~COUNTER_IPC);
      break;
    case 16: // --memory
    case 'm':
      counter_mask |= COUNTER_MEMORY;
      break;
    case 17: // --no-memory
      counter_mask &= (~COUNTER_MEMORY);
      break;
    case 18: // --opcache
      counter_mask |= COUNTER_OPCACHE;
      break;
    case 19: // --no-opcache
      counter_mask &= (~COUNTER_OPCACHE);
      break;
    case 20: // --per-core
    case 'a':
      aflag = 1;
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
    case 21: // --rusage
    case 'r':
      xflag = 1;
      break;
    case 22: // --no-rusage
      xflag = 0;
      break;
    case 23: // --software
    case 'S':
      counter_mask |= COUNTER_SOFTWARE;
      break;
    case 24: // --no-software
      counter_mask &= (~COUNTER_SOFTWARE);
      break;
    case 25: // --tlb
      counter_mask |= COUNTER_TLB;
      break;
    case 26: // --no-tlb
      counter_mask &= (~COUNTER_TLB);
      break;
    case 27: // --topdown
    case 't':
      counter_mask |= COUNTER_TOPDOWN;
      break;
    case 28: // --no-topdown
      counter_mask &= (~COUNTER_TOPDOWN);
      break;
    case 29: // --topdown2
      counter_mask |= COUNTER_TOPDOWN2;
      break;
    case 30: // --no-topdown2
      counter_mask &= (~COUNTER_TOPDOWN2);
      break;
    case 31: // --tree
      if ((treefile = fopen(optarg,"w")) == NULL){
	warning("unable to open tree file: %s, ignored\n",optarg);
      } else {
	treeflag = 1;
      }
      break;
    case 32: // --verbose
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 33: // --float
      counter_mask |= COUNTER_FLOAT;
      break;
    case 34:
      if ((sscanf(optarg,"%d",&value) == 1) && value > 0){
	interval = value;
      } else {
	warning("invalid argument to --interval: %s, ignored\n",optarg);
      }
      break;
    case 35:
      tree_cmdline = 1;
      break;
    default:
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

int child_pipe[2];
int launch_child(int argc,char *const argv[],char *const envp[]){
  pid_t child;
  int len;
  char *p,*path;
  char pathbuf[1024];

  if (pipe(child_pipe) == -1) fatal("pipe creation failed\n");
  switch(child = fork()){
  case 0: // child
    if (treeflag){
      debug("ptrace(PTRACE_TRACEME,0)\n");
      ptrace(PTRACE_TRACEME,0,NULL,NULL);
    }
    close(child_pipe[1]); // close writing end
    read(child_pipe[0],pathbuf,sizeof(pathbuf)); // wait until the parent has written
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
    close(child_pipe[0]); // close reading end
    child_pid = child;
    is_still_running = 1;
    break;
  }
  return 0;
}

void ptrace_setup(pid_t child_pid){
  int status;
  
  debug("ptrace_setup\n");
  waitpid(child_pid,&status,0); // wait for an initial event from the PTRACE_TRACEME

  if (WIFEXITED(status)){
    fatal("child process exited unexpectedly\n");
  } else if (WIFSIGNALED(status)){
    fatal("child process signaled unexpectedly, signal = %d\n",
	  WSTOPSIG(status));
  }
  // set the ptrace event flags
  status = ptrace(PTRACE_SETOPTIONS,child_pid,0,
		  PTRACE_O_EXITKILL | // kill child if I exit
		  PTRACE_O_TRACECLONE|PTRACE_O_TRACEFORK|PTRACE_O_TRACEVFORK|
		  PTRACE_O_TRACEEXIT); // exit(2)
  ptrace(PTRACE_CONT,child_pid,NULL,NULL); // let the child continue
}

// loop through and handle ptrace events
void ptrace_loop(){
  pid_t pid,child;
  int status;
  int i;
  int last_syscall = 0;
  int syscall_entry = 0;
  unsigned long data;
  char buffer[1024];
  char stat_name[128];
  struct stat statbuf;
  FILE *stat_file;
  struct user_regs_struct regs;
  struct rusage rusage;
  double elapsed;

  while(1){
    pid = wait4(-1,&status,0,&rusage);
    debug2("event: pid=%d status=%x\n",pid,status);
    if (pid == -1){
      if (errno == ECHILD){
	break; // no more children to wait
      } else {
	error("wait returns -1 with errno %d - %s\n",errno,strerror(errno));
      }
    }
    if (WIFEXITED(status)){
      clock_gettime(CLOCK_REALTIME,&finish_time);
      elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
      fprintf(treefile,"%5.3f %d exited\n",elapsed,pid);
      fflush(treefile);
      debug2("   exited\n");
      if (pid == child_pid) break;
      continue;
    } else if (WIFSIGNALED(status)){
      clock_gettime(CLOCK_REALTIME,&finish_time);
      elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	start_time.tv_sec - start_time.tv_nsec / 1000000000.0;

      // dump contents of proc/<pid>/comm
      snprintf(stat_name,sizeof(stat_name),"/proc/%d/comm",pid);
      if ((stat_file = fopen(stat_name,"r")) != NULL){
	if (fgets(buffer,sizeof(buffer),stat_file) != NULL){
	  fprintf(treefile,"%5.3f %d comm ",elapsed,pid);
	  fputs(buffer,treefile);
	}
	fclose(stat_file);
      }
      // dump the full command line
      if (tree_cmdline){
	snprintf(stat_name,sizeof(stat_name),"/proc/%d/cmdline",pid);
	fprintf(treefile,"%5.3f %d cmdline",elapsed,pid);
	if ((stat_file = fopen(stat_name,"rb")) != NULL){
	  char *arg = 0;
	  size_t size = 0;
	  while (getdelim(&arg,&size,0,stat_file) != -1){
	    fprintf(treefile," %s",arg);
	  }
	  fclose(stat_file);
	}
	fprintf(treefile,"\n");
      }
      
      fprintf(treefile,"%5.3f %d signal %u\n",elapsed,pid,WTERMSIG(status));
      fflush(treefile);
      debug2("   signaled\n");
      continue;
    } else if (WIFSTOPPED(status)){
      if (WSTOPSIG(status) == SIGTRAP){
	// we have an event!
	switch(status>>16){
	case PTRACE_EVENT_CLONE:
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	  ptrace(PTRACE_GETEVENTMSG,pid,NULL,&data);
	  clock_gettime(CLOCK_REALTIME,&finish_time);
	  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
	  fprintf(treefile,"%5.3f %d fork %lu\n",elapsed,pid,data);
	  fflush(treefile);
	  debug2("   clone/fork/vfork - pid=%d\n",data);
	  break;
	case PTRACE_EVENT_EXIT:
	  clock_gettime(CLOCK_REALTIME,&finish_time);
	  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
	  ptrace(PTRACE_GETEVENTMSG,pid,NULL,&data);
	  // dump contents of proc/<pid>/comm
	  snprintf(stat_name,sizeof(stat_name),"/proc/%d/comm",pid);
	  if ((stat_file = fopen(stat_name,"r")) != NULL){
	    if (fgets(buffer,sizeof(buffer),stat_file) != NULL){
	      fprintf(treefile,"%5.3f %d comm ",elapsed,pid);
	      fputs(buffer,treefile);
	    }
	    fclose(stat_file);
	  }
	  // dump the full command line
	  if (tree_cmdline){
	    snprintf(stat_name,sizeof(stat_name),"/proc/%d/cmdline",pid);
	    fprintf(treefile,"%5.3f %d cmdline",elapsed,pid);
	    if ((stat_file = fopen(stat_name,"rb")) != NULL){
	      char *arg = 0;
	      size_t size = 0;
	      while (getdelim(&arg,&size,0,stat_file) != -1){
		fprintf(treefile," %s",arg);
	      }
	    }
	    fprintf(treefile,"\n");
	    fclose(stat_file);
	  }

	  // dump contents of proc/<pid>/stat
	  snprintf(stat_name,sizeof(stat_name),"/proc/%d/stat",pid);
	  if ((stat_file = fopen(stat_name,"r")) != NULL){
	    if (fgets(buffer,sizeof(buffer),stat_file) != NULL){
	      fprintf(treefile,"%5.3f %d exit ",elapsed,pid);
	      fputs(buffer,treefile);
	      fflush(treefile);
	    }
	    fclose(stat_file);
	  }
	  debug2("   exit - exit status=%d\n",data);
	  break;
	default:
	  // normal SIGTRAP - not sure how we got here, but continue without it.
	  // we seem to get these after a process has exited...
	  clock_gettime(CLOCK_REALTIME,&finish_time);
	  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
	  fprintf(treefile,"%5.3f %d unknown %x\n",elapsed,pid,status);
	  fflush(treefile);
	  debug2("   unknown event: %d status=%x %d\n",pid,status);
	  ptrace(PTRACE_CONT,pid,NULL,NULL);
	  continue;
	}
      } else if (WSTOPSIG(status) == SIGSTOP){
	// newly created process after fork/vfork/clone, continue without passing along signal
	debug2("   new pid\n");
	ptrace(PTRACE_CONT,pid,NULL,NULL);
	continue;
      } else if (WSTOPSIG(status) == (SIGTRAP | 0x80)){
	// stopped because of a system call
	ptrace(PTRACE_GETREGS,pid,0,&regs);
	if (last_syscall != regs.orig_rax){
	  syscall_entry = 1;
	} else {
	  syscall_entry = (1-syscall_entry);
	}
	last_syscall = regs.orig_rax;
	ptrace(PTRACE_SYSCALL,pid,NULL,NULL,NULL);
      } else {
	// pass other signals to the child
	clock_gettime(CLOCK_REALTIME,&finish_time);
	elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	  start_time.tv_sec - start_time.tv_nsec / 1000000000.0;	
	fprintf(treefile,"%5.3f %d signal %d\n",elapsed,pid,WSTOPSIG(status));
	fflush(treefile);
	ptrace(PTRACE_CONT,pid,NULL,WSTOPSIG(status));
      }
    } else if (WIFCONTINUED(status)){
      clock_gettime(CLOCK_REALTIME,&finish_time);
      elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	start_time.tv_sec - start_time.tv_nsec / 1000000000.0;	
      fprintf(treefile,"%5.3f %d continued\n",elapsed,pid);
      fflush(treefile);      
      // nothing here
    }
    // let the child go to the next event
    ptrace(PTRACE_CONT,pid,NULL,NULL);
  }
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

struct cache_event cache_events[] = {
  // L1D
  { PERF_TYPE_HW_CACHE, "l1d-read", PERF_COUNT_HW_CACHE_L1D|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16),
    PERF_COUNT_HW_CACHE_L1D, COUNTER_DCACHE },
  { PERF_TYPE_HW_CACHE,"l1d-read-miss", PERF_COUNT_HW_CACHE_L1D|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_MISS<<16),
    PERF_COUNT_HW_CACHE_L1D, COUNTER_DCACHE },

  // L1I
  { PERF_TYPE_HW_CACHE, "l1i-read", PERF_COUNT_HW_CACHE_L1I|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16),
    PERF_COUNT_HW_CACHE_L1I, COUNTER_ICACHE },
  { PERF_TYPE_HW_CACHE,"l1i-read-miss", PERF_COUNT_HW_CACHE_L1I|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_MISS<<16),
    PERF_COUNT_HW_CACHE_L1I, COUNTER_ICACHE },

  // TLB
  { PERF_TYPE_HW_CACHE, "dTLB-loads", PERF_COUNT_HW_CACHE_DTLB|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16),
    PERF_COUNT_HW_CACHE_DTLB, COUNTER_TLB },
  { PERF_TYPE_HW_CACHE,"dTLB-load-misses", PERF_COUNT_HW_CACHE_DTLB|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_MISS<<16),
    PERF_COUNT_HW_CACHE_DTLB, COUNTER_TLB },  

  { PERF_TYPE_HW_CACHE, "iTLB-loads", PERF_COUNT_HW_CACHE_ITLB|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16),
    PERF_COUNT_HW_CACHE_ITLB, COUNTER_TLB },
  { PERF_TYPE_HW_CACHE,"iTLB-load-misses", PERF_COUNT_HW_CACHE_ITLB|(PERF_COUNT_HW_CACHE_OP_READ<<8)|(PERF_COUNT_HW_CACHE_RESULT_MISS<<16),
    PERF_COUNT_HW_CACHE_ITLB, COUNTER_TLB },
  // instructions
  { PERF_TYPE_HARDWARE,"instructions",PERF_COUNT_HW_INSTRUCTIONS,0,COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB },
};

// creates and allocates a group for cache performance counters
struct counter_group *cache_counter_group(char *name,unsigned int mask){
  int i;
  int ncounters = 0;
  int num_counters_available = (nmi_running)?5:6;
  struct counter_group *cgroup = NULL;
  
  for (i=0;i<sizeof(cache_events)/sizeof(cache_events[0]);i++){
    if (mask & cache_events[i].use) ncounters++;
  }
  if (ncounters == 0) return NULL;
  cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup("cache");
  cgroup->type_id = PERF_TYPE_HW_CACHE;  
  cgroup->ncounters = ncounters;
  cgroup->cinfo = calloc(cgroup->ncounters,sizeof(struct counter_info));
  cgroup->mask = mask;

  int count = 0;
  unsigned int group_id = -1;
  for (i=0;i<sizeof(cache_events)/sizeof(cache_events[0]);i++){
    if (mask & cache_events[i].use){
      cgroup->cinfo[count].label = cache_events[i].name;
      cgroup->cinfo[count].config = cache_events[i].config;
      if (cpu_info->vendor == VENDOR_AMD){
	if ((count % num_counters_available) == 0)
	  cgroup->cinfo[count].is_group_leader = 1;
      }
      count++;
    }
  }
  return cgroup;
}

// creates and allocates a group for raw hardware performance counters
struct counter_group *raw_counter_group(char *name,unsigned int mask){
  int i;
  int available_counters = (nmi_running)?5:6;
  
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
      cgroup->cinfo[count].device_type = events[i].device_type;
      // chunk into multiplex groups if needed
      if (cpu_info->vendor == VENDOR_AMD){
	if (((count % available_counters) == 0) || (events[i].device_type != PERF_TYPE_RAW))
	  cgroup->cinfo[count].is_group_leader = 1;
      }
      count++;
    }
  }
  
  return cgroup;
}

void setup_counter_groups(struct counter_group **counter_group_list){

  int i,count;
  struct counter_group *cgroup;

  // note: These get pushed onto a linked list, so last listed is first printed
  if (counter_mask & (COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB)){
    if (cgroup = cache_counter_group("cache",counter_mask)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;
    }
  }

  if (counter_mask & COUNTER_FLOAT){
    if (cgroup = raw_counter_group("float",COUNTER_FLOAT)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }
  }

  if (counter_mask & COUNTER_OPCACHE){
    if (cgroup = raw_counter_group("op cache",COUNTER_OPCACHE)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }    
  }
  

  if (counter_mask & COUNTER_MEMORY){
    if (cgroup = raw_counter_group("memory",COUNTER_MEMORY)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }
  }

  if (counter_mask & COUNTER_L3CACHE){
    if (cgroup = raw_counter_group("l3 cache",COUNTER_L3CACHE)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }    
  }  

  if (counter_mask & COUNTER_L2CACHE){
    if (cgroup = raw_counter_group("l2 cache",COUNTER_L2CACHE)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }    
  }

  if (counter_mask & COUNTER_BRANCH){
    if (cgroup = raw_counter_group("branch",COUNTER_BRANCH)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }        
  }
      
  if (counter_mask & COUNTER_TOPDOWN2){
    if (cgroup = raw_counter_group("topdown2",COUNTER_TOPDOWN2)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }
  }  
  
  if (counter_mask & COUNTER_TOPDOWN){
    if (cgroup = raw_counter_group("topdown",COUNTER_TOPDOWN)){
      cgroup->next = *counter_group_list;
      *counter_group_list = cgroup;      
    }
  }  

  if (counter_mask & COUNTER_IPC){
    if (cgroup = raw_counter_group("ipc",COUNTER_IPC)){
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
  int group_id = -1;
  struct counter_group *cgroup;
  struct perf_event_attr pe;

  struct cpu_core_info *coreinfo;
  struct counter_def *counter_def;
  int ncounters;

  // create system-wide counters
  for (cgroup = counter_group_list;cgroup;cgroup = cgroup->next){
    debug("Setting up %s counters\n",cgroup->label);
    for (i=0;i<cgroup->ncounters;i++){
      if (cpu_info->vendor == VENDOR_INTEL){
	group_id = intel_group_id;
      } else {
	if (cgroup->cinfo[i].is_group_leader == 1) group_id = -1;
      }
      memset(&pe,0,sizeof(pe));
      pe.type = (cgroup->type_id==PERF_TYPE_RAW)?cgroup->cinfo[i].device_type:cgroup->type_id;
      pe.config = cgroup->cinfo[i].config;
      pe.sample_type = PERF_SAMPLE_IDENTIFIER; // is this needed?
      pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
      pe.size = sizeof(struct perf_event_attr);
      //      pe.exclude_guest = 1; // is this needed
      pe.inherit = 1;
      pe.disabled = 1;

      if (pe.type == PERF_TYPE_L3){
	// read from any process on this core
	pe.exclude_guest = 0;
	status = perf_event_open(&pe,-1,0,group_id,0);
      } else {
	// read from current process
	status = perf_event_open(&pe,0,-1,group_id,PERF_FLAG_FD_CLOEXEC);	
      }


      if (status == -1){
	error("unable to create %s performance counter, name=%s, errno=%d - %s\n",
	      cgroup->label,cgroup->cinfo[i].label,errno,strerror(errno));
	cgroup->cinfo[i].fd = -1;
	nerror++;
      } else {
	cgroup->cinfo[i].fd = status;
	ioctl(cgroup->cinfo[i].fd,PERF_EVENT_IOC_ENABLE,0);
	debug("   create %s performance counter, name=%s\n",cgroup->label,cgroup->cinfo[i].label);
	if (group_id == -1){
	  group_id = cgroup->cinfo[i].fd;
	  intel_group_id = group_id;
	}
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

void read_counters(struct counter_group *counter_group_list,int stop_counters){
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
	  cgroup->cinfo[i].prev_read = cgroup->cinfo[i].last_read;
	  cgroup->cinfo[i].last_read = rf.value;
	  // save only the delta since this was last read...
	  cgroup->cinfo[i].value = rf.value - cgroup->cinfo[i].prev_read;
	  cgroup->cinfo[i].time_running = rf.time_running;
	  cgroup->cinfo[i].time_enabled = rf.time_enabled;
	  // TODO: add scaling for performance counters...

	  if (stop_counters){
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
}

int check_nmi_watchdog(void){
  int c;
  FILE *fp;
  if ((fp = fopen("/proc/sys/kernel/nmi_watchdog","r")) != NULL){
    c = getc(fp);
    if (c == '1'){
      warning("/proc/sys/kernel/nmi_watchdog is running, missing performance counters\n");
      nmi_running = 1;
    }
    fclose(fp);
  }
}

void print_usage(struct rusage *rusage,enum output_format oformat){
  double elapsed;
  
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
  switch(oformat){
  case PRINT_CSV_HEADER:
    fprintf(outfile,"elapsed,utime,stime,");
    break;
  case PRINT_CSV:
    fprintf(outfile,"%4.3f,",elapsed);
    fprintf(outfile,"%4.3f,",(double) rusage->ru_utime.tv_sec + rusage->ru_utime.tv_usec / 1000000.0);
    fprintf(outfile,"%4.3f,",(double) rusage->ru_stime.tv_sec + rusage->ru_stime.tv_usec / 1000000.0);    
    break;
  case PRINT_NORMAL:
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
    fprintf(outfile,"inblock              %-15lu# %4.2f/sec\n",
	    rusage->ru_inblock,rusage->ru_inblock/elapsed);
    fprintf(outfile,"onblock              %-15lu# %4.2f/sec\n",
	    rusage->ru_oublock,rusage->ru_oublock/elapsed);
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

void print_ipc(struct counter_group *cgroup,enum output_format oformat){
  int i;
  unsigned long cpu_cycles=0,scaled_cpu_cycles=0;
  unsigned long instructions=0;
  unsigned long branches=0;
  unsigned long branch_misses=0;
  double elapsed;
  double scale;
  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"ipc,");
    return;
  }
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;  

  for (i=0;i<cgroup->ncounters;i++){
    if (!strcmp(cgroup->cinfo[i].label,"cpu-cycles")){
      cpu_cycles = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
      scaled_cpu_cycles = (double) cpu_cycles * (double) cgroup->cinfo[i].time_enabled / (double) cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"instructions")){
      instructions = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"branches")){
      branches = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    } else if (!strcmp(cgroup->cinfo[i].label,"branch-misses")){
      branch_misses = (double) cgroup->cinfo[i].value * cgroup->cinfo[i].time_enabled / cgroup->cinfo[i].time_running;
    }
  }

  switch(oformat){
  case PRINT_NORMAL:
    if (cpu_cycles){
      fprintf(outfile,"cpu-cycles           %-14lu # %4.2f GHz\n",cpu_cycles,(double) cpu_cycles / elapsed / 1000000000.0 / cpu_info->num_cores_available / (aflag?cpu_info->num_cores_available:1));
      fprintf(outfile,"instructions         %-14lu # %4.2f IPC\n",instructions,(double) instructions / cpu_cycles);
      break;
    case PRINT_CSV:
      fprintf(outfile,"%4.2f,",(double) instructions / cpu_cycles);
      break;
    }
  }
}

void print_topdown(struct counter_group *cgroup,enum output_format oformat,int mask){
  unsigned long slots=0;
  unsigned long slots_no_contention = 0;
  unsigned long retiring=0;
  unsigned long frontend=0;
  unsigned long frontend_latency=0;
  unsigned long frontend_bandwidth=0;
  unsigned long backend=0;
  unsigned long retire_not_complete=0;
  unsigned long retire_load_not_complete=0;
  unsigned long backend_cpu=0;
  unsigned long backend_memory=0;
  unsigned long speculation=0;
  unsigned long speculation_branches=0;
  unsigned long speculation_pipeline=0;
  unsigned long branches_mispredicted=0;
  unsigned long resyncs_or_nc_redirects=0;
  unsigned long contention=0;
  unsigned long retire_ucode=0;
  unsigned long retire_fastpath=0;
  int too_short = 0;
  struct counter_info *cinfo;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"retire,frontend,backend,speculate,");
  }

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    if (cinfo = find_ci_label(cgroup,"slots")) slots = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-retiring")) retiring = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-fe-bound")) frontend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-be-bound")) backend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"core.topdown-bad-spec")) speculation = cinfo->value;

    // backend level 2
    if (cinfo = find_ci_label(cgroup,"core.topdown-mem-bound")){
      backend_memory = cinfo->value;
      backend_cpu = backend - backend_memory;
    }

    // speculation level 2
    if (cinfo = find_ci_label(cgroup,"core.topdown-br-mispredict")){
      speculation_branches = cinfo->value;
      speculation_pipeline = speculation - speculation_branches;
    }

    // frontend level 2
    if (cinfo = find_ci_label(cgroup,"core.topdown-fetch-lat")){
      frontend_latency = cinfo->value;
      frontend_bandwidth = frontend - frontend_latency;
    }

    // retire level 2
    if (cinfo = find_ci_label(cgroup,"core.topdown-heavy-ops")){
      retire_ucode = cinfo->value;
      retire_fastpath = retiring - retire_ucode;
    }
    
    slots_no_contention = slots;
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"cpu-cycles")) slots = cinfo->value * 6;
    if (cinfo = find_ci_label(cgroup,"ex_ret_ops")) retiring = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.no_ops_from_frontend")) frontend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.backend_stalls")) backend = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.smt_contention")) contention = cinfo->value;    
    if (cinfo = find_ci_label(cgroup,"de_src_op_disp.all")){
      if (cinfo->time_running == 0) too_short = 1;
      speculation = cinfo->value - retiring;
    }
    // backend level 2
    if (cinfo = find_ci_label(cgroup,"ex_no_retire.load_not_complete"))
      retire_load_not_complete = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"ex_no_retire.not_complete"))
      retire_not_complete = cinfo->value;
    if (retire_load_not_complete && retire_not_complete){
      backend_memory = backend * (double) retire_load_not_complete / retire_not_complete;
      backend_cpu = backend - backend_memory;
    }

    // speculation level 2
    if (cinfo = find_ci_label(cgroup,"ex_ret_brn_misp"))
      branches_mispredicted = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"resyncs_or_nc_redirects"))
      resyncs_or_nc_redirects = cinfo->value;
    if (branches_mispredicted && resyncs_or_nc_redirects){
      speculation_pipeline = speculation * (double) resyncs_or_nc_redirects / (resyncs_or_nc_redirects + branches_mispredicted);
      speculation_branches = speculation - speculation_pipeline;
    }

    // frontend level 2
    if (cinfo = find_ci_label(cgroup,"de_no_dispatch_per_slot.no_ops_from_frontend.cmask_0x6")){
      frontend_latency = cinfo->value*6;
      frontend_bandwidth = frontend - frontend_latency;
    }

    // retire level 2
    if (cinfo = find_ci_label(cgroup,"ex_ret_ucode_ops")){
      retire_ucode = retiring * (double) cinfo->value / retiring;
      retire_fastpath = retiring - retire_ucode;
    }
    
    slots_no_contention = slots - contention;
    break;
  default:
    return;
  }

  if (slots){
    switch(oformat){
    case PRINT_CSV:
      fprintf(outfile,"%4.1f,",(double) retiring/slots_no_contention*100);
      fprintf(outfile,"%4.1f,",(double) frontend/slots_no_contention*100);
      fprintf(outfile,"%4.1f,",(double) backend/slots_no_contention*100);
      fprintf(outfile,"%4.1f,",(double) speculation/slots_no_contention*100);
      
      break;
    case PRINT_NORMAL:
      if (too_short){
	warning("unable to read performance counter, is runtime too short?\n");
      } else {
	fprintf(outfile,"slots                %-14lu #\n",slots);
	fprintf(outfile,"retiring             %-14lu # %4.1f%% (%4.1lf%%)\n",
		retiring,(double) retiring/slots*100, (double) retiring/slots_no_contention*100);
	if (retire_ucode && frontend_bandwidth){
	  fprintf(outfile,"-- ucode             %-14lu #    %4.1f%%\n",
		  retire_ucode,(double) retire_ucode/slots*100);
	  fprintf(outfile,"-- fastpath          %-14lu #    %4.1f%%\n",
		  retire_fastpath,(double) retire_fastpath/slots*100);
	}		
	fprintf(outfile,"frontend             %-14lu # %4.1f%% (%4.1lf%%)\n",
		frontend,(double) frontend/slots*100, (double) frontend/slots_no_contention*100);
	if (frontend_latency && frontend_bandwidth){
	  fprintf(outfile,"-- latency           %-14lu #    %4.1f%%\n",
		  frontend_latency,(double) frontend_latency/slots*100);
	  fprintf(outfile,"-- bandwidth         %-14lu #    %4.1f%%\n",
		  frontend_bandwidth,(double) frontend_bandwidth/slots*100);
	}	
	fprintf(outfile,"backend              %-14lu # %4.1f%% (%4.1lf%%)\n",
		backend,(double) backend/slots*100, (double) backend/slots_no_contention*100);
	if (backend_memory && backend_cpu){
	  fprintf(outfile,"-- cpu               %-14lu #    %4.1f%%\n",
		  backend_cpu,(double) backend_cpu/slots*100);
	  fprintf(outfile,"-- memory            %-14lu #    %4.1f%%\n",
		  backend_memory,(double) backend_memory/slots*100);
	}
	fprintf(outfile,"speculation          %-14lu # %4.1f%% (%4.1lf%%)\n",
		speculation,(double) speculation/slots*100, (double) speculation/slots_no_contention*100);
	if (speculation_pipeline && speculation_branches){
	  fprintf(outfile,"-- branch mispredict %-14lu #    %4.1lf%%\n",
		  speculation_branches,(double) speculation_branches/slots*100);
	  fprintf(outfile,"-- pipeline restart  %-14lu #    %4.1f%%\n",
		  speculation_pipeline,(double) speculation_pipeline/slots*100);
	}
	fprintf(outfile,"smt-contention       %-14lu # %4.1f%% ( 0.0%%)\n",contention,(double) contention/slots*100);
      }
      break;
    }
  }
}


void print_branch(struct counter_group *cgroup,enum output_format oformat){
  struct counter_info *cinfo;
  unsigned long instructions = 0;
  unsigned long cpu_cycles = 0;
  unsigned long branches = 0;
  unsigned long branch_miss = 0;
  unsigned long cond_branches = 0;
  unsigned long ind_branches = 0;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"branch miss,");
    return;
  }  

  if (cinfo = find_ci_label(cgroup,"instructions"))
    instructions = cinfo->value;
  if (cinfo = find_ci_label(cgroup,"cpu-cycles"))
    cpu_cycles = cinfo->value;
  switch (cpu_info->vendor){
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"branch-instructions"))
      branches = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"branch-misses"))
      branch_miss = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"conditional-branches"))
      cond_branches = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"indirect-branches"))
      ind_branches = cinfo->value;
    break;
  case VENDOR_INTEL:
    if (cinfo = find_ci_label(cgroup,"br_inst_retired.all_branches"))
      branches = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"br_misp_retired.all_branches"))
      branch_miss = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"br_inst_retired.cond"))
      cond_branches = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"br_inst_retired.indirect"))
      ind_branches = cinfo->value;
    break;
    break;
  }
  

  if (csvflag){
      fprintf(outfile,"%4.2f%%,",(double) branch_miss / branches * 100.0);
  } else {
      fprintf(outfile,"branches             %-14lu # %4.3f branches per 1000 inst\n",
	      branches,(double) branches / instructions * 1000.0);
      fprintf(outfile,"branch misses        %-14lu # %4.2f%% branch miss\n",
	      branch_miss, (double) branch_miss / branches * 100.0);
      fprintf(outfile,"conditional          %-14lu # %4.3f conditional branches per 1000 inst\n",
	      cond_branches,(double) cond_branches / instructions * 1000.0);
      fprintf(outfile,"indirect             %-14lu # %4.3f indirect branches per 1000 inst\n",
	      ind_branches,(double) ind_branches / instructions * 1000.0);      
  }
}

void print_l2cache(struct counter_group *cgroup,enum output_format oformat){
  struct counter_info *cinfo;
  unsigned long l2_access=0, l2_miss=0;
  unsigned long instructions = 0;
  unsigned long l2_from_l1_no_prefetch = 0;
  unsigned long l2_pf_hit_l2 = 0;
  unsigned long l2_pf_hit_l3 = 0;
  unsigned long l2_pf_miss_l3 = 0;
  unsigned long l1_miss_l2_miss = 0;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"l2miss,");
    return;
  }  

  if (cinfo = find_ci_label(cgroup,"instructions"))
    instructions = cinfo->value;

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    if (cinfo = find_ci_label(cgroup,"l2_request.all")) l2_access = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l2_request.miss")) l2_miss = cinfo->value;
    if (csvflag){
      fprintf(outfile,"%4.2f%%,",(double) l2_miss / l2_access * 100.0);
    } else {    
      fprintf(outfile,"l2 access            %-14lu # %4.3f l2 access per 1000 inst\n",
	      l2_access,(double) l2_access / instructions*1000.0);
      fprintf(outfile,"l2 miss              %-14lu # %4.2f%% l2 miss\n",
	      l2_miss, (double) l2_miss / l2_access * 100.0);
    }
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"l2_request_g1.all_no_prefetch"))
      l2_from_l1_no_prefetch = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l2_pf_hit_l2"))
      l2_pf_hit_l2 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l2_pf_miss_l2_hit_l3"))
      l2_pf_hit_l3 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l2_pf_miss_l2_l3"))
      l2_pf_miss_l3 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l2_cache_req_stat.ic_dc_miss_in_l2"))
      l1_miss_l2_miss = cinfo->value;

    l2_access = l2_from_l1_no_prefetch + l2_pf_hit_l2 + l2_pf_hit_l3 + l2_pf_miss_l3;
    l2_miss = l1_miss_l2_miss + l2_pf_hit_l3 + l2_pf_miss_l3;

    if (csvflag){
      fprintf(outfile,"%4.2f%%,",(double) l2_miss / l2_access * 100.0);
    } else {
      fprintf(outfile,"instructions         %-14lu # %4.3f l2 access per 1000 inst\n",
	      instructions,(double) l2_access / instructions*1000.0);
      fprintf(outfile,"l2 hit from l1       %-14lu # %4.2f%% l2 miss\n",
	      l2_from_l1_no_prefetch, (double) l2_miss / l2_access * 100.0);
      fprintf(outfile,"l2 miss from l1      %-14lu #\n",l1_miss_l2_miss);
      fprintf(outfile,"l2 hit from l2 pf    %-14lu #\n",l2_pf_hit_l2);
      fprintf(outfile,"l3 hit from l2 pf    %-14lu #\n",l2_pf_hit_l3);
      fprintf(outfile,"l3 miss from l2 pf   %-14lu #\n",l2_pf_miss_l3);
    }
    break;
  default:
    return;
  }
}

void print_l3cache(struct counter_group *cgroup,enum output_format oformat){
  struct counter_info *cinfo;
  unsigned long l3_access=0, l3_miss=0;
  unsigned long instructions = 0;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"l3miss,");
    return;
  }  

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"instructions"))
      instructions = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l3_lookup_state.all_coherent_accesses_to_l3"))
      l3_access = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"l3_lookup_state.l3_miss"))
      l3_miss = cinfo->value;

    if (csvflag){
      fprintf(outfile,"%4.2f%%,",(double) l3_miss / l3_access * 100.0);
    } else {
      fprintf(outfile,"instructions         %-14lu # %4.3f l3 access per 1000 inst\n",
	      instructions,(double) l3_access / instructions*1000.0);
      fprintf(outfile,"l3 access            %-14lu # %4.3f l3 access per 1000 inst\n",
	      l3_access, (double) l3_access / instructions*1000.0);
      fprintf(outfile,"l3 miss              %-14lu # %4.2f%% l3 miss\n",
	      l3_miss, (double) l3_miss / l3_access * 100.0);
    }
    break;    
  default:
    return;
  }
}

void print_memory(struct counter_group *cgroup,enum output_format oformat){
  unsigned long data_cache_local=0;
  unsigned long data_cache_remote=0;
  unsigned long prefetch_local=0;
  unsigned long prefetch_remote=0;
  struct counter_info *cinfo;
  double elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;    

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"bandwidth,");
    return;
  }  

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"ls_data_cache_refills.local_all"))
      data_cache_local = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"ls_data_cache_refills.remote_all"))
      data_cache_remote = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"ls_hwpref_data_cache_refills.local_all"))
      prefetch_local = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"ls_hwpref_data_cache_refills.remote_all"))
      prefetch_remote = cinfo->value;        
    break;
  default:
    return;
  }

  if (csvflag){
    fprintf(outfile,"%4.1f,",(double) (data_cache_local+data_cache_remote+prefetch_local+prefetch_remote)*64.0/1024/1024/elapsed);
  } else {
    fprintf(outfile,"local bandwidth      %-14lu # %4.1f MB/s\n",
	    data_cache_local+prefetch_local,
	    (double)(data_cache_local+prefetch_local)*64.0/1024.0/1024.0/elapsed);
    fprintf(outfile,"remote bandwidth     %-14lu # %4.1f MB/s\n",
	    data_cache_remote+prefetch_remote,
	    (double)(data_cache_remote+prefetch_remote)*64.0/1024.0/1024.0/elapsed);
  }  
}

void print_cache(struct counter_group *cgroup,
		 enum output_format oformat,
		 char *name,char *access_counter,char *miss_counter){
  struct counter_info *cinfo;
  char miss_name[20];
  unsigned long instructions = 0;
  unsigned long access = 0;
  unsigned long miss = 0;
  snprintf(miss_name,sizeof(miss_name),"%s miss",name);

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"%s miss,",name);
    return;
  }  

  if (cinfo = find_ci_label(cgroup,"instructions"))
    instructions = cinfo->value;
  if (cinfo = find_ci_label(cgroup,access_counter))
    access = cinfo->value;
  if (cinfo = find_ci_label(cgroup,miss_counter))
    miss = cinfo->value;  

  if (csvflag){
    fprintf(outfile,"%4.2f%%,",(double) miss / access * 100.0);
  } else {
    fprintf(outfile,"instructions         %-14lu #\n",instructions);
    fprintf(outfile,"%-20s %-14lu # %4.3f %s per 1000 inst\n",
	    name,access,(double) access / instructions * 1000.0,name);
    fprintf(outfile,"%-20s %-14lu # %4.2f%% %s\n",
	    miss_name, miss, (double) miss / access * 100.0, miss_name);
  }
}

void print_opcache(struct counter_group *cgroup,enum output_format oformat){
  print_cache(cgroup,oformat,"opcache",
	      "op_cache_hit_miss.all_op_cache_accesses",
	      "op_cache_hit_miss.op_cache_miss");
}

void print_dcache(struct counter_group *cgroup,enum output_format oformat){
  print_cache(cgroup,oformat,
	      "L1-dcache",
	      "l1d-read",
	      "l1d-read-miss");
}

void print_icache(struct counter_group *cgroup,enum output_format oformat){
  print_cache(cgroup,oformat,
	      "L1-icache",
	      "l1i-read",
	      "l1i-read-miss");
}

void print_itlb(struct counter_group *cgroup,enum output_format oformat){
  print_cache(cgroup,oformat,
	      "iTLB",
	      "iTLB-loads",
	      "iTLB-load-misses");
}

void print_dtlb(struct counter_group *cgroup,enum output_format oformat){
  print_cache(cgroup,oformat,
	      "dTLB",
	      "dTLB-loads",
	      "dTLB-load-misses");
}

void print_float(struct counter_group *cgroup,enum output_format oformat){
  struct counter_info *cinfo;
  unsigned long instructions = 0;
  unsigned long float_512 = 0;
  unsigned long float_256 = 0;
  unsigned long float_128 = 0;
  unsigned long float_mmx = 0;
  unsigned long float_scalar = 0;
  unsigned long float_all = 0;

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"float,");
    return;
  }  

  switch(cpu_info->vendor){
  case VENDOR_INTEL:
    break;
  case VENDOR_AMD:
    if (cinfo = find_ci_label(cgroup,"instructions"))
      instructions = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"fp_ret_fops_AVX512"))
      float_512 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"fp_ret_fops_AVX256"))
      float_256 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"fp_ret_fops_AVX128"))
      float_128 = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"fp_ret_fops_MMX"))
      float_mmx = cinfo->value;
    if (cinfo = find_ci_label(cgroup,"fp_ret_fops_scalar"))
      float_scalar = cinfo->value;

    float_all = float_512 + float_256 + float_128 + float_mmx + float_scalar;

    if (csvflag){
      fprintf(outfile,"%4.2f%%,",(double) float_all / instructions * 100.0);
    } else {
      fprintf(outfile,"instructions         %-14lu # %4.3f float per 1000 inst\n",
	      instructions,(double) float_all / instructions*1000.0);
      fprintf(outfile,"float 512            %-14lu # %4.3f AVX-512 per 1000 inst\n",
	      float_512,(double) float_512 / instructions*1000.0);      
      fprintf(outfile,"float 256            %-14lu # %4.3f AVX-256 per 1000 inst\n",
	      float_256,(double) float_256 / instructions*1000.0);      
      fprintf(outfile,"float 128            %-14lu # %4.3f AVX-128 per 1000 inst\n",
	      float_128,(double) float_128 / instructions*1000.0);
      fprintf(outfile,"float MMX            %-14lu # %4.3f MMX per 1000 inst\n",
	      float_mmx,(double) float_mmx / instructions*1000.0);      
      fprintf(outfile,"float scalar         %-14lu # %4.3f scalar per 1000 inst\n",
	      float_scalar,(double) float_scalar / instructions*1000.0);      
    }
    break;    
  default:
    return;
  }
}

void print_software(struct counter_group *cgroup,enum output_format oformat){
  int i;
  struct counter_info *task_info = find_ci_label(cgroup,"task-clock");
  double task_time = (double) task_info->value / 1000000000.0;
  struct counter_info *cinfo;
  if (oformat == PRINT_CSV_HEADER){
    return;
  }
  for (i=0;i<cgroup->ncounters;i++){
    fprintf(outfile,"%-20s %-14lu",cgroup->cinfo[i].label,cgroup->cinfo[i].value);
    if (!strcmp(cgroup->cinfo[i].label,"task-clock") ||
	!strcmp(cgroup->cinfo[i].label,"cpu-clock")){
      fprintf(outfile," # %4.3f seconds",
	     (double) cgroup->cinfo[i].value / 1000000000.0);
    } else {
      fprintf(outfile," # %4.3f/sec",cgroup->cinfo[i].value / task_time);
	     
    }
    fprintf(outfile,"\n");
  }
}

void print_metrics(struct counter_group *counter_group_list,enum output_format oformat){
  struct counter_group *cgroup;
  for (cgroup = counter_group_list;cgroup;cgroup = cgroup->next){
    if (!strcmp(cgroup->label,"software")){
      print_software(cgroup,oformat);
    } else if (cgroup->mask & COUNTER_IPC){
      print_ipc(cgroup,oformat);
    } else if (cgroup->mask & COUNTER_TOPDOWN){
      print_topdown(cgroup,oformat,COUNTER_TOPDOWN);
    } else if (cgroup->mask & COUNTER_TOPDOWN2){
      print_topdown(cgroup,oformat,COUNTER_TOPDOWN2);
    } else if (cgroup->mask & COUNTER_BRANCH){
      print_branch(cgroup,oformat);
    } else if (cgroup->mask & COUNTER_L2CACHE){
      print_l2cache(cgroup,oformat);      
    } else if (cgroup->mask & COUNTER_L3CACHE){
      print_l3cache(cgroup,oformat);      
    } else if (cgroup->mask & COUNTER_OPCACHE){
      print_opcache(cgroup,oformat);
    } else if (cgroup->mask & (COUNTER_DCACHE|COUNTER_ICACHE|COUNTER_TLB)){
      if (cgroup->mask & COUNTER_DCACHE) print_dcache(cgroup,oformat);
      if (cgroup->mask & COUNTER_ICACHE) print_icache(cgroup,oformat);
      if (cgroup->mask & COUNTER_TLB){
	print_dtlb(cgroup,oformat);	  
	print_itlb(cgroup,oformat);
      }
    } else if (cgroup->mask & COUNTER_FLOAT){
      print_float(cgroup,oformat);
    }
  }
}

// simple alarm(2) based timer
void timer_callback(int signum){
  int i;
  struct rusage rusage;
  double elapsed;

  clock_gettime(CLOCK_REALTIME,&finish_time);
  elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
    start_time.tv_sec - start_time.tv_nsec / 1000000000.0;

  read_counters(cpu_info->systemwide_counters,0);
  if (csvflag){
    fprintf(outfile,"%4.1f,",elapsed);
  }
  print_metrics(cpu_info->systemwide_counters,csvflag?PRINT_CSV:PRINT_NORMAL);
  if (csvflag) fprintf(outfile,"\n");

  if (is_still_running){
    alarm(interval);
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
      fatal("usage: %s -[abcistv][-o <file>] <cmd><args>...\n"
	    "\t--per-core or -a          - metrics per core\n"
	    "\t--rusage or -r            - show getrusage(2) information\n"
	    "\t--tree <file>             - create CSV of processes\n"
	    "\t--tree-cmdline            - record full command lines\n"
	    "\t-o <file>                 - send output to file\n"
	    "\t--csv                     - create csv output\n"
	    "\t--interval <sec>          - read every <sec> seconds\n"
	    "\t--verbose or -v           - print verbose information\n"
	    "\n"
	    "\t--software or -s          - software counters\n"
	    "\t--ipc or i                - IPC counters\n"
	    "\t--branch or -b            - branch counters\n"
	    "\t--dcache                  - L1 dcache counters\n"
	    "\t--icache                  - L1 icache counters\n"
	    "\t--cache2 or -c            - L2 cache counters\n"
	    "\t--cache3                  - L3 cache counters\n"
	    "\t--memory                  - memory counters\n"
	    "\t--opcache                 - opcache counters\n"
	    "\t--tlb                     - TLB counters\n"
	    "\t--topdown or -t           - topdown counters, level 1\n"
	    "\t--topdown2                - topdown counters, level 2\n"
	    ,argv[0]);
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  check_nmi_watchdog();

  // parse the event tables
  setup_raw_events();

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

  signal(SIGINT,SIG_IGN);

  // create CSV headers
  if (csvflag){
    if (interval){
      fprintf(outfile,"time,");
    } else if (xflag){
      print_usage(NULL,PRINT_CSV_HEADER);
    }
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV_HEADER);
    fprintf(outfile,"\n");
  }

  // let the child start after two seconds
  sleep(2);

  clock_gettime(CLOCK_REALTIME,&start_time);
  if (launch_child(command_line_argc,command_line_argv,envp)){
    fatal("unable to launch %s\n",command_line_argv[0]);
  }

  if (interval){
    signal(SIGALRM,timer_callback);
    alarm(interval);
  }

  write(child_pipe[1],"start\n",6);
  if (treeflag){
    ptrace_setup(child_pid);
    fprintf(treefile,"0.000 %d root\n",child_pid);
    ptrace_loop();
    getrusage(RUSAGE_CHILDREN,&rusage);
  } else {
    // without ptrace, this process waits to complete
    wait4(child_pid,&status,0,&rusage);
  }

  is_still_running = 0;
  
  clock_gettime(CLOCK_REALTIME,&finish_time);

  // -----  
  // stop core-specific and system-wide counters
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters)
      read_counters(cpu_info->coreinfo[i].core_specific_counters,1);
  }
  read_counters(cpu_info->systemwide_counters,1);

  if (csvflag){
    if (interval){
      double elapsed;
  
      elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	start_time.tv_sec - start_time.tv_nsec / 1000000000.0;    
      fprintf(outfile,"%4.1f,",elapsed);
    } else if (xflag){
      print_usage(&rusage,PRINT_CSV);
    }
  } else if (xflag){
    print_usage(&rusage,PRINT_NORMAL);
  }

  print_metrics(cpu_info->systemwide_counters,csvflag?PRINT_CSV:PRINT_NORMAL);
  if (csvflag) fprintf(outfile,"\n");
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters){
      if (csvflag){
	print_usage(&rusage,PRINT_CSV);
      } else {
	fprintf(outfile,"##### core %2d #######################\n",i);
      }
      print_metrics(cpu_info->coreinfo[i].core_specific_counters,csvflag?PRINT_CSV:PRINT_NORMAL);
      if (csvflag) fprintf(outfile,"\n");
    }
  }

  if (oflag) fclose(outfile);
  // -----
  return 0;
}
