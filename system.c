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
#include <dirent.h>
#include "wspy.h"
#include "error.h"
#if AMDGPU
#include "amd_sysfs.h"
#endif

unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK|SYSTEM_FREQ;

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
  double freq_mhz; // average current frequency across online cpus with cpufreq
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

// cpu ids (from /sys/devices/system/cpu/cpu<N>) that expose cpufreq's
// scaling_cur_freq, cached once since the set doesn't change mid-run
static int *freq_cpu_ids = NULL;
static int num_freq_cpus = 0;
static int freq_setup_done = 0;

// scan /sys/devices/system/cpu for cpu<N> entries with a readable
// cpufreq/scaling_cur_freq file -- readdir-driven rather than assuming
// 0..num_procs-1 are all present/online, same idiom as amd_sysfs.c's
// device scan
static void setup_freq_info(void){
  DIR *dir;
  struct dirent *entry;
  int capacity = 0;
  char path[256];
  char *endptr;
  long id;

  freq_setup_done = 1;
  if ((dir = opendir("/sys/devices/system/cpu")) == NULL) return;
  while ((entry = readdir(dir)) != NULL){
    if (strncmp(entry->d_name,"cpu",3) != 0) continue;
    id = strtol(entry->d_name+3,&endptr,10);
    if (endptr == entry->d_name+3 || *endptr != '\0') continue; // not "cpu<digits>"
    snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq",id);
    if (access(path,R_OK) != 0) continue;
    if (num_freq_cpus >= capacity){
      int newcap = capacity ? capacity * 2 : 8;
      int *newlist = realloc(freq_cpu_ids,newcap * sizeof(int));
      if (newlist == NULL) break;
      freq_cpu_ids = newlist;
      capacity = newcap;
    }
    freq_cpu_ids[num_freq_cpus++] = (int) id;
  }
  closedir(dir);
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
  if (system_mask & SYSTEM_FREQ){
    unsigned long khz;
    double sum = 0;
    int count = 0;
    if (!freq_setup_done) setup_freq_info();
    for (i=0;i<num_freq_cpus;i++){
      snprintf(buffer,sizeof(buffer),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",freq_cpu_ids[i]);
      if ((fp = fopen(buffer,"r"))){
        if (fscanf(fp,"%lu",&khz) == 1){
          sum += khz;
          count++;
        }
        fclose(fp);
      }
    }
    // cpufreq unavailable (e.g. some VMs/containers) degrades to 0 rather
    // than failing, same "measured vs unavailable" idiom used throughout
    system_state.freq_mhz = count ? (sum / count) / 1000.0 : 0.0;
  }
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
    if (system_mask & SYSTEM_FREQ) fprintf(outfile,"freq,");
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
    if (system_mask & SYSTEM_FREQ) fprintf(outfile,"%4.0f,",system_state.freq_mhz);
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
	      (double) (system_state.cpu.irqtime)/elapsed/num_procs);
    }
    if (system_mask & SYSTEM_FREQ){
      fprintf(outfile,"freq                 %4.0f MHz\n",system_state.freq_mhz);
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
