/*
 * wspy.c - workload spy - main program
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <getopt.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/wait.h>
#include "wspy.h"
#if AMDGPU
#include "amd_smi.h"
#include "amd_sysfs.h"
#endif
#include "error.h"
#include "manifest.h"
#include "run_index.h"
#include "coverage.h"
#include "provenance.h"

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
int versionflag = 0;
int capabilitiesflag = 0;
#if AMDGPU
int gpu_smi_requested = 0; /* legacy */
int gpu_busy_requested = 0;
int gpu_metrics_requested = 0;
#endif

char *outfile_path = NULL;
char *tree_output_path = NULL;
char *manifest_path = NULL;
char *run_index_path = NULL;

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
    { "capabilities", no_argument, 0, 54 },
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
    /* GPU options always recognized so we can warn if not built with AMDGPU */
    { "gpu-smi", no_argument, 0, 48 },
    { "gpu-busy", no_argument, 0, 49 },
    { "gpu-metrics", no_argument, 0, 50 },
    { "icache", no_argument, 0, 12 },
    { "no-icache", no_argument, 0, 13 },
    { "interval", required_argument, 0, 34 },
    { "ipc", no_argument, 0, 14 },
    { "no-ipc", no_argument, 0, 15 },
    { "manifest", required_argument, 0, 52 },
    { "memory", no_argument, 0, 16 },
    { "no-memory", no_argument, 0, 17 },
    { "opcache", no_argument, 0, 18 }, 
    { "no-opcache", no_argument, 0, 19 },
    { "per-core", no_argument, 0, 20 },
    { "run-index", required_argument, 0, 53 },
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
    { "version", no_argument, 0, 51 },
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
	outfile_path = optarg;
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
    case 48: // --gpu-smi
 #if AMDGPU
      gpu_smi_requested = 1;
      system_mask |= SYSTEM_GPU;
 #else
      warning("GPU support not built (rebuild with AMDGPU=1): --gpu-smi ignored\n");
 #endif
      break;
        case 49: // --gpu-busy
    #if AMDGPU
      gpu_busy_requested = 1;
      system_mask |= SYSTEM_GPU; /* ensure GPU init */
    #else
      warning("GPU support not built (rebuild with AMDGPU=1): --gpu-busy ignored\n");
    #endif
      break;
        case 50: // --gpu-metrics
    #if AMDGPU
      gpu_metrics_requested = 1;
      system_mask |= SYSTEM_GPU;
    #else
      warning("GPU support not built (rebuild with AMDGPU=1): --gpu-metrics ignored\n");
    #endif
      break;
            case 51: // --version
          versionflag = 1;
          break;
    case 52: // --manifest
      manifest_path = optarg;
      break;
    case 53: // --run-index
      run_index_path = optarg;
      break;
    case 54: // --capabilities
      capabilitiesflag = 1;
      break;
    case 31: // --tree
      if ((treefile = fopen(optarg,"w")) == NULL){
	warning("unable to open tree file: %s, ignored\n",optarg);
      } else {
	treeflag = 1;
	tree_output_path = optarg;
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
  if (versionflag){
    return 2;
  }
  if (capabilitiesflag){
    return 3; // no workload command needed for a capability probe
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

// Standalone counter capability discovery (wspy --capabilities): probes
// every counter type wspy knows how to request against this host/kernel
// and reports available vs unavailable, without launching a workload.
// Counter-selecting flags given alongside --capabilities are ignored --
// probing "everything" is the point of a discovery run.
static int run_capabilities_probe(void){
  struct counter_group *cgroup;

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }
  check_nmi_watchdog();
  setup_raw_events();

  counter_mask = COUNTER_ALL;
  coverage_reset();
  setup_counter_groups(&cpu_info->systemwide_counters);
  if (counter_mask & COUNTER_SOFTWARE){
    if ((cgroup = software_counter_group("software"))){
      cgroup->next = cpu_info->systemwide_counters;
      cpu_info->systemwide_counters = cgroup;
    }
  }
  setup_counters(cpu_info->systemwide_counters);

  print_capability_report();
  if (oflag) fclose(outfile);
  return 0;
}

#ifndef TEST_WSPY
int main(int argc,char *const argv[],char *const envp[]){
#else
static int original_main(int argc,char *const argv[],char *const envp[]){
#endif
  int i;
  int status;
  struct rusage rusage;
  struct counter_group *cgroup;
  outfile = stdout;
  num_procs = get_nprocs();
  clocks_per_second = sysconf(_SC_CLK_TCK);
  
  i = parse_options(argc,argv);
  if (i == 2){
    fprintf(stdout,"wspy %d.%d\n",WSPY_VERSION_MAJOR,WSPY_VERSION_MINOR);
    return 0;
  }
  if (i == 3){
    return run_capabilities_probe();
  }
  if (i){
      fatal("usage: %s -[abcistv][-o <file>] <cmd><args>...\n"
	    "\t--version                 - show version and exit\n"
	    "\t--capabilities            - probe available counters for this host/kernel and exit\n"
	    "\t--per-core or -a          - metrics per core\n"
	    "\t--rusage or -r            - show getrusage(2) information\n"
	    "\t--tree <file>             - create CSV of processes\n"
	    "\t--tree-cmdline            - record full command lines\n"
	    "\t--tree-vmsize             - virtual memory size\n"
	    "\t-o <file>                 - send output to file\n"
	    "\t--csv                     - create csv output\n"
	    "\t--manifest <file>         - write a JSON run manifest to <file>\n"
	    "\t--run-index <file>        - append a JSON run-index record to <file>\n"
	    "\t--interval <sec>          - read every <sec> seconds\n"
	    "\t--verbose or -v           - print verbose information\n"
	    "\t--system                  - system-wide metrics (load, cpu, gpu, network)\n"
	    "\n"
	    "\t--software or -s          - software counters\n"
	    "\t--ipc or i                - IPC counters\n"
	    "\t--branch or -b            - branch counters\n"
	    "\t--dcache                  - L1 dcache counters\n"
	    "\t--icache                  - L1 icache counters\n"
	    "\t--cache1                  - L1 cache counters\n"
	    "\t--cache2 or -c            - L2 cache counters\n"
	    "\t--cache3                  - L3 cache counters\n"
	    "\t--memory                  - memory counters\n"
	    "\t--opcache                 - opcache counters\n"
	    "\t--tlb                     - TLB counters\n"
	    "\t--float                   - floating point counters\n"
	    "\t--topdown or -t           - topdown counters, level 1\n"
	    "\t--topdown2                - topdown counters, level 2\n"
	    "\t--topdown-frontend        - topdown related to fe\n"
	    "\t--topdown-backend         - topdown related to be\n"
	    "\t--topdown-optlb           - topdown related to opcache, dtlb\n"
#if AMDGPU
	    "\t--gpu-smi                 - get gpu information from smi interface\n"
      "\t--gpu-busy                - read instantaneous GPU busy percent (sysfs)\n"
      "\t--gpu-metrics             - read detailed GPU metrics (sysfs)\n"
#endif
	    ,argv[0]);
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  check_nmi_watchdog();

#if AMDGPU
  if (system_mask & SYSTEM_GPU){
    /* Only invoke SMI APIs when explicitly requested */
    if (gpu_smi_requested){
      amd_smi_initialize();
      amd_smi_metrics();
      amd_smi_memory();
    }
    /* Initialize sysfs GPU interfaces if requested */
    if (gpu_busy_requested || gpu_metrics_requested) {
      amd_sysfs_initialize();
      if (gpu_busy_requested) {
        int busy = amd_sysfs_gpu_busy_percent();
        debug("initial gpu busy percent: %d\n", busy);
      }
      if (gpu_metrics_requested) {
        amd_sysfs_gpu_metrics();
      }
    }
  }
#endif

  // parse the event tables
  setup_raw_events();

  coverage_reset();

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
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      /* standalone gpu busy column after rusage */
      fprintf(outfile,"gpu_busy,");
    }
    if (!sflag && gpu_metrics_requested){
      fprintf(outfile,"gpu_temp,gpu_activity,gpu_power,gpu_freq,");
    }
    if (!sflag && gpu_smi_requested){
      fprintf(outfile,"gpu_smi_temp,gpu_smi_activity,gpu_smi_vram_used,gpu_smi_vram_total,");
    }
#endif
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV_HEADER);
    print_counter_coverage(PRINT_CSV_HEADER);
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
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      int busy = amd_sysfs_gpu_busy_percent();
      fprintf(outfile,"%d,",busy);
    }
    if (!sflag && gpu_metrics_requested){
      amd_sysfs_gpu_metrics();
      if (amd_sysfs_gpu_metrics_valid()){
        fprintf(outfile,"%d,%u,%.2f,%u,",
          amd_sysfs_get_gpu_temp(),
          amd_sysfs_get_gpu_activity(),
          amd_sysfs_get_gpu_power(),
          amd_sysfs_get_gpu_freq());
      } else {
        fprintf(outfile,"0,0,0.00,0,");
      }
    }
    if (!sflag && gpu_smi_requested){
      amd_smi_metrics();
      amd_smi_memory();
      fprintf(outfile,"%u,%u,%u,%u,",
        amd_smi_metrics_valid() ? amd_smi_get_temp() : 0,
        amd_smi_metrics_valid() ? amd_smi_get_activity() : 0,
        amd_smi_memory_valid() ? amd_smi_get_vram_used() : 0,
        amd_smi_memory_valid() ? amd_smi_get_vram_total() : 0);
    }
#endif
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV);
    print_counter_coverage(PRINT_CSV);
    fprintf(outfile,"\n");
  } else {
    if (sflag) print_system(PRINT_NORMAL);
    if (xflag) print_usage(&rusage,PRINT_NORMAL);
    print_metrics(cpu_info->systemwide_counters,PRINT_NORMAL);
    print_counter_coverage(PRINT_NORMAL);
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      int busy_final = amd_sysfs_gpu_busy_percent();
      fprintf(outfile,"gpu busy             %d%%\n",busy_final);
    }
    if (!sflag && gpu_metrics_requested){
      amd_sysfs_gpu_metrics();
      if (amd_sysfs_gpu_metrics_valid()){
        fprintf(outfile,"gpu temp             %d C\n", amd_sysfs_get_gpu_temp());
        fprintf(outfile,"gpu activity         %u%%\n", amd_sysfs_get_gpu_activity());
        fprintf(outfile,"gpu power            %.2f W\n", amd_sysfs_get_gpu_power());
        fprintf(outfile,"gpu freq             %u MHz\n", amd_sysfs_get_gpu_freq());
      }
    }
    if (!sflag && gpu_smi_requested){
      amd_smi_metrics();
      amd_smi_memory();
      if (amd_smi_metrics_valid()){
        fprintf(outfile,"gpu smi temp         %u C\n", amd_smi_get_temp());
        fprintf(outfile,"gpu smi activity     %u%%\n", amd_smi_get_activity());
      }
      if (amd_smi_memory_valid()){
        fprintf(outfile,"gpu vram used        %u MB\n", amd_smi_get_vram_used());
        fprintf(outfile,"gpu vram total       %u MB\n", amd_smi_get_vram_total());
      }
    }
#endif
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

  if (manifest_path || run_index_path){
    struct manifest_info minfo;
    struct manifest_counter_gap *gaps = NULL;
    struct coverage_entry *ce;
    int ngaps = 0;
    memset(&minfo,0,sizeof(minfo));
    minfo.start_time = start_time;
    minfo.finish_time = finish_time;
    minfo.argc = command_line_argc;
    minfo.argv = command_line_argv;
    /* --tree reaps children via its own ptrace wait loop, so the root
     * child's exit status isn't available here the way it is via wait4(). */
    minfo.exit_status.known = !treeflag;
    if (minfo.exit_status.known){
      minfo.exit_status.exited = WIFEXITED(status) ? 1 : 0;
      if (minfo.exit_status.exited) minfo.exit_status.exit_code = WEXITSTATUS(status);
      minfo.exit_status.signaled = WIFSIGNALED(status) ? 1 : 0;
      if (minfo.exit_status.signaled) minfo.exit_status.term_signal = WTERMSIG(status);
    }
    minfo.counter_mask = counter_mask;
    minfo.aflag = aflag;
    minfo.sflag = sflag;
    minfo.csvflag = csvflag;
    minfo.treeflag = treeflag;
    minfo.interval = interval;
    minfo.output_path = oflag ? outfile_path : NULL;
    minfo.tree_output_path = treeflag ? tree_output_path : NULL;
    minfo.manifest_path = manifest_path;
    minfo.counters_requested = coverage_requested;
    minfo.counters_measured = coverage_measured;
    for (ce = coverage_entries; ce; ce = ce->next) if (!ce->available) ngaps++;
    if (ngaps){
      gaps = calloc(ngaps,sizeof(struct manifest_counter_gap));
      ngaps = 0;
      for (ce = coverage_entries; ce; ce = ce->next){
	if (!ce->available){
	  gaps[ngaps].group_label = ce->group_label;
	  gaps[ngaps].counter_label = ce->counter_label;
	  gaps[ngaps].open_errno = ce->open_errno;
	  ngaps++;
	}
      }
    }
    minfo.counters_unavailable_count = ngaps;
    minfo.counters_unavailable = gaps;
    provenance_collect(&minfo.provenance);
    if (manifest_path) write_manifest(manifest_path,&minfo);
    if (run_index_path) append_run_index(run_index_path,&minfo);
    free(gaps);
  }

#if AMDGPU
  if (gpu_smi_requested)
    amd_smi_finalize();
  if (gpu_busy_requested || gpu_metrics_requested)
    amd_sysfs_finalize();
#endif
  
  return 0;
}
