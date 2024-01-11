/*
 * system.c - system-wide status
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "wspy.h"
#include "error.h"

unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU;

// system state
struct system_state {
  double load; // 1 minute load average
  int runnable; // # runnable processes
  struct cpustat {
    unsigned long usertime, systemtime, idletime, iowaittime, irqtime;
    unsigned long last_usertime, last_systemtime, last_idletime, last_iowaittime, last_irqtime;
    unsigned long prev_usertime, prev_systemtime, prev_idletime, prev_iowaittime, prev_irqtime; } cpu;
} system_state = { 0 };

// read system-wide state
void read_system(void){
  FILE *fp;
  char buffer[1024];
  double value1,value5,value15;
  int runnable;
  unsigned long usertime,nicetime,systemtime,idletime,iowait,irqtime,softirqtime;
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
}

void print_system(enum output_format oformat){
  double elapsed;
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
  }
}
