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
int all_counters_same = 0;
struct counterlist *perf_counters_same = NULL;
struct counterlist *perf_counters_by_process[NUM_COUNTERS_PER_PROCESS] = { 0 };
#define COUNTER_DEFINITION_DIRECTORY	"/sys/devices"
struct counterinfo *countertable = 0;
#define COUNTERTABLE_ALLOC_CHUNK 1024
int num_countertable = 0;
int num_countertable_allocated = 0;

FILE *perfctrfile = NULL;

void print_perf_counter_gnuplot_file(void);

#define MAX_COUNTERS_PER_CORE 6

struct perf_config {
  char *label;
  uint32_t type;
  uint64_t config;
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

void init_global_perf_counters(){
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
      pe.inherit = 1;
      pe.sample_type = PERF_SAMPLE_IDENTIFIER;
      pe.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED|PERF_FORMAT_TOTAL_TIME_RUNNING;
      pe.size = sizeof(struct perf_event_attr);
      pe.disabled = 1;
      pe.exclude_kernel = 0;
      pe.exclude_hv = 0;
      pe.exclude_idle = 0;
      if (pe.type > 6){
	pe.exclude_idle = 0;
      }
      status = perf_event_open(&pe,-1,i,-1,0);	
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

// for now just measure IPC
int default_process_counters[] = {
  PERF_COUNT_HW_INSTRUCTIONS,
  PERF_COUNT_HW_CPU_CYCLES,
};

char *def_process_counters_intel[] = {
  "instructions",
  "topdown-total-slots",
  "topdown-fetch-bubbles",
  "topdown-recovery-bubbles",
  "topdown-slots-issued",
  "topdown-slots-retired"
};
char *def_process_counters_amd[] = {
  "instructions",
  "cpu-cycles",
  "stalled-cycles-frontend",
  "stalled-cycles-backend",
};

void init_process_counterinfo(void){
  int i;
  char *vendor;
  struct counterlist *cl;
  char **default_counters;
  int num_counters;

  if (perf_counters_same){
    // --set-counters option given
    i = 0;
    for (cl = perf_counters_same;cl;cl = cl->next){
      cl->ci = counterinfo_lookup(cl->name,0,0);
      if (cl->ci == NULL){
	error("unknown performance counter, ignored: %s\n",cl->name);
	continue;
      }
      if (i >= NUM_COUNTERS_PER_PROCESS){
	warning("maximum %d counters, ignored: %s\n",NUM_COUNTERS_PER_PROCESS,cl->name);
      } else {
	perf_counters_by_process[i] = cl;
	i++;
      }
    }
  } else {
    vendor = lookup_vendor();
    if (vendor && !strcmp(vendor,"GenuineIntel")){
      default_counters = def_process_counters_intel;
      num_counters = sizeof(def_process_counters_intel)/sizeof(def_process_counters_intel[0]);
    } else if (vendor && !strcmp(vendor,"AuthenticAMD")){
      default_counters = def_process_counters_amd;
      num_counters = sizeof(def_process_counters_amd)/sizeof(def_process_counters_amd[0]);
    }
    else {
      return;
    }
    for (i=0;i<num_counters;i++){
      cl = calloc(1,sizeof(struct counterlist));
      cl->name = default_counters[i];
      cl->ci = counterinfo_lookup(cl->name,0,0);
      if (cl->ci == NULL){
	error("unknown performance counter, ignored: %s\n",cl->name);
      } else {
	perf_counters_by_process[i] = cl;
      }
    }
  }
}

void start_process_perf_counters(pid_t pid,struct process_counter_info *pci,int root){
  int i;
  struct perf_event_attr pe[NUM_COUNTERS_PER_PROCESS];
  
  if (pci){
    memset(pe,'\0',sizeof(pe));
    for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++){
      if (perf_counters_by_process[i] == 0) continue;
      pe[i].type = perf_counters_by_process[i]->ci->type;
      pe[i].config = perf_counters_by_process[i]->ci->config;
      pe[i].config1 = perf_counters_by_process[i]->ci->config1;
      pe[i].size = sizeof(struct perf_event_attr);
      if (perfcounter_model == PM_APPLICATION){
	if (root) pe[i].inherit = 1;
	else break;
      }
      pci->perf_fd[i] = perf_event_open(&pe[i],pid,-1,-1,0);
      if (pci->perf_fd[i] != -1){
	ioctl(pci->perf_fd[i],PERF_EVENT_IOC_RESET,0);
	ioctl(pci->perf_fd[i],PERF_EVENT_IOC_ENABLE,0);
      }
      debug("start counter %d for pid %d (fd=%d)\n",i,pid,pci->perf_fd[i]);
    }
  }
}

void read_global_perf_counters(double time){
  int status,i;
  struct counterlist *cl,*cl_same;
  struct read_format { uint64_t value, time_enabled,time_running,id; } rf;
  fprintf(perfctrfile,"time %f\n",time);
  if (all_counters_same){
    for (cl_same = perf_counters_same;cl_same;cl_same = cl_same->next){
      cl_same->value = 0;
    }
  }
  for (i=0;i<num_procs;i++){
    cl_same = perf_counters_same;
    for (cl = perf_counters_by_cpu[i];cl;cl = cl->next){
      status = read(cl->fd,&rf,sizeof(rf));
      cl->value = rf.value * (( double) rf.time_enabled / rf.time_running);
      if (cl_same) cl_same->value += cl->value;
      fprintf(perfctrfile,"%d_%s %lu\n",i,cl->name,cl->value);
      debug("%d_%s %lu (%lu * %lu / %lu)\n",i,cl->name,cl->value,rf.value,rf.time_enabled,rf.time_running);
      if (cl_same) cl_same = cl_same->next;
    }
  }
  for (cl_same = perf_counters_same;cl_same;cl_same=cl_same->next){
    fprintf(perfctrfile,"total_%s %lu\n",cl_same->name,cl_same->value);
    debug("total_%s %lu\n",cl_same->name,cl_same->value);        
  }
}

void stop_process_perf_counters(pid_t pid,struct process_counter_info *pci){
  int status;
  int i;
  if (pci){
    for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++){
      if (pci->perf_fd[i] > 0){
	status = read(pci->perf_fd[i],&pci->perf_counter[i],
		      sizeof(pci->perf_counter[i]));
	close(pci->perf_fd[i]);
	debug("stop counter %d for pid %d\n",i,pid);	
      }
    }
  }
}

void print_counter_info(int num,char *name,char *delim,FILE *output){
  int i;
  char *p;
  struct perfctr_info *pi;
  struct counter_values { unsigned long value[MAX_COUNTERS_PER_CORE]; } prev, current;
  int timeseen = 0;
  int len = strlen(name);
  char buffer[1024];
  double elapsed;
  int colnum;
  int column_scale[MAX_COUNTERS_PER_CORE];
  struct counterlist *cl;

  // print header row
  fprintf(output,"core%d",num);
  colnum = 0;
  for (cl = perf_counters_by_cpu[num];cl;cl = cl->next){
    fprintf(output,"%s%s",delim,cl->name);
    column_scale[colnum] = cl->ci->scale;
    colnum++;
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
	  if (column_scale[i]){
	    fprintf(output,"%s%lu",delim,
		    column_scale[i]*(current.value[i]-prev.value[i]));
	  } else {
	    fprintf(output,"%s%lu",delim,(current.value[i]-prev.value[i]));
	  }
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
	sscanf(p+1,"%lu",&current.value[colnum]);
      } else {
	current.value[colnum] = 0;
      }
      colnum++;
    }
  }
  // dump the last row
  fprintf(output,"%-10.2f",elapsed);
  for (i=0;i<colnum;i++){
    if (column_scale[i]){
      fprintf(output,"%s%ld",delim,column_scale[i]*(current.value[i]-prev.value[i]));      
    } else {
      fprintf(output,"%s%ld",delim,(current.value[i]-prev.value[i]));
    }
  }
  fprintf(output,"\n");

  // dump the totals
  fprintf(output,"#totals");
  for (i=0;i<colnum;i++){
    fprintf(output,"%s%lu",delim,current.value[i]);
  }
  fprintf(outfile,"\n");
}

void print_global_perf_counters(void){
  int i;
  char perfbuf[16];

  for (i=0;i<num_procs;i++){
    sprintf(perfbuf,"%d_",i);
    print_counter_info(i,perfbuf,"\t",outfile);
  }
}

void print_global_perf_counter_files(void){
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
  if (all_counters_same){
    fp = fopen("perftotal.csv","w");
    if (fp) print_counter_info(0,"total_",",",fp);
  }
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
	  if (sscanf(&field[7],"%lx",&value) == 1){
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
  snprintf(filename,sizeof(filename),"%s/%s.scale",dir,name);
  if (fp = fopen(filename,"r")){
    int c;
    int scale = 0;
    int len,digits;
    if (fgets(line,sizeof(line),fp)){ // only deal with integers for now
      len = strlen(line);
      digits = strspn(line,"0123456789\n");
      if (len == digits){
	sscanf(line,"%d",&scale);
	if (scale){
	  ci->scale = scale;
	}
      }
      fclose(fp);
      ci->scale = scale;
    }
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
	fclose(fp);
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
