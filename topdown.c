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

int num_procs;
int cflag = 0;
int oflag = 0;
int xflag = 0;
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


struct counterdef {
  char *name;
  unsigned int event;
  unsigned int umask;
  unsigned int cmask;
  unsigned int any;
  unsigned int scale;
  unsigned int use;
};
struct counterdef counters[] = {
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

struct countergroup {
  char *label;
  int num_counters;
  char *names[6];
  unsigned int use;
};
struct countergroup groups[] = {
  { "ipc",
    2,
    { "instructions",
      "cpu-cycles" },
    USE_IPC },
  { "level1",
    5,
    { "topdown-total-slots",
      "topdown-fetch-bubbles",
      "topdown-recovery-bubbles",
      "topdown-slots-issued",
      "topdown-slots-retired" },
    USE_L1,
  },
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
  while ((opt = getopt(argc,argv,"abcfil:o:rsx")) != -1){
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
  return ret;
}

void setup_counters(void){
  unsigned int mask = 0;
  int i,j,index,count,count2;
  int status;
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
  // count the # of performance counters
  count = 0;
  for (i=0;i<sizeof(counters)/sizeof(counters[0]);i++){
    if (mask & counters[i].use) count++;
  }
  // allocate space for the counters
  num_core_counters = count;
  num_total_counters = count * num_procs;
  app_counters = calloc(sizeof(struct counterdata),num_total_counters);

  // collect together the counter definitions for each core
  count2 = 0;
  for (i=0;i<sizeof(counters)/sizeof(counters[0]);i++){
    if (mask & counters[i].use){
      for (j=0;j<num_procs;j++){
	index = j*count + count2;
	app_counters[index].corenum = j;
	app_counters[index].definition = &counters[i];
      }
      count2++;
    }
  }
  // set up the counters and leave them disabled
  for (i=0;i<num_total_counters;i++){
    debug("core=%d, counter=%s\n",app_counters[i].corenum,app_counters[i].definition->name);
    memset(&pe,0,sizeof(pe));
    pe.type = PERF_TYPE_RAW;
    pe.config = app_counters[i].definition->event |
      (app_counters[i].definition->umask<<8) |
      (app_counters[i].definition->any<<21) |
      (app_counters[i].definition->cmask<<24);
    pe.sample_type = PERF_SAMPLE_IDENTIFIER;
    pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
    pe.size = sizeof(struct perf_event_attr);
    pe.disabled = 1;
    status = perf_event_open(&pe,-1,app_counters[i].corenum,-1,0);
    if (status == -1){
      error("unable to open performance counter cpu=%d, name=%s, errno=%d\n",
	    app_counters[i],app_counters[i].definition->name,errno);
    } else {
      app_counters[i].fd = status;
      ioctl(app_counters[i].fd,PERF_EVENT_IOC_RESET,0);
    }
  }
}

void start_counters(void){
  int i;
  for (i=0;i<num_total_counters;i++){
    ioctl(app_counters[i].fd,PERF_EVENT_IOC_ENABLE,0);
  }
}

void stop_counters(void){
  int i;
  int status;
  struct read_format { uint64_t value, time_enabled, time_running,id; } rf;
  for (i=0;i<num_total_counters;i++){
    status = read(app_counters[i].fd,&rf,sizeof(rf));
    if (status == -1){
      error("unable to read performance counter cpu=%d, name=%s, errno=%d\n",
	    app_counters[i],app_counters[i].definition->name,errno);
    } else {
      app_counters[i].value = rf.value * ((double) rf.time_enabled / rf.time_running);
      if (app_counters[i].definition->scale){
	app_counters[i].value *= app_counters[i].definition->scale;
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
	  (double) rusage->ru_utime.tv_sec +
	  rusage->ru_utime.tv_usec / 1000000.0);
  fprintf(outfile,"nvcsw          %lu (%4.2f%%)\n",
	  rusage->ru_nvcsw,(double) rusage->ru_nvcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"nivcsw         %lu (%4.2f%%)\n",
	  rusage->ru_nivcsw,(double) rusage->ru_nivcsw / (rusage->ru_nvcsw + rusage->ru_nivcsw) * 100.0);
  fprintf(outfile,"inblock        %lu\n",rusage->ru_inblock);
  fprintf(outfile,"onblock        %lu\n",rusage->ru_oublock);  
}

void print_ipc(void){
  int i;
  unsigned long int instructions[4];
  unsigned long int cpu_cycles[4];
  unsigned long int total_instructions = 0;
  unsigned long int total_cpu_cycles = 0;
  for (i=0;i<4;i++){
    instructions[i] = 0;
    cpu_cycles[i] = 0;
  }
  for (i=0;i<num_total_counters;i++){
    if (!strcmp(app_counters[i].definition->name,"instructions")){
      instructions[app_counters[i].corenum % 4] += app_counters[i].value;
      total_instructions += app_counters[i].value;
    } else if (!strcmp(app_counters[i].definition->name,"cpu-cycles")){
      cpu_cycles[app_counters[i].corenum % 4] += app_counters[i].value;
      total_cpu_cycles += app_counters[i].value;
    }
  }
  fprintf(outfile,"IPC\t%4.3f\n",
	  (double) total_instructions / total_cpu_cycles);
  if (cflag){
    for (i=0;i<4;i++){
      fprintf(outfile,"%d.IPC\t%4.3f\n",i,
	      (double) instructions[i] / cpu_cycles[i]);
    }
  }
}

void print_topdown1(void){
  int i;
  unsigned long int topdown_total_slots[4];
  unsigned long int topdown_fetch_bubbles[4];
  unsigned long int topdown_recovery_bubbles[4];
  unsigned long int topdown_slots_issued[4];
  unsigned long int topdown_slots_retired[4];
  unsigned long int resource_stalls_sb[4];
  unsigned long int stalls_ldm_pending[4];
  unsigned long int uops0_delivered[4],uops1_delivered[4],uops2_delivered[4],uops3_delivered[4];
  unsigned long int branch_misses[4],machine_clears[4],ms_uops[4];
  unsigned long int icache_stall[4],itlb_stlb_hit[4],itlb_walk_duration[4],dsb_uops[4];
  unsigned long int l2_refs[4],l2_misses[4],l3_refs[4],l3_misses[4];
  unsigned long int total_topdown_total_slots=0,total_topdown_fetch_bubbles=0,
    total_topdown_recovery_bubbles=0,total_topdown_slots_issued=0,total_topdown_slots_retired=0,
    total_resource_stalls_sb=0,total_stalls_ldm_pending=0,
    total_uops0_delivered=0,total_uops1_delivered=0,total_uops2_delivered=0,total_uops3_delivered=0,
    total_branch_misses=0,total_machine_clears=0,total_ms_uops=0,
    total_icache_stall=0,total_itlb_stlb_hit=0,total_itlb_walk_duration=0,total_dsb_uops = 0,
    total_l2_refs=0,total_l2_misses=0,total_l3_refs=0,total_l3_misses=0;
  double frontend_bound,retiring,speculation,backend_bound;
  for (i=0;i<4;i++){
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
      topdown_total_slots[app_counters[i].corenum % 4] += app_counters[i].value;
      total_topdown_total_slots += app_counters[i].value;
    } else if (!strcmp(app_counters[i].definition->name,"topdown-fetch-bubbles")){
      topdown_fetch_bubbles[app_counters[i].corenum % 4] += app_counters[i].value;
      total_topdown_fetch_bubbles += app_counters[i].value;      
    } else if (!strcmp(app_counters[i].definition->name,"topdown-recovery-bubbles")){
      topdown_recovery_bubbles[app_counters[i].corenum % 4] += app_counters[i].value;
      total_topdown_recovery_bubbles += app_counters[i].value;            
    } else if (!strcmp(app_counters[i].definition->name,"topdown-slots-issued")){
      topdown_slots_issued[app_counters[i].corenum % 4] += app_counters[i].value;
      total_topdown_slots_issued += app_counters[i].value;                  
    } else if (!strcmp(app_counters[i].definition->name,"topdown-slots-retired")){
      topdown_slots_retired[app_counters[i].corenum % 4] += app_counters[i].value;
      total_topdown_slots_retired += app_counters[i].value;                        
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"resource-stalls.sb")){
      resource_stalls_sb[app_counters[i].corenum % 4] += app_counters[i].value;
      total_resource_stalls_sb += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"cycle-activity.stalls-ldm-pending")){
      stalls_ldm_pending[app_counters[i].corenum % 4] += app_counters[i].value;
      total_stalls_ldm_pending += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.0_uops")){
      uops0_delivered[app_counters[i].corenum % 4] += app_counters[i].value;
      total_uops0_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.1_uops")){
      uops1_delivered[app_counters[i].corenum % 4] += app_counters[i].value;
      total_uops1_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.2_uops")){
      uops2_delivered[app_counters[i].corenum % 4] += app_counters[i].value;
      total_uops2_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq_uops_not_delivered.3_uops")){
      uops3_delivered[app_counters[i].corenum % 4] += app_counters[i].value;
      total_uops3_delivered += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"branch-misses")){
      branch_misses[app_counters[i].corenum % 4] += app_counters[i].value;
      total_branch_misses += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"machine_clears.count")){
      machine_clears[app_counters[i].corenum % 4] += app_counters[i].value;
      total_machine_clears += app_counters[i].value;
    } else if ((level > 1) && !strcmp(app_counters[i].definition->name,"idq.ms_uops")){
      ms_uops[app_counters[i].corenum % 4] += app_counters[i].value;
      total_ms_uops += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"icache.ifdata_stall")){
      icache_stall[app_counters[i].corenum % 4] += app_counters[i].value;
      total_icache_stall += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"itlb_misses.stlb_hit")){
      itlb_stlb_hit[app_counters[i].corenum % 4] += app_counters[i].value;
      total_itlb_stlb_hit += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"itlb_misses.walk_duration")){
      itlb_walk_duration[app_counters[i].corenum % 4] += app_counters[i].value;
      total_itlb_walk_duration += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"idq.dsb_uops")){
      dsb_uops[app_counters[i].corenum % 4] += app_counters[i].value;
      total_dsb_uops += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"l2_rqsts.reference")){
      l2_refs[app_counters[i].corenum % 4] += app_counters[i].value;
      total_l2_refs += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"l2_rqsts.miss")){
      l2_misses[app_counters[i].corenum % 4] += app_counters[i].value;
      total_l2_misses += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"longest_lat_cache.reference")){
      l3_refs[app_counters[i].corenum % 4] += app_counters[i].value;
      total_l3_refs += app_counters[i].value;
    } else if ((level > 2) && !strcmp(app_counters[i].definition->name,"longest_lat_cache.miss")){
      l3_misses[app_counters[i].corenum % 4] += app_counters[i].value;
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
    for (i=0;i<4;i++){
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
      fatal("usage: %s -[abcfrs][-l <1|2|3|4>][-o <file>] <cmd><args>...\n"
	    "\t-l <level> - expand out <level> levels (default 1)\n"
	    "\t-c         - show cores as separate\n"
	    "\t-o <file>  - send output to <file>\n"
	    "\t-a         - expand all areas\n"
	    "\t-b         - expand backend stalls area\n"
	    "\t-f         - expand frontend stalls area\n"
	    "\t-r         - expand retiring area\n"
	    "\t-s         - expand speculation area\n"
	    ,argv[0]);
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
    print_ipc();
  } else {
    print_topdown1();
  }
  
  if (oflag) fclose(outfile);
  return 0;
}
