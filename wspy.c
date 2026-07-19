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
#if NVIDIA
#include "nvidia_nvml.h"
#endif
#include "error.h"
#include "manifest.h"
#include "run_index.h"
#include "coverage.h"
#include "provenance.h"
#include "ibs.h"
#include "power.h"
#include "preflight.h"
#include "phase.h"
#include "multipass.h"
#include "affinity.h"

int aflag = 0;
int oflag = 0;
int sflag = 0;
int vflag = 0;
int xflag = 1;
int csvflag = 0;
int interval = 0;
int phase_flag = 1;
int treeflag = 0;
int tree_cmdline = 0;
int tree_open = 0;
int tree_futex = 0;
int tree_io = 0;
int tree_io_wait = 0;
int tree_schedstat = 0;
int tree_connect = 0;
int tree_nanosleep = 0;
int tree_wait = 0;
int tree_poll = 0;
int tree_vmsize = 0;
int trace_syscall = 0;
int versionflag = 0;
int capabilitiesflag = 0;
int preflightflag = 0;
int listaffinityflag = 0;
/* Core/thread affinity control (INVESTIGATION.md's "Core/thread
 * affinity control" item, affinity.h): defaults to AFFINITY_ALL, i.e. "every
 * CPU currently visible to this process" -- today's implicit behavior with
 * no --affinity given at all, made an explicit (no-op) choice in the spec
 * vocabulary rather than "absence of a flag". main() resolves this against
 * the discovered topology/cpu_info's own available-CPU mask before
 * launch_child() reads it. */
struct affinity_spec requested_affinity = { .mode = AFFINITY_ALL };
int affinity_active = 0;
const char *affinity_requested_arg = NULL; /* raw --affinity=<spec> text, NULL if not given -- manifest/run-index provenance */
int exit_with_child_flag = 0;
int multipass_flag = 0;
unsigned int passes_requested_mask = 0;
int multiplex_flag = 0; // --multiplex: --passes opens all requested groups in one pass instead of bin-packing N
int gpu_busy_requested = 0;
#if AMDGPU
int gpu_smi_requested = 0; /* legacy */
int gpu_metrics_requested = 0;
int gpu_device_index = -1; /* -1 = auto-select (lowest-numbered sysfs card / SMI device 0) */
#endif
#if NVIDIA
int gpu_nvidia_requested = 0;
int gpu_nvidia_device_index = -1; /* -1 = auto-select (NVML device 0) */
#endif

char *outfile_path = NULL;
char *tree_output_path = NULL;
char *manifest_path = NULL;
char *run_index_path = NULL;

/* Structured configuration provenance (INVESTIGATION.md's "What shipped in
 * 4.1"): purely
 * metadata passed through from a front end (wspy-run's builtin profiles, the
 * web launcher) via --preset-name/--config-name/--config-option -- wspy
 * itself has no notion of presets/configurations and none of these affect
 * what the run actually does. See manifest.h's manifest_config_provenance
 * comment. */
const char *config_provenance_preset = NULL;
const char *config_provenance_configuration = NULL;
static struct manifest_config_option *config_provenance_options = NULL;
static int config_provenance_noptions = 0;
static int config_provenance_options_cap = 0;

FILE *treefile = NULL;
FILE *outfile = NULL;
unsigned int counter_mask = COUNTER_IPC;

int num_procs;
int clocks_per_second;
int command_line_argc;
char **command_line_argv;

static void bind_core_counter_groups(struct counter_group *list,int cpu,unsigned int pmu_type){
  struct counter_group *cgroup;
  int i;

  for (cgroup = list; cgroup; cgroup = cgroup->next){
    cgroup->target_cpu = cpu;
    for (i=0;i<cgroup->ncounters;i++){
      if (cgroup->type_id == PERF_TYPE_RAW && cgroup->cinfo[i].device_type == PERF_TYPE_RAW){
        cgroup->cinfo[i].device_type = pmu_type;
      }
    }
  }
}

// Parses one --config-option key=value argument (structured configuration
// provenance, see the config_provenance_* globals above), appending it to
// config_provenance_options. A malformed argument (no '=', or an empty key)
// is warned about and skipped rather than fatal -- this is optional metadata,
// not something that should abort an otherwise-fine run, matching e.g.
// --ibs-maxcnt's own sscanf-failure warning.
static void add_config_provenance_option(const char *arg){
  const char *eq = strchr(arg,'=');

  if (!eq || eq == arg){
    warning("invalid argument to --config-option (expected key=value): %s, ignored\n",arg);
    return;
  }
  if (config_provenance_noptions == config_provenance_options_cap){
    config_provenance_options_cap = config_provenance_options_cap ? config_provenance_options_cap * 2 : 4;
    config_provenance_options = realloc(config_provenance_options,
      config_provenance_options_cap * sizeof(*config_provenance_options));
  }
  config_provenance_options[config_provenance_noptions].name = strndup(arg,eq-arg);
  config_provenance_options[config_provenance_noptions].value = strdup(eq+1);
  config_provenance_noptions++;
}

int parse_options(int argc,char *const argv[]){
  FILE *fp;
  int opt;
  int i;
  int value;
  unsigned int lev;
  static struct option long_options[] = {
    { "affinity", required_argument, 0, 71 },
    { "branch", no_argument, 0, 4 },
    { "no-branch", no_argument, 0, 5 },
    { "capabilities", no_argument, 0, 54 },
    { "config-name", required_argument, 0, 69 },
    { "config-option", required_argument, 0, 70 },
    { "csv", no_argument, 0, 3 },
    { "cache1", no_argument, 0, 39 },
    { "no-cache1", no_argument, 0, 40 },
    { "cache2", no_argument, 0, 6 }, 
    { "no-cache2", no_argument, 0, 7 },
    { "cache3", no_argument, 0, 8 }, 
    { "no-cache3", no_argument, 0, 9 },
    { "dcache", no_argument, 0, 10 },
    { "no-cache", no_argument, 0, 11 },
    { "exit-with-child", no_argument, 0, 55 },
    { "float",no_argument,0,33 },
    /* GPU options always recognized so we can warn if not built with AMDGPU/NVIDIA */
    { "gpu-smi", no_argument, 0, 48 },
    { "gpu-busy", no_argument, 0, 49 },
    { "gpu-metrics", no_argument, 0, 50 },
    { "gpu-device", required_argument, 0, 64 },
    { "gpu-nvidia", no_argument, 0, 89 },
    { "gpu-nvidia-device", required_argument, 0, 90 },
    { "ibs-basic", no_argument, 0, 56 },
    { "ibs-memory-deep", no_argument, 0, 57 },
    { "ibs-maxcnt", required_argument, 0, 58 },
    { "ibs-ldlat", required_argument, 0, 59 },
    { "ibs-fetchlat", required_argument, 0, 60 },
    { "icache", no_argument, 0, 12 },
    { "no-icache", no_argument, 0, 13 },
    { "interval", required_argument, 0, 34 },
    { "ipc", no_argument, 0, 14 },
    { "no-ipc", no_argument, 0, 15 },
    { "list-affinity", no_argument, 0, 72 },
    { "manifest", required_argument, 0, 52 },
    { "memory", no_argument, 0, 16 },
    { "no-memory", no_argument, 0, 17 },
    { "multiplex", no_argument, 0, 66 },
    { "no-multiplex", no_argument, 0, 67 },
    { "opcache", no_argument, 0, 18 },
    { "no-opcache", no_argument, 0, 19 },
    { "passes", required_argument, 0, 65 },
    { "per-core", no_argument, 0, 20 },
    { "phase-detect", no_argument, 0, 62 },
    { "no-phase-detect", no_argument, 0, 63 },
    { "power", no_argument, 0, 87 },
    { "no-power", no_argument, 0, 88 },
    { "preflight", no_argument, 0, 61 },
    { "preset-name", required_argument, 0, 68 },
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
    { "tree-futex",no_argument,0, 79 },
    { "tree-io",no_argument,0, 80 },
    { "tree-io-wait",no_argument,0, 81 },
    { "tree-schedstat",no_argument,0, 82 },
    { "tree-connect",no_argument,0, 83 },
    { "tree-nanosleep",no_argument,0, 84 },
    { "tree-wait",no_argument,0, 85 },
    { "tree-poll",no_argument,0, 86 },
    { "tree-open",no_argument,0, 38 },
    { "tree-vmsize",no_argument,0,41 },
    { "verbose", no_argument, 0, 32 },
    { "arm-dcache-mem", no_argument, 0, 73 },
    { "no-arm-dcache-mem", no_argument, 0, 74 },
    { "arm-icache-tlb", no_argument, 0, 75 },
    { "no-arm-icache-tlb", no_argument, 0, 76 },
    { "arm-mem-align-tlb", no_argument, 0, 77 },
    { "no-arm-mem-align-tlb", no_argument, 0, 78 },
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
    case 73: // --arm-dcache-mem
      counter_mask |= COUNTER_ARM_DCACHE_MEM;
      break;
    case 74: // --no-arm-dcache-mem
      counter_mask &= (~COUNTER_ARM_DCACHE_MEM);
      break;
    case 75: // --arm-icache-tlb
      counter_mask |= COUNTER_ARM_ICACHE_TLB;
      break;
    case 76: // --no-arm-icache-tlb
      counter_mask &= (~COUNTER_ARM_ICACHE_TLB);
      break;
    case 77: // --arm-mem-align-tlb
      counter_mask |= COUNTER_ARM_MEM_ALIGN_TLB;
      break;
    case 78: // --no-arm-mem-align-tlb
      counter_mask &= (~COUNTER_ARM_MEM_ALIGN_TLB);
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
          gpu_busy_requested = 1;
          system_mask |= SYSTEM_GPU; /* ensure GPU init */
#if !AMDGPU
          warning("GPU support not built (rebuild with AMDGPU=1): using iGPU fallback if available\n");
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
    case 55: // --exit-with-child
      exit_with_child_flag = 1;
      break;
    case 56: // --ibs-basic
      counter_mask |= COUNTER_IBS;
      ibs_collection_profile = IBS_PROFILE_BASIC;
      break;
    case 57: // --ibs-memory-deep
      counter_mask |= COUNTER_IBS;
      ibs_collection_profile = IBS_PROFILE_MEMORY_DEEP;
      break;
    case 58: // --ibs-maxcnt
      if ((sscanf(optarg,"%d",&value) == 1) && value > 0){
	ibs_params.maxcnt = (unsigned int)value;
      } else {
	warning("invalid argument to --ibs-maxcnt: %s, ignored\n",optarg);
      }
      break;
    case 59: // --ibs-ldlat
      if ((sscanf(optarg,"%d",&value) == 1) && value > 0){
	ibs_params.ldlat_threshold = (unsigned int)value;
      } else {
	warning("invalid argument to --ibs-ldlat: %s, ignored\n",optarg);
      }
      break;
    case 60: // --ibs-fetchlat
      if ((sscanf(optarg,"%d",&value) == 1) && value > 0){
	ibs_params.fetchlat_threshold = (unsigned int)value;
      } else {
	warning("invalid argument to --ibs-fetchlat: %s, ignored\n",optarg);
      }
      break;
    case 61: // --preflight
      preflightflag = 1;
      break;
    case 62: // --phase-detect
      phase_flag = 1;
      break;
    case 63: // --no-phase-detect
      phase_flag = 0;
      break;
    case 87: // --power
      counter_mask |= COUNTER_POWER;
      break;
    case 88: // --no-power
      counter_mask &= (~COUNTER_POWER);
      break;
    case 64: // --gpu-device
#if AMDGPU
      if ((sscanf(optarg,"%d",&value) == 1) && value >= 0){
	gpu_device_index = value;
      } else {
	warning("invalid argument to --gpu-device: %s, ignored\n",optarg);
      }
#else
      warning("GPU support not built (rebuild with AMDGPU=1): --gpu-device ignored\n");
#endif
      break;
    case 89: // --gpu-nvidia
#if NVIDIA
      gpu_nvidia_requested = 1;
      system_mask |= SYSTEM_GPU;
#else
      warning("GPU support not built (rebuild with NVIDIA=1): --gpu-nvidia ignored\n");
#endif
      break;
    case 90: // --gpu-nvidia-device
#if NVIDIA
      if ((sscanf(optarg,"%d",&value) == 1) && value >= 0){
	gpu_nvidia_device_index = value;
      } else {
	warning("invalid argument to --gpu-nvidia-device: %s, ignored\n",optarg);
      }
#else
      warning("GPU support not built (rebuild with NVIDIA=1): --gpu-nvidia-device ignored\n");
#endif
      break;
    case 65: { // --passes
      char *copy = strdup(optarg);
      char *tok,*saveptr;
      unsigned int bit;

      multipass_flag = 1;
      passes_requested_mask = 0;
      for (tok = strtok_r(copy,",",&saveptr); tok; tok = strtok_r(NULL,",",&saveptr)){
	if (!multipass_lookup_group_name(tok,&bit)){
	  fatal("--passes: unrecognized counter group name '%s' (see --help for the valid list)\n",tok);
	}
	passes_requested_mask |= bit;
      }
      free(copy);
      if (passes_requested_mask == 0){
	fatal("--passes: at least one counter group name is required\n");
      }
      break;
    }
    case 66: // --multiplex
      multiplex_flag = 1;
      break;
    case 67: // --no-multiplex
      multiplex_flag = 0;
      break;
    case 68: // --preset-name
      config_provenance_preset = optarg;
      break;
    case 69: // --config-name
      config_provenance_configuration = optarg;
      break;
    case 70: // --config-option
      add_config_provenance_option(optarg);
      break;
    case 71: { // --affinity
      struct affinity_spec parsed;
      if (affinity_parse_spec(optarg,&parsed) != 0){
	warning("invalid argument to --affinity: %s, ignored (see --help)\n",optarg);
      } else {
	requested_affinity = parsed;
	affinity_requested_arg = optarg;
      }
      break;
    }
    case 72: // --list-affinity
      listaffinityflag = 1;
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
    case 79:
      tree_futex = 1;
      trace_syscall = 1;
      break;
    case 80: // --tree-io
      tree_io = 1;
      break;
    case 81: // --tree-io-wait
      tree_io_wait = 1;
      trace_syscall = 1;
      break;
    case 82: // --tree-schedstat
      tree_schedstat = 1;
      break;
    case 83: // --tree-connect
      tree_connect = 1;
      trace_syscall = 1;
      break;
    case 84: // --tree-nanosleep
      tree_nanosleep = 1;
      trace_syscall = 1;
      break;
    case 85: // --tree-wait
      tree_wait = 1;
      trace_syscall = 1;
      break;
    case 86: // --tree-poll
      tree_poll = 1;
      trace_syscall = 1;
      break;
    case 41: // --tree-vmsize
      // Originally registered as a long option (and documented in --help)
      // with no case here at all -- any use fell through to
      // `default: return 1;` below (a fatal "usage: ..." error) -- then
      // later given this case to stop that crash, but tree_vmsize still
      // wasn't consumed anywhere (topdown.c/proctree.c), so the flag was a
      // pure no-op for a long stretch of this codebase's history. Now
      // actually wired up: topdown.c reads a passive /proc/<pid>/status
      // scrape (VmHWM/RssAnon/RssFile/RssShmem/VmSwap) gated on this flag,
      // same shape as --tree-io/--tree-schedstat. See
      // doc/INVESTIGATION_ARCHIVE.md's "Concrete design: memory footprint
      // detail via /proc/<pid>/status".
      tree_vmsize = 1;
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
  if (preflightflag){
    return 4; // no workload command needed, and no privileges either -- pure arithmetic
  }
  if (listaffinityflag){
    return 5; // no workload command needed, and no privileges either -- pure sysfs discovery
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
  struct ibs_capabilities ibs;
  struct power_capabilities power;

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }
  check_nmi_watchdog();

  // counter_mask must be its final value (COUNTER_ALL here) *before*
  // setup_raw_events() runs -- that function only parses a raw_event
  // table entry's .config when events[i].use intersects counter_mask, so
  // calling it against whatever counter_mask the CLI flags happened to
  // leave behind (rather than the COUNTER_ALL this probe actually wants)
  // silently leaves most raw events unparsed. Same bug class as the
  // --passes fix in main() below -- see that comment for the full story.
  counter_mask = COUNTER_ALL;
  setup_raw_events();

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
  print_cpu_pmu_report(outfile);

  // Core/thread affinity discovery (SMT sibling groups + L3-sharing
  // domains, affinity.h): folded into --capabilities' combined report so a
  // caller doesn't need a separate command to see which --affinity=
  // domain=<id>/thread=<id> values are meaningful on this host; also
  // available on its own, without probing any counters, via --list-affinity.
  affinity_topology_discover();
  affinity_print_report(outfile);

  ibs = ibs_probe();
  print_ibs_capability_report(&ibs);

  power = power_probe();
  print_power_capability_report(&power);

#if AMDGPU
  // GPU device enumeration -- lists every AMD card/device this host can
  // see, independent of whether --gpu-busy/--gpu-metrics/--gpu-smi were
  // given, so --gpu-device=<idx> can be chosen from the printed indices.
  amd_sysfs_initialize(-1);
  amd_sysfs_print_capability_report(outfile);
  amd_sysfs_finalize();
  amd_smi_initialize(-1);
  amd_smi_print_capability_report(outfile);
  amd_smi_finalize();
#endif

#if NVIDIA
  // NVIDIA GPU device enumeration -- lists every NVML device this host can
  // see, independent of whether --gpu-nvidia was given, so
  // --gpu-nvidia-device=<idx> can be chosen from the printed indices.
  nvidia_nvml_initialize(-1);
  nvidia_nvml_print_capability_report(outfile);
  nvidia_nvml_finalize();
#endif

  if (oflag) fclose(outfile);
  return 0;
}

// Standalone counter-fit preflight (wspy --preflight [<counter flags>]):
// evaluates whatever counter_mask the given flags produced (default just
// COUNTER_IPC, same as a normal run -- unlike --capabilities, this doesn't
// force COUNTER_ALL, since the whole point is to check the combination the
// caller actually intends to run) against the available general-purpose
// hardware PMU counter slots, without launching a workload or opening any
// perf events -- so unlike --capabilities, this needs no root/perf access.
static int run_preflight_probe(void){
  struct preflight_result pf;

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }
  check_nmi_watchdog();
  setup_raw_events();

  pf = preflight_evaluate(counter_mask);
  print_preflight_report(&pf);
  preflight_result_free(&pf);

  if (oflag) fclose(outfile);
  return 0;
}

// Standalone core/thread affinity topology discovery (wspy --list-affinity):
// prints every logical CPU's core_id/package_id/SMT-primary-thread flag and
// L3-sharing domain (the ids --affinity=domain=<id> refers to) plus, on ARM,
// its MIDR-derived core type (the ids --affinity=coretype=<id> refers to --
// e.g. distinguishing a big.LITTLE part's "big"/"little" clusters even when
// they share one combined L3, which domain=<id> alone can't tell apart),
// needing no privileges or perf access -- pure sysfs reads, same "no root
// needed" standing as --preflight. Also folded into --capabilities' own
// combined report (run_capabilities_probe() above) so a caller probing
// everything at once sees this too without a second command.
static int run_affinity_report_probe(void){
  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }
  affinity_topology_discover();
  affinity_print_report(outfile);

  if (oflag) fclose(outfile);
  return 0;
}

// Populates every struct manifest_info field common to a single-pass run
// (main()'s own tail) and a --passes run (run_multipass()'s tail) -- every
// field except counter_mask (differs: the single mask vs. the union of all
// passes) and npasses/passes (only set for --passes). Both callers read
// this from the same globals at the point each considers "the run"
// finished -- run_multipass() restores those globals to its primary pass's
// values before calling this, so from here on both paths are identical.
static void populate_manifest_common(struct manifest_info *minfo){
  struct manifest_counter_gap *gaps = NULL;
  struct coverage_entry *ce;
  int ngaps = 0;

  minfo->collector = "wspy";
  minfo->start_time = start_time;
  minfo->finish_time = finish_time;
  minfo->argc = command_line_argc;
  minfo->argv = command_line_argv;
  /* child_exit_known etc. are populated either above (non-tree, wait4()) or
   * by ptrace_loop() itself (--tree mode) -- or, for a --passes run, by
   * run_multipass() restoring them to its primary pass's values. */
  minfo->exit_status.known = child_exit_known;
  minfo->exit_status.exited = child_exited;
  minfo->exit_status.exit_code = child_exit_code;
  minfo->exit_status.signaled = child_signaled;
  minfo->exit_status.term_signal = child_term_signal;
  minfo->aflag = aflag;
  minfo->sflag = sflag;
  minfo->csvflag = csvflag;
  minfo->treeflag = treeflag;
  minfo->interval = interval;
  minfo->output_path = oflag ? outfile_path : NULL;
  minfo->tree_output_path = treeflag ? tree_output_path : NULL;
  minfo->manifest_path = manifest_path;
  minfo->counters_requested = coverage_requested;
  minfo->counters_measured = coverage_measured;
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
  minfo->counters_unavailable_count = ngaps;
  minfo->counters_unavailable = gaps;
  // Core/thread affinity control (affinity.h): the resolved placement is
  // part of a run's provenance, not just implicit in how it was launched --
  // static, since populate_manifest_common()'s two callers (main()'s
  // single-pass tail, run_multipass()'s tail) each write_manifest()/
  // append_run_index() synchronously right after, before this function
  // could be called again.
  {
    static char affinity_cpus_buf[512];
    affinity_format_cpu_set(&requested_affinity.set,cpu_info->num_cores,affinity_cpus_buf,sizeof(affinity_cpus_buf));
    minfo->affinity_mode = affinity_mode_name(requested_affinity.mode);
    minfo->affinity_requested = affinity_requested_arg;
    minfo->affinity_cpus = affinity_cpus_buf;
  }
  provenance_collect(&minfo->provenance);
  minfo->config_provenance.preset = config_provenance_preset;
  minfo->config_provenance.configuration = config_provenance_configuration;
  minfo->config_provenance.noptions = config_provenance_noptions;
  minfo->config_provenance.options = config_provenance_options;
}

// --exit-with-child's return-code logic, shared between main()'s single-pass
// tail and run_multipass()'s tail -- both read child_exit_known/etc. from
// their own "primary" execution (see populate_manifest_common()).
static int exit_code_epilogue(void){
  if (exit_with_child_flag){
    if (!child_exit_known){
      warning("--exit-with-child: child exit status not observed, exiting 0\n");
      return 0;
    }
    if (child_signaled){
      // conventional shell exit-code encoding for death-by-signal
      return 128 + child_term_signal;
    }
    return child_exit_code;
  }
  return 0;
}

// Native multi-pass counter execution (wspy --passes=<list>): re-launches
// the workload once per pass in a multipass_plan_build() plan, each pass
// requesting only the counter groups that fit the available hardware PMU
// budget, and merges the result into one CSV/manifest/run-index record
// instead of requiring N separate wspy invocations (wspy-run's builtin
// profiles automate that externally today by shelling out N times; this
// moves the same "run N times, once per counter-group subset that fits"
// strategy inside wspy itself). Called from main() once counter_mask/aflag/
// etc. are final and cpu_info/raw events are already set up.
//
// --multiplex swaps the plan builder for multipass_plan_build_multiplexed(),
// which always produces exactly one pass covering every requested group and
// leans on the kernel's own PMU multiplexing plus read_counters()'s
// time_running/time_enabled scaling (INVESTIGATION.md's "What shipped in
// 4.1", the multiplex-scaling correctness fix) to
// keep the values correct despite oversubscription -- one workload execution
// instead of N, at the cost of lower per-counter confidence. Not the
// default: bin-packing into fully-scheduled passes remains the more precise
// choice when re-executing the workload N times is affordable.
//
// V1 scope is aggregate-only: --interval/--per-core/--tree/IBS/GPU are all
// fatal'd against --passes in main() before this is ever reached, so this
// function doesn't need any of their logic (phase detection, per-core rows,
// ptrace, GPU columns).
//
// Each pass is a genuinely separate re-execution of the workload (its own
// fork/exec, its own start/finish time, its own rusage/exit status) -- there
// is no single canonical "elapsed"/rusage/exit status across N runs, so
// pass 0 is treated as "primary": its rusage/start_time/finish_time/exit
// status become the merged row's base columns and the manifest's top-level
// exit_status, exactly as if it were the only pass. Every pass's own timing/
// exit status/coverage delta is still recorded in the manifest's "passes"
// array for full audit; a pass whose exit status differs from pass 0's
// produces a warning (not fatal -- real non-determinism across
// re-executions is a legitimate signal worth surfacing, not a reason to
// discard an otherwise useful run).
static int run_multipass(char *const envp[]){
  struct multipass_plan plan;
  struct counter_group **pass_lists;
  struct manifest_pass_info *mpasses;
  struct rusage rusage,primary_rusage;
  struct timespec primary_start,primary_finish;
  struct manifest_exit_status primary_exit;
  int p,status;

  plan = multiplex_flag ? multipass_plan_build_multiplexed(passes_requested_mask)
                         : multipass_plan_build(passes_requested_mask);
  pass_lists = calloc(plan.npasses,sizeof(struct counter_group *));
  mpasses = calloc(plan.npasses,sizeof(struct manifest_pass_info));

  // Phase A: build every pass's counter_group list up front (cheap, no
  // privileges) -- same save/swap/restore counter_mask idiom
  // preflight_evaluate() already uses to build a throwaway list for an
  // arbitrary mask without disturbing the real one.
  for (p = 0; p < plan.npasses; p++){
    unsigned int saved_mask = counter_mask;
    struct counter_group *cgroup;

    counter_mask = plan.pass_mask[p];
    setup_counter_groups(&pass_lists[p]);
    counter_mask = saved_mask;
    // setup_counter_groups() never handles COUNTER_SOFTWARE itself (see
    // main()'s single-pass path) -- replicate that per pass exactly.
    if (plan.pass_mask[p] & COUNTER_SOFTWARE){
      if ((cgroup = software_counter_group("software"))){
	cgroup->next = pass_lists[p];
	pass_lists[p] = cgroup;
      }
    }
  }

  signal(SIGINT,SIG_IGN);

  // Phase B: CSV header -- base columns once, then each pass's group
  // columns in sequence. print_metrics() dispatches purely off
  // cgroup->mask, so calling it once per pass just concatenates that
  // pass's columns; no two passes can emit the same column name since the
  // bin-packer never duplicates a bit across passes.
  if (csvflag){
    if (sflag) print_system(PRINT_CSV_HEADER);
    if (xflag) print_usage(NULL,PRINT_CSV_HEADER);
    for (p = 0; p < plan.npasses; p++) print_metrics(pass_lists[p],PRINT_CSV_HEADER);
    print_counter_coverage(PRINT_CSV_HEADER);
    fprintf(outfile,"\n");
  }

  // Phase C: execute each pass in turn -- the only part that needs a live
  // child.
  for (p = 0; p < plan.npasses; p++){
    int req0 = coverage_requested,meas0 = coverage_measured;

    setup_counters(pass_lists[p]);
    start_counters(pass_lists[p]);

    // let the counters run for two seconds before the measurement window
    // starts, same as a single-pass run -- see the comment at the
    // equivalent sleep(2) in main().
    sleep(2);
    if (p == 0 && sflag) read_system();
    clock_gettime(CLOCK_REALTIME,&start_time);
    if (launch_child(command_line_argc,command_line_argv,envp)){
      fatal("unable to launch %s (pass %d/%d)\n",command_line_argv[0],p+1,plan.npasses);
    }
    write(child_pipe[1],"start\n",6);
    wait4(child_pid,&status,0,&rusage);
    close(child_pipe[1]);
    child_exit_known = 1;
    child_exited = WIFEXITED(status) ? 1 : 0;
    if (child_exited) child_exit_code = WEXITSTATUS(status);
    child_signaled = WIFSIGNALED(status) ? 1 : 0;
    if (child_signaled) child_term_signal = WTERMSIG(status);
    is_still_running = 0;
    clock_gettime(CLOCK_REALTIME,&finish_time);
    if (p == 0) read_system();

    read_counters(pass_lists[p],1);
    close_counters(pass_lists[p]); // MUST run before the next pass's setup_counters()

    mpasses[p].counter_mask = plan.pass_mask[p];
    mpasses[p].counters_requested = coverage_requested - req0;
    mpasses[p].counters_measured = coverage_measured - meas0;
    mpasses[p].start_time = start_time;
    mpasses[p].finish_time = finish_time;
    mpasses[p].exit_status.known = child_exit_known;
    mpasses[p].exit_status.exited = child_exited;
    mpasses[p].exit_status.exit_code = child_exit_code;
    mpasses[p].exit_status.signaled = child_signaled;
    mpasses[p].exit_status.term_signal = child_term_signal;

    if (p == 0){
      primary_rusage = rusage;
      primary_start = start_time;
      primary_finish = finish_time;
      primary_exit = mpasses[0].exit_status;
    } else if (memcmp(&mpasses[p].exit_status,&primary_exit,sizeof(primary_exit)) != 0){
      warning("--passes: pass %d exit status differs from pass 1 (mask 0x%x vs 0x%x) -- "
	      "workload may be non-deterministic across re-executions\n",
	      p+1,plan.pass_mask[p],plan.pass_mask[0]);
    }
  }

  // Restore the globals every downstream consumer reads (print_usage(),
  // populate_manifest_common(), exit_code_epilogue()) to the primary pass's
  // values, so they run completely unmodified as if "the run" were pass 0.
  start_time = primary_start;
  finish_time = primary_finish;
  rusage = primary_rusage;
  child_exit_known = primary_exit.known;
  child_exited = primary_exit.exited;
  child_exit_code = primary_exit.exit_code;
  child_signaled = primary_exit.signaled;
  child_term_signal = primary_exit.term_signal;

  // Phase D: CSV/human value row.
  if (csvflag){
    if (sflag) print_system(PRINT_CSV);
    if (xflag) print_usage(&rusage,PRINT_CSV);
    for (p = 0; p < plan.npasses; p++) print_metrics(pass_lists[p],PRINT_CSV);
    print_counter_coverage(PRINT_CSV);
    fprintf(outfile,"\n");
  } else {
    if (sflag) print_system(PRINT_NORMAL);
    if (xflag) print_usage(&rusage,PRINT_NORMAL);
    for (p = 0; p < plan.npasses; p++){
      fprintf(outfile,"##### pass %2d (mask 0x%x) #####################\n",p,plan.pass_mask[p]);
      print_metrics(pass_lists[p],PRINT_NORMAL);
    }
    print_counter_coverage(PRINT_NORMAL);
  }

  if (oflag) fclose(outfile);

  if (manifest_path || run_index_path){
    struct manifest_info minfo;
    memset(&minfo,0,sizeof(minfo));
    populate_manifest_common(&minfo);
    minfo.counter_mask = passes_requested_mask;
    minfo.npasses = plan.npasses;
    minfo.passes = mpasses;
    if (manifest_path) write_manifest(manifest_path,&minfo);
    if (run_index_path) append_run_index(run_index_path,&minfo);
    free((void *)minfo.counters_unavailable);
  }

  return exit_code_epilogue();
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
  enum wspy_phase final_phase = PHASE_WARMUP;
  int per_core_csv = 0;
  int first_core_with_counters = -1;
  outfile = stdout;
  num_procs = get_nprocs();
  clocks_per_second = sysconf(_SC_CLK_TCK);
  
  i = parse_options(argc,argv);
  if (i == 2){
    fprintf(stdout,"wspy %d.%d.%d\n",WSPY_VERSION_MAJOR,WSPY_VERSION_MINOR,WSPY_VERSION_PATCH);
    return 0;
  }
  if (i == 3){
    return run_capabilities_probe();
  }
  if (i == 4){
    return run_preflight_probe();
  }
  if (i == 5){
    return run_affinity_report_probe();
  }
  if (i){
      fatal("usage: %s -[abcistv][-o <file>] <cmd><args>...\n"
	    "\t--version                 - show version and exit\n"
	    "\t--capabilities            - probe available counters for this host/kernel and exit\n"
	    "\t--preflight               - check counter-fit for the given flags and exit (no root needed)\n"
	    "\t--list-affinity           - list core/thread/L3-domain/core-type topology and exit\n"
	    "\t                            (no root needed)\n"
	    "\t--affinity=<spec>         - pin the workload to selected CPUs: all (default),\n"
	    "\t                            thread=<id>, nosmt (one thread per core), domain=<id>\n"
	    "\t                            (one L3-sharing core-complex), coretype=<id> (one\n"
	    "\t                            microarchitecture/core type, e.g. only the \"big\" or\n"
	    "\t                            only the \"little\" cores on a big.LITTLE ARM part --\n"
	    "\t                            ARM only, see --list-affinity), or cpuset=<c0,c1,...>\n"
	    "\t                            (explicit list/ranges)\n"
	    "\t--per-core or -a          - metrics per core\n"
	    "\t--rusage or -r            - show getrusage(2) information\n"
	    "\t--tree <file>             - create CSV of processes\n"
	    "\t--tree-cmdline            - record full command lines\n"
	    "\t--tree-futex              - record per-pid/thread blocking futex-wait time\n"
	    "\t--tree-io                 - record per-pid /proc/<pid>/io byte counters\n"
	    "\t--tree-io-wait            - record per-pid/thread blocking I/O (read/write) wait time\n"
	    "\t--tree-schedstat          - record per-pid/thread run-queue delay + timeslice count\n"
	    "\t--tree-vmsize             - record peak RSS + anon/file/shmem RSS composition + swap\n"
	    "\t--tree-connect            - record per-pid/thread connect() setup latency\n"
	    "\t--tree-wait               - record per-pid/thread time blocked in wait4/waitid\n"
	    "\t--tree-poll               - record per-pid/thread time blocked in poll/select/epoll_wait\n"
	    "\t--tree-nanosleep          - record per-pid/thread deliberate nanosleep/clock_nanosleep time\n"
	    "\t-o <file>                 - send output to file\n"
	    "\t--csv                     - create csv output\n"
	    "\t--manifest <file>         - write a JSON run manifest to <file>\n"
	    "\t--run-index <file>        - append a JSON run-index record to <file>\n"
	    "\t--preset-name <name>      - record a launcher's named preset (e.g. wspy-run's own\n"
	    "\t                            profile name) in the manifest/run-index; metadata\n"
	    "\t                            only, doesn't affect what this run does\n"
	    "\t--config-name <name>      - record a launcher's configuration category (e.g.\n"
	    "\t                            \"performance-counters\") in the manifest/run-index;\n"
	    "\t                            metadata only\n"
	    "\t--config-option <k>=<v>   - record one launcher-vocabulary option (repeatable);\n"
	    "\t                            metadata only -- see INVESTIGATION.md's\n"
	    "\t                            \"What shipped in 4.1\", \"structured configuration\n"
	    "\t                            provenance\"\n"
	    "\t--exit-with-child         - exit with the launched command's exit status\n"
	    "\t--passes=<list>           - re-launch the workload once per automatically-sized\n"
	    "\t                            pass covering the comma-separated counter group\n"
	    "\t                            names in <list> (e.g. ipc,topdown,cache2,software),\n"
	    "\t                            merging the result into one CSV/manifest -- avoids\n"
	    "\t                            multiplexing when <list> exceeds the available\n"
	    "\t                            hardware PMU slots. Incompatible with --interval,\n"
	    "\t                            --per-core, --tree, --ibs-*, --gpu-*.\n"
	    "\t--multiplex               - with --passes, open every requested counter group\n"
	    "\t                            in a single pass instead of bin-packing N passes,\n"
	    "\t                            relying on the kernel to multiplex and on wspy's\n"
	    "\t                            own time_running/time_enabled scaling to keep the\n"
	    "\t                            reported values correct. Trades some precision\n"
	    "\t                            (more multiplexing = lower per-counter confidence)\n"
	    "\t                            for a single workload execution. Off by default.\n"
	    "\t--interval <sec>          - read every <sec> seconds\n"
	    "\t--no-phase-detect         - disable automatic phase (warmup/steady/degraded)\n"
	    "\t                            detection on --interval samples (on by default)\n"
	    "\t--verbose or -v           - print verbose information\n"
	    "\t--system                  - system-wide metrics (load, cpu, freq, gpu, network)\n"
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
	    "\t--ibs-basic               - AMD IBS: unfiltered ibs_fetch/ibs_op sample counts\n"
	    "\t--ibs-memory-deep         - AMD IBS: ibs_op with l3missonly+ldlat filtering (skews\n"
	    "\t                            sampling period -- see output annotations/docs)\n"
	    "\t--ibs-maxcnt <n>          - override IBS sampling interval (MaxCnt, via sample_period)\n"
	    "\t--ibs-ldlat <n>           - override ibs-memory-deep load-latency threshold (cycles)\n"
	    "\t--ibs-fetchlat <n>        - override ibs-memory-deep fetch-latency threshold (cycles)\n"
	    "\t--power                   - CPU package energy/power (power/energy-pkg dynamic PMU,\n"
	    "\t                            RAPL-equivalent): pkg_joules/pkg_watts, system-wide only\n"
#if AMDGPU
	    "\t--gpu-smi                 - get gpu information from smi interface\n"
      "\t--gpu-busy                - read instantaneous GPU busy percent (sysfs)\n"
      "\t--gpu-metrics             - read detailed GPU metrics (sysfs)\n"
      "\t--gpu-device=<idx>        - select AMD GPU device <idx> for the above (default:\n"
      "\t                            lowest-numbered card / SMI device 0); see the device\n"
      "\t                            list printed by --capabilities on multi-GPU hosts\n"
#endif
#if NVIDIA
      "\t--gpu-nvidia              - read GPU busy percent + VRAM used/total (NVML)\n"
      "\t--gpu-nvidia-device=<idx> - select NVIDIA GPU device <idx> for the above (default:\n"
      "\t                            device 0); see the device list printed by\n"
      "\t                            --capabilities on multi-GPU hosts\n"
#endif
	    ,argv[0]);
  }

  // --passes' merge semantics only cover the common aggregate case; each of
  // these has either no well-defined multi-pass merge (periodic ticks/cores
  // across separately-timed re-executions of the child) or its own separate
  // collection/budget model already (IBS, GPU). Fail loudly rather than
  // silently ignoring the incompatible flag or producing a misleading result.
  if (multipass_flag){
    if (interval)
      fatal("--passes is incompatible with --interval (no defined multi-pass merge semantics for periodic ticks)\n");
    if (aflag)
      fatal("--passes is incompatible with --per-core/-a (per-core x per-pass grid not supported)\n");
    if (treeflag)
      fatal("--passes is incompatible with --tree (ptrace child-tracing model conflicts with re-executing the child N times)\n");
    if (counter_mask & COUNTER_IBS)
      fatal("--passes is incompatible with --ibs-basic/--ibs-memory-deep (IBS has its own separate system-wide budget)\n");
    if (counter_mask & COUNTER_POWER)
      fatal("--passes is incompatible with --power (power/energy-pkg is a cumulative counter with"
            " its own separate system-wide budget, and scale-multiplication across separately-timed"
            " re-executions of the child isn't a defined merge)\n");
    if (gpu_busy_requested)
      fatal("--passes is incompatible with --gpu-busy\n");
#if AMDGPU
    if (gpu_smi_requested || gpu_metrics_requested)
      fatal("--passes is incompatible with --gpu-smi/--gpu-metrics\n");
#endif
#if NVIDIA
    if (gpu_nvidia_requested)
      fatal("--passes is incompatible with --gpu-nvidia\n");
#endif
  } else if (multiplex_flag){
    fatal("--multiplex only applies to --passes (it selects how --passes packs its requested counter groups)\n");
  }

  if (inventory_cpu() != 0){
    fatal("unable to query CPU information\n");
  }

  if (multipass_flag && cpu_info->vendor != VENDOR_AMD) {
    passes_requested_mask &= ~(COUNTER_TOPDOWN_FE | COUNTER_TOPDOWN_OP);
    if (passes_requested_mask == 0) {
      fatal("--passes: none of the requested counter groups are supported on this CPU vendor\n");
    }
  }

  check_nmi_watchdog();
  if (tree_schedstat) check_schedstat_enabled();

  // Core/thread affinity control (affinity.h): resolve the --affinity=
  // request (default "all", a no-op) against the discovered SMT/L3
  // topology and this process's own available-CPU mask, before
  // launch_child() (topdown.c) reads requested_affinity/affinity_active in
  // the forked child. A request that can't be satisfied at all (bad
  // domain/thread id, or no overlap with cpu_info's available-CPU mask) is
  // fatal here -- this is a real run, unlike --list-affinity's pure
  // discovery probe -- affinity_resolve() itself only warns (and narrows
  // the set) for a partial overlap.
  affinity_topology_discover();
  if (affinity_resolve(&requested_affinity) != 0){
    fatal("--affinity: unable to resolve requested CPU set (see above)\n");
  }
  affinity_active = (requested_affinity.mode != AFFINITY_ALL);

#if AMDGPU
  if (system_mask & SYSTEM_GPU){
    /* Only invoke SMI APIs when explicitly requested */
    if (gpu_smi_requested){
      amd_smi_initialize(gpu_device_index);
      amd_smi_metrics();
      amd_smi_memory();
    }
    /* Initialize sysfs GPU interfaces if requested */
    if (gpu_busy_requested || gpu_metrics_requested) {
      amd_sysfs_initialize(gpu_device_index);
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

#if NVIDIA
  if ((system_mask & SYSTEM_GPU) && gpu_nvidia_requested){
    nvidia_nvml_initialize(gpu_nvidia_device_index);
    nvidia_nvml_metrics();
  }
#endif

  // parse the event tables -- setup_raw_events() only parses (fills in
  // .raw.config from the "event=...,umask=..." description) a raw_event
  // table entry whose .use bits intersect counter_mask, since normally
  // that's already the CLI's final, complete set of requested groups. With
  // --passes, though, the requested groups live in passes_requested_mask
  // instead (parse_options() never touches counter_mask for --passes,
  // which stays at its COUNTER_IPC default) -- so without this, any raw
  // event needed only by a non-default pass group never gets parsed and
  // silently opens as a meaningless config=0 raw event later (real bug,
  // reproduced live: --passes=topdown-frontend reads 0 for every counter
  // except the one event that also happens to be COUNTER_IPC-tagged).
  if (multipass_flag) counter_mask |= passes_requested_mask;
  setup_raw_events();

  coverage_reset();

  // Native multi-pass counter execution (--passes=<list>): re-launches the
  // workload once per automatically-sized pass and merges the result into
  // one CSV/manifest -- see multipass.h and run_multipass() below. A single
  // early-return branch, so the rest of this single-pass main() is
  // untouched (aside from the incompatibility checks above).
  if (multipass_flag){
    return run_multipass(envp);
  }

  // Counter-fit preflight: estimate whether the requested counter groups
  // will fit in the available general-purpose hardware PMU slots without
  // multiplexing, before any perf_event_open() calls are made below --
  // silent when the fit is fine, a stderr warning with suggested
  // downgrades (and the nmi_watchdog free-a-slot tip) when it isn't. See
  // preflight.h; the same check is available standalone via --preflight.
  {
    struct preflight_result pf = preflight_evaluate(counter_mask);
    preflight_warn_if_tight(&pf);
    preflight_result_free(&pf);
  }

  // set up either system-wide or core-specific counters
  if (aflag){
    for (i=0;i<cpu_info->num_cores;i++){
      if (cpu_info->coreinfo[i].is_available &&
    ((cpu_info->coreinfo[i].vendor == CORE_AMD_ZEN)||
     (cpu_info->coreinfo[i].vendor == CORE_AMD_ZEN5)||
     (cpu_info->coreinfo[i].vendor == CORE_INTEL_CORE)||
     (is_arm_core_type(cpu_info->coreinfo[i].vendor)))){
	setup_counter_groups(&cpu_info->coreinfo[i].core_specific_counters);
  bind_core_counter_groups(cpu_info->coreinfo[i].core_specific_counters,
         i,cpu_info->coreinfo[i].pmu_type);
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

  // AMD IBS collection profiles are only system-wide -- IBS is a
  // machine-wide dynamic PMU, not a per-core countable event the way cache/
  // branch raw events are, so --per-core has no natural meaning here.
  if (counter_mask & COUNTER_IBS){
    if ((cgroup = ibs_counter_group("ibs",ibs_collection_profile,&ibs_params))){
      cgroup->next = cpu_info->systemwide_counters;
      cpu_info->systemwide_counters = cgroup;
    }
  }

  // CPU package energy (power/energy-pkg, power.c) is only system-wide too --
  // it's one cumulative machine-wide RAPL-equivalent register, not a
  // per-core countable event, so --per-core has no natural meaning here
  // either.
  if (counter_mask & COUNTER_POWER){
    if ((cgroup = power_counter_group("power"))){
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

  // --per-core CSV output needs a different row shape than the default
  // aggregate single row: aflag routes every counter group named in
  // counter_mask onto each core's own core_specific_counters rather than
  // cpu_info->systemwide_counters, so the old single aggregate row (built
  // from systemwide_counters alone) never had per-core group columns of
  // its own -- the header matched it, but per-core data was then appended
  // as extra unheaded columns below. Below, one CSV row per active core
  // (tagged with a "core" column) replaces that aggregate row instead.
  // Interval mode keeps its existing, separately-documented limitation --
  // timer_callback() only ever reads systemwide_counters, so per-core data
  // isn't visible in periodic ticks either way -- so this new shape only
  // applies outside --interval.
  if (aflag && csvflag && !interval){
    for (i=0;i<cpu_info->num_cores;i++){
      if (cpu_info->coreinfo[i].core_specific_counters){
	first_core_with_counters = i;
	break;
      }
    }
    per_core_csv = (first_core_with_counters >= 0);
  }

  signal(SIGINT,SIG_IGN);

  // interval automatic phase-boundary detection (warmup/steady/degraded);
  // see phase.h. .enabled is decided once here from the flags/counter_mask
  // that are now final, and read directly (not re-derived) by every tick.
  phase_detector_init(&phase_state,phase_detect_is_available());

  // create CSV headers
  if (csvflag){
    if (interval){
      fprintf(outfile,"time,");
      if (phase_state.enabled) fprintf(outfile,"phase,");
    }
    if (sflag) print_system(PRINT_CSV_HEADER);
    if (!interval && xflag) print_usage(NULL,PRINT_CSV_HEADER);
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      /* print_system() already emits an equivalent gpu_busy column when
       * sflag is set (same amd_sysfs_gpu_busy_percent() source) -- this
       * standalone column would just duplicate it. gpu_metrics/gpu_smi/
       * gpu_nvidia have no such home in print_system(), so unlike this one
       * they must NOT be gated on !sflag -- doing so used to silently drop
       * all GPU telemetry (temp/activity/power/freq/VRAM) whenever --system
       * was also given, since both header and value emission shared the
       * same (wrong) gate and so never showed up as a column-count mismatch.
       */
      fprintf(outfile,"gpu_busy,");
    }
    if (gpu_metrics_requested){
      fprintf(outfile,"gpu_temp,gpu_activity,gpu_power,gpu_freq,");
    }
    if (gpu_smi_requested){
      fprintf(outfile,"gpu_smi_temp,gpu_smi_activity,gpu_smi_vram_used,gpu_smi_vram_total,");
    }
#endif
#if NVIDIA
    if (gpu_nvidia_requested){
      fprintf(outfile,"nv_gpu_busy,nv_vram_used_mb,nv_vram_total_mb,");
    }
#endif
    if (per_core_csv){
      fprintf(outfile,"core,");
      print_metrics(cpu_info->coreinfo[first_core_with_counters].core_specific_counters,PRINT_CSV_HEADER);
    }
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
    child_exit_known = 1;
    child_exited = WIFEXITED(status) ? 1 : 0;
    if (child_exited) child_exit_code = WEXITSTATUS(status);
    child_signaled = WIFSIGNALED(status) ? 1 : 0;
    if (child_signaled) child_term_signal = WTERMSIG(status);
  }

  is_still_running = 0;

  // Close the race between the last periodic --interval tick and this
  // final tail-row print: alarm(interval) is always re-armed at the end
  // of timer_callback() (topdown.c) for as long as is_still_running was 1
  // at that point, so a SIGALRM can already be pending/in flight the
  // instant the blocking wait above returns -- setting is_still_running=0
  // only stops the *next* re-arm, it doesn't retract a signal the kernel
  // has already queued. If that signal is then delivered partway through
  // the tail row's own sequence of fprintf() calls below, timer_callback()
  // runs to completion (printing its own full periodic row, including its
  // trailing newline) in the middle of it, splicing two rows' text
  // together with no line break between them -- reproducible with e.g.
  // `sleep 3 --interval 1`. Blocking SIGALRM and disarming the timer here,
  // as the very first thing after the blocking wait returns (before any
  // of the tail row's own output), leaves only the handful of instructions
  // between the wait returning and this sigprocmask() call as a residual
  // window -- narrowed from "the entire multi-fprintf tail print sequence"
  // to effectively zero, not made mathematically impossible. No matching
  // unblock is needed: once the child has exited there's no further
  // legitimate use for SIGALRM before the process exits.
  if (interval){
    sigset_t alrm_mask;
    sigemptyset(&alrm_mask);
    sigaddset(&alrm_mask,SIGALRM);
    sigprocmask(SIG_BLOCK,&alrm_mask,NULL);
    alarm(0);
  }

  clock_gettime(CLOCK_REALTIME,&finish_time);

  read_system();

  // -----  
  // stop core-specific and system-wide counters
  for (i=0;i<cpu_info->num_cores;i++){
    if (cpu_info->coreinfo[i].core_specific_counters)
      read_counters(cpu_info->coreinfo[i].core_specific_counters,1);
  }
  read_counters(cpu_info->systemwide_counters,1);

  // classify this final (tail) interval tick the same way timer_callback()
  // classifies every periodic one, so the last row/print reflects it too
  if (phase_state.enabled){
    double phase_elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
      start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
    final_phase = phase_detector_update(&phase_state,
      phase_current_ipc(cpu_info->systemwide_counters),phase_elapsed);
  }

  if (csvflag && per_core_csv){
    // one row per active core, each tagged with its core index -- matches
    // the "core,"+group-header addition above. The old aggregate row is
    // folded into these (software/ibs's systemwide-only groups and the
    // coverage counts are the same on every row, same as they'd be on the
    // single row they used to only appear on once).
    for (i=0;i<cpu_info->num_cores;i++){
      if (!cpu_info->coreinfo[i].core_specific_counters) continue;
      if (sflag) print_system(PRINT_CSV);
      if (xflag) print_usage(&rusage,PRINT_CSV);
#if AMDGPU
      if (!sflag && gpu_busy_requested){
        int busy = amd_sysfs_gpu_busy_percent();
        fprintf(outfile,"%d,",busy);
      }
      if (gpu_metrics_requested){
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
      if (gpu_smi_requested){
        amd_smi_metrics();
        amd_smi_memory();
        fprintf(outfile,"%u,%u,%u,%u,",
          amd_smi_metrics_valid() ? amd_smi_get_temp() : 0,
          amd_smi_metrics_valid() ? amd_smi_get_activity() : 0,
          amd_smi_memory_valid() ? amd_smi_get_vram_used() : 0,
          amd_smi_memory_valid() ? amd_smi_get_vram_total() : 0);
      }
#endif
#if NVIDIA
      if (gpu_nvidia_requested){
        nvidia_nvml_metrics();
        fprintf(outfile,"%u,%llu,%llu,",
          nvidia_nvml_metrics_valid() ? nvidia_nvml_get_busy() : 0,
          nvidia_nvml_metrics_valid() ? (unsigned long long)nvidia_nvml_get_vram_used_mb() : 0,
          nvidia_nvml_metrics_valid() ? (unsigned long long)nvidia_nvml_get_vram_total_mb() : 0);
      }
#endif
      fprintf(outfile,"%d,",i);
      print_metrics(cpu_info->coreinfo[i].core_specific_counters,PRINT_CSV);
      print_metrics(cpu_info->systemwide_counters,PRINT_CSV);
      print_counter_coverage(PRINT_CSV);
      fprintf(outfile,"\n");
    }
  } else if (csvflag){
    if (interval){
      double elapsed;

      elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
	start_time.tv_sec - start_time.tv_nsec / 1000000000.0;
      fprintf(outfile,"%4.1f,",elapsed);
      if (phase_state.enabled) fprintf(outfile,"%s,",phase_name(final_phase));
    }
    if (sflag) print_system(PRINT_CSV);
    if (xflag && !interval) print_usage(&rusage,PRINT_CSV);
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      int busy = amd_sysfs_gpu_busy_percent();
      fprintf(outfile,"%d,",busy);
    }
    if (gpu_metrics_requested){
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
    if (gpu_smi_requested){
      amd_smi_metrics();
      amd_smi_memory();
      fprintf(outfile,"%u,%u,%u,%u,",
        amd_smi_metrics_valid() ? amd_smi_get_temp() : 0,
        amd_smi_metrics_valid() ? amd_smi_get_activity() : 0,
        amd_smi_memory_valid() ? amd_smi_get_vram_used() : 0,
        amd_smi_memory_valid() ? amd_smi_get_vram_total() : 0);
    }
#endif
#if NVIDIA
    if (gpu_nvidia_requested){
      nvidia_nvml_metrics();
      fprintf(outfile,"%u,%llu,%llu,",
        nvidia_nvml_metrics_valid() ? nvidia_nvml_get_busy() : 0,
        nvidia_nvml_metrics_valid() ? (unsigned long long)nvidia_nvml_get_vram_used_mb() : 0,
        nvidia_nvml_metrics_valid() ? (unsigned long long)nvidia_nvml_get_vram_total_mb() : 0);
    }
#endif
    print_metrics(cpu_info->systemwide_counters,PRINT_CSV);
    print_counter_coverage(PRINT_CSV);
    fprintf(outfile,"\n");
  } else {
    if (sflag) print_system(PRINT_NORMAL);
    if (xflag) print_usage(&rusage,PRINT_NORMAL);
    if (phase_state.enabled) fprintf(outfile,"phase                %s\n",phase_name(final_phase));
    print_metrics(cpu_info->systemwide_counters,PRINT_NORMAL);
    print_counter_coverage(PRINT_NORMAL);
    if (phase_state.enabled) phase_print_boundaries(&phase_state);
#if AMDGPU
    if (!sflag && gpu_busy_requested){
      int busy_final = amd_sysfs_gpu_busy_percent();
      fprintf(outfile,"gpu busy             %d%%\n",busy_final);
    }
    if (gpu_metrics_requested){
      amd_sysfs_gpu_metrics();
      if (amd_sysfs_gpu_metrics_valid()){
        fprintf(outfile,"gpu temp             %d C\n", amd_sysfs_get_gpu_temp());
        fprintf(outfile,"gpu activity         %u%%\n", amd_sysfs_get_gpu_activity());
        fprintf(outfile,"gpu power            %.2f W\n", amd_sysfs_get_gpu_power());
        fprintf(outfile,"gpu freq             %u MHz\n", amd_sysfs_get_gpu_freq());
      }
    }
    if (gpu_smi_requested){
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
#if NVIDIA
    if (gpu_nvidia_requested){
      nvidia_nvml_metrics();
      if (nvidia_nvml_metrics_valid()){
        fprintf(outfile,"nv gpu busy          %u%%\n", nvidia_nvml_get_busy());
        fprintf(outfile,"nv vram used         %llu MB\n", (unsigned long long)nvidia_nvml_get_vram_used_mb());
        fprintf(outfile,"nv vram total        %llu MB\n", (unsigned long long)nvidia_nvml_get_vram_total_mb());
      }
    }
#endif
  }

  // per_core_csv already emitted one row per core above; this loop only
  // has work left to do for the human (non-CSV) block form, or for the
  // pre-existing --interval + --per-core combination that per_core_csv
  // deliberately doesn't touch (see the comment where per_core_csv is
  // computed).
  if (!per_core_csv){
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
  }

  if (oflag) fclose(outfile);
  // -----

  if (manifest_path || run_index_path){
    struct manifest_info minfo;
    memset(&minfo,0,sizeof(minfo));
    populate_manifest_common(&minfo);
    minfo.counter_mask = counter_mask;
    if (manifest_path) write_manifest(manifest_path,&minfo);
    if (run_index_path) append_run_index(run_index_path,&minfo);
    free((void *)minfo.counters_unavailable);
  }

#if AMDGPU
  if (gpu_smi_requested)
    amd_smi_finalize();
  if (gpu_busy_requested || gpu_metrics_requested)
    amd_sysfs_finalize();
#endif
#if NVIDIA
  if (gpu_nvidia_requested)
    nvidia_nvml_finalize();
#endif

  return exit_code_epilogue();
}
