/*
 * pcounter.c - performance counter subsystem
 *
 * Implementation of a periodic sampling of performance counters
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/stat.h>
#include <linux/perf_event.h>
#include "wspy.h"
#include "error.h"

struct counterlist *perf_counters_by_cpu[MAXCPU] = { 0 };
#define COUNTER_DEFINITION_DIRECTORY	"/sys/devices"
struct counterinfo *countertable = 0;
#define COUNTERTABLE_ALLOC_CHUNK 1024
int num_countertable = 0;
int num_countertable_allocated = 0;

FILE *perfctrfile = NULL;

void print_perf_counter_gnuplot_file(void);

#define MAX_COUNTERS_PER_CORE 4
struct perf_config {
  char *label;
  uint32_t type;
  uint64_t config;
};
struct core_perf_config {
  int ncount;
  struct perf_config counter[MAX_COUNTERS_PER_CORE];
} default_config[] = {
  { 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "cacheref", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES },
     { "cachemiss", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES }}},
  { 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "branch", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS },
     { "branchmiss", PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES }}},
  { 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "dtlbmiss", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_DTLB)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_MISS<<16) },
     { "itlbmiss", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_ITLB)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_MISS<<16) }}},     
  { 4,
    {{ "inst", PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS },
     { "cycle", PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES },
     { "l1dref", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_L1D)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_ACCESS<<16) },
     { "l1dmiss", PERF_TYPE_HW_CACHE,
       (PERF_COUNT_HW_CACHE_L1D)|(PERF_COUNT_HW_CACHE_OP_READ<<8)|
       (PERF_COUNT_HW_CACHE_RESULT_MISS<<16) }}}
};

struct perfctr_info {
  int fd;
  int corenum;
  struct perf_config pconfig;
  long value;
  struct perfctr_info *prev;
  struct perfctr_info *next;  
};

//static struct perfctr_info *performance_counters = NULL;

// syscall wrapper since not part of glibc
int perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
		    int cpu, int group_fd, unsigned long flags){
  int ret;
  ret = syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags);
  return ret;
}

void init_perf_counters(){
  int i,j,confignum;
  int status;
  struct perf_event_attr pe;
  struct perfctr_info *pi;
  struct perfctr_info *last_pi = NULL, *first_pi = NULL;
  struct counterlist *cl;
  perfctrfile = tmpfile();

  for (i=0;i<num_procs;i++){
    for (cl = perf_counters_by_cpu[i];cl;cl = cl->next){
      cl->ci = counterinfo_lookup(cl->name,0,0);
      if (cl->ci == NULL){
	// shouldn't fail since we already looked it up once in parsing the args...
	error("unknown performance counter, ignored: %s\n",cl->name);
      }
      cl->value = 0;
      memset(&pe,0,sizeof(pe));
      pe.type = cl->ci->type;
      pe.config = cl->ci->config;
      pe.config1 = cl->ci->config1;
      pe.size = sizeof(struct perf_event_attr);
      pe.disabled = 1;
      pe.exclude_kernel = 0;
      pe.exclude_hv = 0;
      pe.exclude_idle = 1;
      if (pe.type > 6){
	pe.exclude_idle = 0;
      }
      status = perf_event_open(&pe,-1,0,-1,0);	
      if (status == -1){
	error("unable to open performance counter pid=%d cpu=%d type=%d config=%d errno=%d %s\n",
	      -1,i,pe.type,pe.config,errno,strerror(errno));
      } else {
	cl->fd = status;
      }
    }
  }

  // walk through to start all the counters
  for (i=0;i<num_procs;i++){
    for (cl = perf_counters_by_cpu[i];cl;cl = cl->next){
      status = ioctl(cl->fd,PERF_EVENT_IOC_RESET,0);
      if (status == -1){
	error("reset of %s_%d returns -1, errno = %d %s\n",
	      cl->name,i,errno,strerror(errno));
      }
      status = ioctl(cl->fd,PERF_EVENT_IOC_ENABLE,0);
    }
  }
}

void read_perf_counters(double time){
  int status,i;
  struct counterlist *cl;
  fprintf(perfctrfile,"time %f\n",time);
  for (i=0;i<num_procs;i++){
    for (cl = perf_counters_by_cpu[i];cl;cl = cl->next){
      status = read(cl->fd,&cl->value,sizeof(cl->value));
      fprintf(perfctrfile,"%d_%s %ld\n",i,cl->name,cl->value);
      debug("%d_%s %ld\n",i,cl->name,cl->value);
    }
  }
}

void print_counter_info(int num,char *name,char *delim,FILE *output){
  int i;
  char *p;
  struct perfctr_info *pi;
  struct counter_values { long value[MAX_COUNTERS_PER_CORE]; } prev, current;
  int timeseen = 0;
  int len = strlen(name);
  char buffer[1024];
  double elapsed;
  int colnum;
  struct counterlist *cl;

  // print header row
  fprintf(output,"core%d",num);
  for (cl = perf_counters_by_cpu[num];cl;cl = cl->next){
    fprintf(output,"%s%s",delim,cl->name);
  }
  fprintf(output,"\n");

  // print values
  rewind(perfctrfile);
  colnum = 0;
  while (fgets(buffer,sizeof(buffer),perfctrfile) != NULL){
    if (!strncmp(buffer,"time",4)){
      timeseen++;
      if (timeseen > 2){
	// When we've seen "time" twice, we've seen all our first counters
	// dump the previous values
	fprintf(output,"%-10.2f",elapsed);
	for (i=0;i<colnum;i++){
	  fprintf(output,"%s%ld",delim,(current.value[i]-prev.value[i]));
	}
	fprintf(output,"\n");
      }
      // update to start collecting the next
      sscanf(buffer,"time %lf",&elapsed);
      colnum = 0;
      prev = current;
    }
    if (!strncmp(buffer,name,len)){
      debug("buffer: %s",buffer);
      p = strchr(buffer,' ');
      if (p){
	sscanf(p+1,"%ld",&current.value[colnum]);
      } else {
	current.value[colnum] = 0;
      }
      colnum++;
    }
  }
  // dump the last row
  fprintf(output,"%-10.2f",elapsed);
  for (i=0;i<colnum;i++){
    fprintf(output,"%s%ld",delim,(current.value[i]-prev.value[i]));
  }
  fprintf(output,"\n");  
}

void print_perf_counters(void){
  int i;
  char perfbuf[16];

  for (i=0;i<num_procs;i++){
    sprintf(perfbuf,"%d_",i);
    print_counter_info(i,perfbuf,"\t",outfile);
  }
}

void print_perf_counter_files(void){
  int i;
  char perfbuf[16];
  char perffile[32];
  FILE *fp;

  for (i=0;i<num_procs;i++){
    sprintf(perfbuf,"%d_",i);
    sprintf(perffile,"perf%d.csv",i);
    fp = fopen(perffile,"w");
    if (fp){
      print_counter_info(i,perfbuf,",",fp);
    }
    fclose(fp);
  }
  //  print_perf_counter_gnuplot_file();
}

void print_perf_counter_gnuplot_file(void){
  int i;
  FILE *fp = fopen("perf-gnuplot.sh","w");
  if (fp){
    fprintf(fp,"#!/bin/bash\n");
    fprintf(fp,"gnuplot <<PLOTCMD\n");
    fprintf(fp,"set terminal png\n");
    fprintf(fp,"set output 'ipcall.png'\n");
    fprintf(fp,"set title 'ALL CPU IPC'\n");
    fprintf(fp,"set datafile separator \",\"\n");
    fprintf(fp,"plot");
    for (i=0;i<num_procs;i++){
      if (i != 0) fprintf(fp,",");
      fprintf(fp," 'perf%d.csv' using 1:(\\$2/\\$3) title 'CPU %d' with linespoints",i,i);
    }
    fprintf(fp,"\n");
    fprintf(fp,"PLOTCMD\n\n");

    for (i=0;i<num_procs;i++){
      fprintf(fp,"gnuplot <<PLOTCMD\n");
      fprintf(fp,"set terminal png\n");
      fprintf(fp,"set output 'ipc%d.png'\n",i);
      fprintf(fp,"set title 'CPU %d IPC'\n",i);
      fprintf(fp,"set datafile separator \",\"\n");
      fprintf(fp,"plot 'perf%d.csv' using 1:(\\$2/\\$3) title 'IPC' with linespoints\n",i);
      fprintf(fp,"PLOTCMD\n");
      if (i%4 == 0){
	fprintf(fp,"gnuplot <<PLOTCMD\n");
	fprintf(fp,"set terminal png\n");
	fprintf(fp,"set output 'cache%d.png'\n",i);
	fprintf(fp,"set title 'CPU %d Cache Ratio'\n",i);
	fprintf(fp,"set datafile separator \",\"\n");
	fprintf(fp,"plot 'perf%d.csv' using 1:(\\$5/\\$4) title 'Miss Ratio' with linespoints\n",i);
	fprintf(fp,"PLOTCMD\n");
      } else if (i%4 == 1){
	fprintf(fp,"gnuplot <<PLOTCMD\n");
	fprintf(fp,"set terminal png\n");
	fprintf(fp,"set output 'branch%d.png'\n",i);
	fprintf(fp,"set title 'CPU %d Branch Ratio'\n",i);
	fprintf(fp,"set datafile separator \",\"\n");
	fprintf(fp,"plot 'perf%d.csv' using 1:(\\$5/\\$4) title 'Miss Ratio' with linespoints\n",i);
	fprintf(fp,"PLOTCMD\n");

	fprintf(fp,"gnuplot <<PLOTCMD\n");
	fprintf(fp,"set terminal png\n");
	fprintf(fp,"set output 'brancha%d.png'\n",i);
	fprintf(fp,"set title 'CPU %d Branch Activity'\n",i);
	fprintf(fp,"set datafile separator \",\"\n");
	fprintf(fp,"plot 'perf%d.csv' using 1:(\\$4/\\$2) title 'Branches' with linespoints, 'perf%d.csv' using 1:(\\$5/\\$2) title 'Misses' with linespoints\n",i,i);
	fprintf(fp,"PLOTCMD\n");		
      } else if (i%4 == 3){
	fprintf(fp,"gnuplot <<PLOTCMD\n");
	fprintf(fp,"set terminal png\n");
	fprintf(fp,"set output 'cacheL1D%d.png'\n",i);
	fprintf(fp,"set title 'CPU %d Cache Ratio'\n",i);
	fprintf(fp,"set datafile separator \",\"\n");
	fprintf(fp,"plot 'perf%d.csv' using 1:(\\$5/\\$4) title 'Miss Ratio' with linespoints\n",i);
	fprintf(fp,"PLOTCMD\n");

	fprintf(fp,"gnuplot <<PLOTCMD\n");
	fprintf(fp,"set terminal png\n");
	fprintf(fp,"set output 'cacheL1Da%d.png'\n",i);
	fprintf(fp,"set title 'CPU %d L1D Activity'\n",i);
	fprintf(fp,"set datafile separator \",\"\n");
	fprintf(fp,"plot 'perf%d.csv' using 1:(\\$4/\\$2) title 'Accesses' with linespoints, 'perf%d.csv' using 1:(\\$5/\\$2) title 'Misses' with linespoints\n",i,i);
	fprintf(fp,"PLOTCMD\n");			
      }
    }
    fchmod(fileno(fp),0755);
    fclose(fp);
  }
}

struct counterinfo *counterinfo_lookup(char *name,char *group,int insert){
  int i;
  int multiple = 0;
  for (i=0;i<num_countertable;i++){
    if (countertable[i].name && !strcmp(name,countertable[i].name)){
      // multiples match if the groups match or are null
      if (countertable[i].is_multiple){
	if ((group == NULL)||(countertable[i].group == NULL)||
	    !strcmp(group,countertable[i].group)){
	  return &countertable[i];
	} else {
	  // mark the multiple field on the original, and for later insertion
	  countertable[i].is_multiple = 1;
	  multiple = 1;
	  continue;
	}
      } else {
	// not multiple (yet), but check if this situation applies
	if (group && countertable[i].group &&
	    (strcmp(group,countertable[i].group) != 0)){
	  // found a multiple, so mark it...
	  countertable[i].is_multiple = 1;
	  multiple = 1;
	} else {
	  return &countertable[i];
	}
      }
    }
  }
  if (insert == 0) return NULL;
  if (num_countertable_allocated == 0){
    num_countertable_allocated = COUNTERTABLE_ALLOC_CHUNK;
    countertable = calloc(num_countertable_allocated,sizeof(struct counterinfo));
    num_countertable = 1;
    return &countertable[0];
  } else if (num_countertable >= num_countertable_allocated){
    num_countertable_allocated += COUNTERTABLE_ALLOC_CHUNK;
    // NOTE: Use of realloc() means the address of counter objects can change!
    // OK for now since the building phase happens distinctly from the using
    // phase, but latent bug if this changes.
    countertable = realloc(countertable,sizeof(struct counterinfo));
    memset(&countertable[num_countertable],'\0',sizeof(struct counterinfo)*COUNTERTABLE_ALLOC_CHUNK);
  }
  num_countertable++;
  countertable[num_countertable-1].is_multiple = multiple;
  return &countertable[num_countertable-1];
}

void display_counter(struct counterinfo *ci,FILE *fp){
  if (ci->is_multiple){
    fprintf(fp,"%16s.%-13s",ci->group,ci->name);
  } else {
    fprintf(fp,"%30s",ci->name);
  }
  fprintf(fp," type=%-2d",ci->type);
  fprintf(fp," config=0x%.8lx",ci->config);
  if (ci->config1)
    fprintf(fp," config1=0x%.8lx",ci->config1);  
  fprintf(fp,"\n");
}

int compare_counters(const void *c1,const void *c2){
  const struct counterinfo *c_one = c1,*c_two = c2;
  int result;
  if ((result = strcmp(c_one->group,c_two->group)) < 0){
    return -1;
  } else if (result > 0){
    return 1;
  } else if ((result = strcmp(c_one->name,c_two->name)) < 0){
    return -1;
  } else if (result > 0){
    return 1;
  } else return 0;
}

void sort_counters(void){
  qsort(countertable,num_countertable,sizeof(struct counterinfo),compare_counters);
}

void print_counters(FILE *fp){
  int i;
  for (i=0;i<num_countertable;i++){
    display_counter(&countertable[i],fp);
  }
}

void add_counterinfo(char *dir,char *name,char *group,int type){
  FILE *fp;
  char filename[1024];
  char line[1024];
  char *field;
  uint64_t value;
  struct counterinfo *ci;
  debug("add counter (%d): %s/%s\n",type,dir,name);
  snprintf(filename,sizeof(filename),"%s/%s",dir,name);
  if (fp = fopen(filename,"r")){
    if (fgets(line,sizeof(line),fp) != NULL){
      ci = counterinfo_lookup(name,group,1);
      ci->type = type;
      ci->name = strdup(name);
      ci->group = strdup(group);
      ci->directory = strdup(dir);
      for (field = strtok(line,",\n");field;field = strtok(NULL,",\n")){
	// NOTE: Field placements are "hardwired" rather than parsing the
	// "format" directories. I've checked that this doesn't cause issues
	// for my supported platforms noted below, but this needs to be fixed
	// for a more general solution.
	if (!strncmp(field,"event=",6)){
	  if (sscanf(&field[6],"%lx",&value) == 1){
	    ci->config |= value;
	    continue;
	  }
	} else if (!strncmp(field,"umask=",6)){
	  if (sscanf(&field[6],"%lx",&value) == 1){
	    ci->config |= (value << 8);
	    continue;
	  }
	} else if (!strncmp(field,"cmask=",6)){
	  if (sscanf(&field[6],"%lx",&value) == 1){
	    ci->config |= (value << 24);
	  }
	} else if (!strncmp(field,"any=",4)){
	  if (sscanf(&field[4],"%lx",&value) == 1){
	    ci->config |= (value << 21);
	  }
	} else if (!strncmp(field,"in_tx=",6)){
	  if (sscanf(&field[6],"%lx",&value) == 1){
	    ci->config |= (value << 32);
	  }
	} else if (!strncmp(field,"in_tx_cp=",9)){
	  if (sscanf(&field[9],"%lx",&value) == 1){
	    ci->config |= (value << 33);
	  }	  
	} else if (!strncmp(field,"ldlat=",6)){
	  if (sscanf(&field[6],"%lx",&value) == 1){
	    ci->config1 |= value;
	  }
	} else if (!strncmp(field,"csource=",8)){
	  if (sscanf(&field[8],"%lx",&value) == 1){
	    ci->config |= value;
	  }
	} else if (!strncmp(field,"result=",7)){
	  if (sscanf(&field[8],"%lx",&value) == 1){
	    ci->config |= (value << 16);	  
	}
	} else {
	  // So far only implemented:
	  //   Intel: i7-4770 and C2750
	  //   AMD:   A10-7850 and A4-5000
	  warning("unimplemented field: %s\n",field);
	}
      }
    }
    fclose(fp);
  }
}

void inventory_counters(char *directory){
  DIR *counterdir;
  DIR *eventdir;
  FILE *fp;
  int lasttype = -1;
  struct dirent *dec,*dee;
  char buffer[1024];
  char buffer_counter[1024];
  char *p;
  int status;
  struct stat statbuf;
  if (directory == NULL) directory = COUNTER_DEFINITION_DIRECTORY;
  counterdir = opendir(directory);
  if (counterdir){
    while (dec = readdir(counterdir)){
      if (!strcmp(dec->d_name,".") || !strcmp(dec->d_name,"..")) continue;
      // look for a "type" file
      snprintf(buffer,sizeof(buffer),"%s/%s/type",directory,dec->d_name);
      status = stat(buffer,&statbuf);
      if ((status == -1) || !S_ISREG(statbuf.st_mode)){ continue; }
      fp = fopen(buffer,"r");
      if (fp){
	if (fscanf(fp,"%d",&lasttype) != 1){
	  fclose(fp);
	  continue;
	}
      } else {
	continue;
      }
      // verify an "events" directory
      snprintf(buffer,sizeof(buffer),"%s/%s/events",directory,dec->d_name);
      status = stat(buffer,&statbuf);
      if ((status == -1) || !S_ISDIR(statbuf.st_mode)){	continue; }

      // read contents of the events directory
      eventdir = opendir(buffer);
      while (dee = readdir(eventdir)){
	if (!strcmp(dee->d_name,".") || !strcmp(dee->d_name,"..")) continue;
	// ignore the scale and unit files for now
	if (p = strstr(dee->d_name,".scale")) continue;
	if (p = strstr(dee->d_name,".unit")) continue;
	add_counterinfo(buffer,dee->d_name,dec->d_name,lasttype);
      }
      closedir(eventdir);
    }
    closedir(counterdir);
  }
}
