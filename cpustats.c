/*
 * cpustatus - program to read and process CPU status information
 *             Initially looks at contents of /proc/stat
 *             For CPU state
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <libgen.h>
#include "wspy.h"
#include "error.h"

FILE *cpufile = NULL;

void print_cpustats_gnuplot_file(void);

void init_cpustats(void){
  cpufile = tmpfile();
}

void read_cpustats(double time){
  FILE *fp = fopen("/proc/stat","r");
  char buffer[1024];

  fprintf(cpufile,"time %f\n",time);
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    if (strncmp(buffer,"cpu",3)) break;
    fputs(buffer,cpufile);
  }
  fclose(fp);
}

void print_cpuinfo(char *name,char *delim,FILE *output){
  char buffer[1024];
  int len = strlen(name);
  int first = 1;
  double elapsed = 0;
  struct statinfo {
    int user,nice,system,idle,iowait,irq,softirq,steal,guest,guestnice;
  } last_st,curr_st,zero_st = { 0 };
  int time_user,time_system,time_iowait,time_irq,time_idle,time_total;
  
  rewind(cpufile);
  fprintf(output,"%s%suser%ssystem%siowait%sirq%sidle\n",name,
	  delim,delim,delim,delim,delim);
  while (fgets(buffer,sizeof(buffer),cpufile) != NULL){
    if (!strncmp(buffer,"time",4)){
      sscanf(buffer,"time %lf",&elapsed);
    }
    if (!strncmp(buffer,name,len)){
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
	fprintf(output,"%-10.2f%s%3.0f%%%s%3.0f%%%s%3.0f%%%s%3.0f%%%s%3.0f%%\n",
		elapsed,delim,
		round((double) time_user / time_total * 100.0),delim,
		round((double) time_system / time_total * 100.0),delim,
		round((double) time_iowait / time_total * 100.0),delim,
		round((double) time_irq / time_total * 100.0),delim,
		round((double) time_idle / time_total * 100.0));
      }
    }
  }
}

void print_cpustats(void){
  int i;
  char cpubuf[16];

  print_cpuinfo("cpu ","\t",outfile);
  for (i=0;i<get_nprocs();i++){
    sprintf(cpubuf,"cpu%d",i);
    print_cpuinfo(cpubuf,"\t",outfile);
  }
}

void print_cpustats_files(void){
  char cpuname[16];
  char cpufile[32];
  int i;
  FILE *fp;
  fp = fopen("allcpu.csv","w");
  if (fp){
    print_cpuinfo("cpu ",",",fp);
  }
  fclose(fp);
  
  for (i=0;i<get_nprocs();i++){
    sprintf(cpuname,"cpu%d",i);
    sprintf(cpufile,"cpu%d.csv",i);
    fp = fopen(cpufile,"w");
    if (fp){
      print_cpuinfo(cpuname,",",fp);
    }
    fclose(fp);
  }
  print_cpustats_gnuplot_file();
}

void print_cpustats_gnuplot_file(void){
  int i;
  FILE *fp = fopen("cpu-gnuplot.sh","w");

  if (fp){
    fprintf(fp,"#!/bin/bash\n");
    fprintf(fp,"gnuplot <<PLOTCMD\n");
    fprintf(fp,"set terminal png\n");
    fprintf(fp,"set output 'allcpu.png'\n");
    fprintf(fp,"set title 'All CPUs'\n");
    fprintf(fp,"set datafile separator \",\"\n");    
    fprintf(fp,"plot 'allcpu.csv' using 1:2 with lines title 'user', 'allcpu.csv' using 1:3 with lines title 'system'\n");
    fprintf(fp,"PLOTCMD\n\n");

    fprintf(fp,"gnuplot <<PLOTCMD\n");
    fprintf(fp,"set terminal png\n");
    fprintf(fp,"set output 'cpulist.png'\n");
    fprintf(fp,"set title 'All CPUs'\n");
    fprintf(fp,"set datafile separator \",\"\n");        
    fprintf(fp,"plot");
    for (i=0;i<get_nprocs();i++){
      if (i != 0) fprintf(fp,",");
      fprintf(fp," 'cpu%d.csv' using 1:2 with lines title 'CPU %d'",i,i);
    }
    fprintf(fp,"\n");
    fprintf(fp,"PLOTCMD\n\n");
    
    for (i=0;i<get_nprocs();i++){
      fprintf(fp,"gnuplot <<PLOTCMD\n");
      fprintf(fp,"set terminal png\n");
      fprintf(fp,"set output 'cpu%d.png'\n",i);
      fprintf(fp,"set title 'CPU %d'\n",i);
      fprintf(fp,"set datafile separator \",\"\n");          
      fprintf(fp,"plot 'cpu%d.csv' using 1:2 with lines title 'user', 'cpu%d.csv' using 1:3 with lines title 'system'\n",i,i);
      fprintf(fp,"PLOTCMD\n\n");
    }
    fchmod(fileno(fp),0755);
    fclose(fp);
  }
}

