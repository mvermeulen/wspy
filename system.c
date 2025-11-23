/*
 * system.c - system-wide status
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include "wspy.h"
#include "error.h"
#if AMDGPU
#include "amd_sysfs.h"
#endif

unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK;

struct netinfo {
  char *name;
  unsigned long bytes, last_bytes, prev_bytes;
};

// system state
struct system_state {
  double load; // 1 minute load average
  int runnable; // # runnable processes
  struct cpustat {
    unsigned long usertime, systemtime, idletime, iowaittime, irqtime;
    unsigned long last_usertime, last_systemtime, last_idletime, last_iowaittime, last_irqtime;
    unsigned long prev_usertime, prev_systemtime, prev_idletime, prev_iowaittime, prev_irqtime;
  } cpu;
  int num_net;
  struct netinfo *netinfo;
#if AMDGPU
  struct gpu {
    int busy_percent;
    int last_busy_percent;
    int prev_busy_percent;
  } gpu;
#endif
} system_state = { 0 };

// read /proc/net/dev and initialize the system_state structure for networks...
void setup_net_info(void){
  FILE *fp;
  char buffer[1024];
  char *p,*p2;
  int capacity = 0;
  if ((fp = fopen("/proc/net/dev","r")) == NULL) return;
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    p = strchr(buffer,':');
    if (p == NULL) continue; // skip header lines
    *p = 0;
    p2 = buffer;
    while (isspace(*p2)) p2++;
    if (system_state.num_net >= capacity){
      int newcap = capacity ? capacity * 2 : 4;
      struct netinfo *newlist = realloc(system_state.netinfo,newcap * sizeof(struct netinfo));
      if (newlist == NULL){
        fclose(fp);
        return;
      }
      // zero initialize new slots
      if (newcap > capacity){
        memset(newlist + capacity,0,(newcap - capacity)*sizeof(struct netinfo));
      }
      system_state.netinfo = newlist;
      capacity = newcap;
    }
    system_state.netinfo[system_state.num_net].name = strdup(p2);
    if (system_state.netinfo[system_state.num_net].name == NULL || system_state.netinfo[system_state.num_net].name[0] == '\0'){
      char tmp[32];
      snprintf(tmp,sizeof(tmp),"net%d",system_state.num_net);
      if (system_state.netinfo[system_state.num_net].name) free(system_state.netinfo[system_state.num_net].name);
      system_state.netinfo[system_state.num_net].name = strdup(tmp);
    }
    debug("parsed network interface: %s\n",system_state.netinfo[system_state.num_net].name);
    system_state.num_net++;
  }
  fclose(fp);
  if (system_state.num_net == 0){
    free(system_state.netinfo);
    system_state.netinfo = NULL;
  }
}

// read system-wide state
void read_system(void){
  FILE *fp;
  char buffer[1024];
  char *p,*p2;
  double value1,value5,value15;
  int runnable;
  unsigned long usertime,nicetime,systemtime,idletime,iowait,irqtime,softirqtime;
  int i;
  if (system_mask & SYSTEM_LOADAVG){
    if ((fp = fopen("/proc/loadavg","r"))){
      if (fscanf(fp,"%lf %lf %lf %d",&value1,&value5,&value15,&runnable) == 4){
        system_state.load = value1;
        system_state.runnable = runnable;
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_CPU){
    if ((fp = fopen("/proc/stat","r"))){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
        if (!strncmp(buffer,"cpu ",4)){
          if (sscanf(&buffer[4],"%lu %lu %lu %lu %lu %lu %lu",
                     &usertime,&nicetime,&systemtime,&idletime,&iowait,&irqtime,&softirqtime) == 7){
            system_state.cpu.prev_usertime = system_state.cpu.last_usertime;
            system_state.cpu.prev_systemtime = system_state.cpu.last_systemtime;
            system_state.cpu.prev_idletime = system_state.cpu.last_idletime;
            system_state.cpu.prev_iowaittime = system_state.cpu.last_iowaittime;
            system_state.cpu.prev_irqtime = system_state.cpu.last_irqtime;
            system_state.cpu.last_usertime = usertime;
            system_state.cpu.last_systemtime = systemtime;
            system_state.cpu.last_idletime = idletime;
            system_state.cpu.last_iowaittime = iowait;
            system_state.cpu.last_irqtime = irqtime + softirqtime;
            system_state.cpu.usertime = system_state.cpu.last_usertime - system_state.cpu.prev_usertime;
            system_state.cpu.systemtime = system_state.cpu.last_systemtime - system_state.cpu.prev_systemtime;
            system_state.cpu.idletime = system_state.cpu.last_idletime - system_state.cpu.prev_idletime;
            system_state.cpu.iowaittime = system_state.cpu.last_iowaittime - system_state.cpu.prev_iowaittime;
            system_state.cpu.irqtime = system_state.cpu.last_irqtime - system_state.cpu.prev_irqtime;
            break;
          }
        }
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_NETWORK){
    unsigned long int field1,field2,field3,field4,field5,field6,field7,field8,field9;
    if (system_state.netinfo == NULL) setup_net_info();
    if ((fp = fopen("/proc/net/dev","r"))){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
        p = strchr(buffer,':');
        if (p == NULL) continue;
        *p = 0;
        p++;
        p2 = buffer;
        while (isspace(*p2)) p2++;
        for (i=0;i<system_state.num_net;i++){
          if (system_state.netinfo[i].name && !strcmp(p2,system_state.netinfo[i].name)){
            if (sscanf(p,"%lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       &field1,&field2,&field3,&field4,&field5,&field6,&field7,&field8,&field9) == 9){
              system_state.netinfo[i].prev_bytes = system_state.netinfo[i].last_bytes;
              system_state.netinfo[i].last_bytes = field1 + field9;
              system_state.netinfo[i].bytes = system_state.netinfo[i].last_bytes - system_state.netinfo[i].prev_bytes;
              continue;
            }
          }
        }
      }
      fclose(fp);
    }
  }
#if AMDGPU
  if (system_mask & SYSTEM_GPU){
    system_state.gpu.prev_busy_percent = system_state.gpu.last_busy_percent;
    system_state.gpu.last_busy_percent = amd_sysfs_gpu_busy_percent();
    system_state.gpu.busy_percent = system_state.gpu.last_busy_percent;
  }
#endif
}

void print_system(enum output_format oformat){
  double elapsed;
  int i;
  if (interval) elapsed = interval;
  else {
    clock_gettime(CLOCK_REALTIME,&finish_time);
    elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
      start_time.tv_sec - start_time.tv_nsec / 1000000000.0;    
  }

  switch(oformat){
  case PRINT_CSV_HEADER:
    if (system_mask & SYSTEM_LOADAVG) fprintf(outfile,"load,runnable,");
    if (system_mask & SYSTEM_CPU) fprintf(outfile,"cpu,idle,iowait,irq,");
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++) fprintf(outfile,"net %s,",system_state.netinfo[i].name);
    }
#if AMDGPU
    if (system_mask & SYSTEM_GPU) fprintf(outfile,"gpu_busy,");
#endif
    break;
  case PRINT_CSV:
    if (system_mask & SYSTEM_LOADAVG) fprintf(outfile,"%4.2f,%d,",system_state.load,system_state.runnable);
    if (system_mask & SYSTEM_CPU){
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.usertime+system_state.cpu.systemtime)/elapsed/num_procs);
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.idletime)/elapsed/num_procs);
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.iowaittime)/elapsed/num_procs);      
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.irqtime)/elapsed/num_procs);
    }
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%lu,",system_state.netinfo[i].bytes);
      }
    }
#if AMDGPU
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"%d,",system_state.gpu.busy_percent);
    }
#endif
    break;
  case PRINT_NORMAL:
    if (system_mask & SYSTEM_LOADAVG){
      fprintf(outfile,"load                 %4.2f\n",system_state.load);
      fprintf(outfile,"runnable             %d\n",system_state.runnable);      
    }
    if (system_mask & SYSTEM_CPU){
      fprintf(outfile,"cpu                  %3.2f%%\n",
	      (double) (system_state.cpu.usertime+system_state.cpu.systemtime)/elapsed/num_procs);
      fprintf(outfile,"idle                 %3.2f%%\n",
	      (double) (system_state.cpu.idletime/elapsed/num_procs));
      fprintf(outfile,"io                   %3.2f%%\n",
	      (double) (system_state.cpu.iowaittime/elapsed/num_procs));      
      fprintf(outfile,"irq                  %3.2f%%\n",
	      (double) (system_state.cpu.usertime+system_state.cpu.systemtime)/elapsed/num_procs);      
    }
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%-14s       %lu\n",system_state.netinfo[i].name,system_state.netinfo[i].bytes);
      }
    }
#if AMDGPU
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"gpu busy             %d%%\n",system_state.gpu.busy_percent);
    }
#endif
  }
}
