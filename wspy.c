/*
 * wspy.c - workload spy - main program
 */
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "wspy.h"
#if AMDGPU
#include "gpu_info.h"
#endif
#include "error.h"

int aflag = 0;
int oflag = 0;
int sflag = 0;
int vflag = 0;
int xflag = 1;
int csvflag = 0;
int interval = 0;
int treeflag = 0;
int tree_cmdline = 0;
int tree_open = 0;
int trace_syscall = 0;

FILE *treefile = NULL;
FILE *outfile = NULL;
unsigned int counter_mask = COUNTER_IPC;

int num_procs;
int clocks_per_second;
int command_line_argc;
char **command_line_argv;

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
    { "cache1", no_argument, 0, 39 },
    { "no-cache1", no_argument, 0, 40 },
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
    { "system", no_argument, 0, 36 },
    { "no-system", no_argument, 0, 37 },
    { "tlb", no_argument, 0, 25 }, 
    { "no-tlb", no_argument, 0, 26 },
    { "topdown", no_argument, 0, 27 }, // (t)
    { "no-topdown", no_argument, 0, 28 },
    { "topdown2", no_argument, 0, 29 }, //
    { "no-topdown2", no_argument, 0, 30 },
    { "topdown-frontend",no_argument, 0, 42 },
    { "no-topdown-frontend",no_argument, 0, 43 },
    { "topdown-backend",no_argument,0,46 },
    { "no-topdown-backend",no_argument,0,47 },
    { "topdown-optlb",no_argument,0,44 },
    { "no-topdown-optlb",no_argument,0,45 },
    { "tree", required_argument, 0, 31 }, //
    { "tree-cmdline",no_argument,0,35 },
    { "tree-open",no_argument,0, 38 },
    { "tree-vmsize",no_argument,0,41 },
    { "verbose", no_argument, 0, 32 },
    { 0,0,0,0 },
  };
  while ((opt = getopt_long(argc,argv,"+abcio:rsStv",long_options,NULL)) != -1){
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
    case 39: // --cache1
      counter_mask |= COUNTER_L1CACHE;
      break;
    case 40:
      counter_mask &= (~COUNTER_L1CACHE);
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
    case 42: // --topdown-frontend
      counter_mask |= COUNTER_TOPDOWN_FE;
      break;
    case 43: // --no-topdown-frontend
      counter_mask &= (~COUNTER_TOPDOWN_FE);
      break;
    case 46: // --topdown-backend
      counter_mask |= COUNTER_TOPDOWN_BE;
      break;
    case 47: // --no-topdown-backend
      counter_mask &= (~COUNTER_TOPDOWN_BE);
      break;      
    case 44: // --topdown-optlb
      counter_mask |= COUNTER_TOPDOWN_OP;
      break;
    case 45: // --no-topdown-optlb
      counter_mask &= (~COUNTER_TOPDOWN_OP);
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
    case 36: // --system
    case 's':
      sflag = 1;
      break;
    case 37:
      sflag = 0;
      break;
    case 38:
      tree_open = 1;
      trace_syscall = 1;
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

int main(int argc,char *const argv[],char *const envp[]){
  int i;
  int status;
  struct rusage rusage;
  struct counter_group *cgroup;
  outfile = stdout;
  num_procs = get_nprocs();
  clocks_per_second = sysconf(_SC_CLK_TCK);
  
  if (parse_options(argc,argv)){
      fatal("usage: %s -[abcistv][-o <file>] <cmd><args>...\n"
	    "\t--per-core or -a          - metrics per core\n"
	    "\t--rusage or -r            - show getrusage(2) information\n"
	    "\t--tree <file>             - create CSV of processes\n"
	    "\t--tree-cmdline            - record full command lines\n"
	    "\t--tree-vmsize             - virtual memory size\n"
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
	    "\t--topdown-frontend        - topdown related to fe\n"
	    "\t--topdown-optlb           - topdown related to opcache, dtlb\n"
	    ,argv[0]);
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  check_nmi_watchdog();

#if AMDGPU
  gpu_info_initialize();
#endif

  // parse the event tables
  setup_raw_events();

  // set up either system-wide or core-specific counters
  if (aflag){
    for (i=0;i<cpu_info->num_cores;i++){
      if (cpu_info->coreinfo[i].is_available &&
	  ((cpu_info->coreinfo[i].vendor == CORE_AMD_ZEN)||(cpu_info->coreinfo[i].vendor == CORE_AMD_ZEN5)||(cpu_info->coreinfo[i].vendor == CORE_INTEL_CORE))){
	setup_counter_groups(&cpu_info->coreinfo[i].core_specific_counters);
      }
    }
  } else {
    setup_counter_groups(&cpu_info->systemwide_counters);
  }
  // software counters are only system-wide
  if (counter_mask & COUNTER_SOFTWARE){
    if ((cgroup = software_counter_group("software"))){
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
    }
    if (sflag) print_system(PRINT_CSV_HEADER);
    if (!interval && xflag) print_usage(NULL,PRINT_CSV_HEADER);
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV_HEADER);
    fprintf(outfile,"\n");
  }

  // let the child start after two seconds
  sleep(2);

  if (sflag) read_system();

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

  read_system();

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
    }
    if (sflag) print_system(PRINT_CSV);
    if (xflag && !interval) print_usage(&rusage,PRINT_CSV);
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV);    
    fprintf(outfile,"\n");    
  } else {
    if (sflag) print_system(PRINT_NORMAL);
    if (xflag) print_usage(&rusage,PRINT_NORMAL);
    print_metrics(cpu_info->systemwide_counters,PRINT_NORMAL);
  }

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

#if AMDGPU
  gpu_info_finalize();
#endif
  
  return 0;
}
