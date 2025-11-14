/*
 * system.c - system-wide status
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include "wspy.h"
#include "error.h"
#if AMDGPU
#include "gpu_info.h"
#endif

#if AMDGPU
unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK|SYSTEM_GPU;
#else
unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK;
#endif

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
#if AMDGPU
  struct gpu_query_data gpu;
#endif
  int num_net;
  struct netinfo *netinfo;
} system_state = { 0 };

// read /proc/net/dev and initialize the system_state structure for networks...
void setup_net_info(void){
  FILE *fp;
  char buffer[1024];
  char *p,*p2;
  int i;
  int count = 0;
  if (fp = fopen("/proc/net/dev","r")){
    // process once to count the number of lines with ':'
    while (fgets(buffer,sizeof(buffer),fp) != NULL){
      p = strchr(buffer,':');
      if (p == NULL) continue;
      count++;
    }

    rewind(fp);
    system_state.num_net = count;
    system_state.netinfo = calloc(count,sizeof(struct netinfo));

    // process again to create device entries
    count = 0;
    while (fgets(buffer,sizeof(buffer),fp) != NULL){
      p = strchr(buffer,':');
      if (p == NULL) continue;      
      *p = 0;
      p2 = buffer;
      while (isspace(*p2)) p2++;
      system_state.netinfo[count].name = strdup(p2);
      count++;
    }
    fclose(fp);
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
  // loadavg
  if (system_mask & SYSTEM_LOADAVG){
    if (fp = fopen("/proc/loadavg","r")){
      if (fscanf(fp,"%lf %lf %lf %d",&value1,&value5,&value15,&runnable) == 4){
	system_state.load = value1;
	system_state.runnable = runnable;
      }
      fclose(fp);
    }
  }
  // cpu
  if (system_mask & SYSTEM_CPU){
    if (fp = fopen("/proc/stat","r")){
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
	    system_state.cpu.last_idletime = iowait;
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
#if AMDGPU
  // gpu
  if (system_mask & SYSTEM_GPU){
    gpu_info_query(&system_state.gpu);
  }
#endif

  //
  if (system_mask & SYSTEM_NETWORK){
    unsigned long int field1,field2,field3,field4,field5,field6,field7,field8,field9;
    if (system_state.netinfo == NULL) setup_net_info();
    if (fp = fopen("/proc/net/dev","r")){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
	p = strchr(buffer,':');
	if (p == NULL) continue;
	*p = 0;
	p++;
	p2 = buffer;
	while (isspace(*p2)) p2++;
	for (i=0;i<system_state.num_net;i++){
	  if (!strcmp(p2,system_state.netinfo[i].name)){
	    if (sscanf(p,"%lu %lu %lu %lu %lu %lu %lu %lu %lu",
		       &field1,&field2,&field3,&field4,&field5,&field6,&field7,&field8,&field9) == 9){
	      system_state.netinfo[i].prev_bytes = system_state.netinfo[i].last_bytes;
	      system_state.netinfo[i].last_bytes = field1 + field9; // receive and transmit
	      system_state.netinfo[i].bytes = system_state.netinfo[i].last_bytes - system_state.netinfo[i].prev_bytes;
	      continue;
	    }
	  }
	}
      }
      fclose(fp);
    }
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
#if AMDGPU
    if (system_mask & SYSTEM_GPU)
      fprintf(outfile,"gpu temp,gpu gfx,gpu umc,gpu_mm,");
#endif
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++) fprintf(outfile,"net %s,",system_state.netinfo[i].name);
    }
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
#if AMDGPU
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"%d,",system_state.gpu.temperature);
      fprintf(outfile,"%d%%,",system_state.gpu.gfx_activity);
      fprintf(outfile,"%d%%,",system_state.gpu.umc_activity);
      fprintf(outfile,"%d%%,",system_state.gpu.mm_activity);
    }
#endif
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%lu,",system_state.netinfo[i].bytes);
      }
    }    
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
#if AMDGPU
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"temperature          %dC\n",system_state.gpu.temperature);
      fprintf(outfile,"gpu gfx              %d%%\n",system_state.gpu.gfx_activity);
      fprintf(outfile,"gpu umc              %d%%\n",system_state.gpu.umc_activity);
      fprintf(outfile,"gpu mm               %d%%\n",system_state.gpu.mm_activity);      
    }
#endif
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%-14s       %lu\n",system_state.netinfo[i].name,system_state.netinfo[i].bytes);
      }
    }    
  }
}
