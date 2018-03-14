/*
 * cpustatus - program to read and process CPU status information
 *             Initially looks at contents of /proc/stat
 *             For CPU state
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/sysinfo.h>
#include "wspy.h"
#include "error.h"

FILE *cpufile = NULL;

void init_cpustatus(void){
  cpufile = tmpfile();
}

void read_cpustatus(double time){
  FILE *fp = fopen("/proc/stat","r");
  char buffer[1024];

  fprintf(cpufile,"time %f\n",time);
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    if (strncmp(buffer,"cpu",3)) break;
    fputs(buffer,cpufile);
  }
  fclose(fp);
}

void print_cpustatus(double basetime){
  int i,len,first;
  char cpubuf[16];
  char buffer[1024];
  long int time_sec,time_usec;
  double elapsed = 0;
  struct statinfo {
    int user,nice,system,idle,iowait,irq,softirq,steal,guest,guestnice;
  } last_st,curr_st,zero_st = { 0 };
  int time_user,time_system,time_iowait,time_irq,time_idle,time_total;

  for (i=0;i<get_nprocs();i++){
    rewind(cpufile);
    sprintf(cpubuf,"cpu%d",i);
    len = strlen(cpubuf);
    curr_st = zero_st;
    first = 1;

    fprintf(outfile,"CPU %d\t-\tuser\tsystem\tiowait\tirq\tidle\n",i);
    while (fgets(buffer,sizeof(buffer),cpufile) != NULL){
      if (!strncmp(buffer,"time",4)){
	sscanf(buffer,"time %lf",&elapsed);
      }
      if (!strncmp(buffer,cpubuf,len)){
	last_st = curr_st;
	sscanf(&buffer[len],"%d %d %d %d %d %d %d %d %d %d",
	       &curr_st.user,
	       &curr_st.nice,
	       &curr_st.system,
	       &curr_st.idle,
	       &curr_st.iowait,
	       &curr_st.irq,
	       &curr_st.softirq,
	       &curr_st.steal,
	       &curr_st.guest,
	       &curr_st.guestnice);
	if (first){
	  first = 0;
	} else {
	  time_user = curr_st.user - last_st.user + curr_st.nice - last_st.nice;
	  time_system = curr_st.system - last_st.system;
	  time_iowait = curr_st.iowait - last_st.iowait;
	  time_irq = curr_st.irq - last_st.irq + curr_st.softirq - last_st.softirq;
	  time_idle = curr_st.idle - last_st.idle;
	  time_total = time_user + time_system + time_iowait + time_irq + time_idle;
	  fprintf(outfile,"%10.2f\t%3.0f%%\t%3.0f%%\t%3.0f%%\t%3.0f%%\t%3.0f%%\n",elapsed,
		  round((double) time_user / time_total * 100.0),
		  round((double) time_system / time_total * 100.0),
		  round((double) time_iowait / time_total * 100.0),
		  round((double) time_irq / time_total * 100.0),
		  round((double) time_idle / time_total * 100.0));
	}
      }
    }
  }
}
