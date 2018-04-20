/*
 * process_csv - read and format data from the "processtree.csv" file.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "error.h"

#define NUM_COUNTERS_PER_PROCESS 6 // hard coded format for now

char *input_filename = "processtree.csv";
char *format_specifier = 0;
int mflag = 0;
int pid_root = -1;
static int clocks_per_second = 0;
static int num_procs = 0;

int parse_options(int argc,char *const argv[]){
  int opt;
  while ((opt = getopt(argc,argv,"f:F:mp:")) != -1){
    switch(opt){
    case 'f':
      input_filename = strdup(optarg);
      break;
    case 'F':
      format_specifier = strdup(optarg);
      break;
    case 'm':
      mflag = 1;
      break;
    case 'p':
      if (sscanf(optarg,"%d",&pid_root) != 1){
	error("bad argument to -p: %s\n",optarg);
	return 1;
      }
      break;
    default:
      return 1;
      break;
    }
  }
  return 0;
}

char *signature = "#pid,ppid,filename,starttime,start,finish,cpu,utime,stime,vsize,rss,minflt,majflt,num_counters";

struct process_info {
  struct process_info *parent,*sibling,*child1,*childn;
  pid_t pid,ppid;
  int cpu,num_counters;
  char *filename;
  unsigned long utime,stime,vsize,rss,minflt,majflt;
  unsigned long long starttime;
  double start,finish;
  unsigned long counter[NUM_COUNTERS_PER_PROCESS];
  // derived information
  int level;
  int nproc;
  unsigned long total_utime,total_stime,total_minflt,total_majflt;
  unsigned long total_counter[NUM_COUNTERS_PER_PROCESS];
};

struct process_info *process_table;
int process_table_size = 0;
int process_table_allocated = 0;

void add_process_info(struct process_info *pi){
  static int chunksize = 1024;
  if (process_table_allocated == 0){
    process_table = malloc(chunksize*sizeof(struct process_info));
    if (process_table == 0){
      fatal("memory allocation failed\n");
    }
    process_table_allocated = chunksize;
  } else if (process_table_size >= process_table_allocated){
    chunksize *= 2;
    process_table = realloc(process_table,chunksize*sizeof(struct process_info));
    if (process_table == 0){
      fatal("memory allocation failed\n");
    }
    process_table_allocated = chunksize;
  }
  process_table[process_table_size] = *pi;
  process_table_size++;
}

int read_input_file(void){
  FILE *fp;
  char line[1024],filename[64];
  int lineno = 0;
  char *token;
  struct process_info pi;
  if (fp = fopen(input_filename,"r")){
    while (fgets(line,sizeof(line),fp) != NULL){
      lineno++;
      if (line[0] == '#'){
	if (strncmp(line,signature,strlen(signature)) != 0){
	  // either because it isn't a csv file or the format has changed
	  // format has to match so hard coded sequence below works
	  error("file does not match signature\n");
	  return 1;
	}
      } else {
	// parse according to the signature above
	memset(&pi,'\0',sizeof(pi)); // just in case
	token = strtok(line,",\n"); // pid
	if (token){
	  sscanf(token,"%d",&pi.pid);
	}
	token = strtok(NULL,",\n"); // ppid
	if (token){
	  sscanf(token,"%d",&pi.ppid);
	}
	token = strtok(NULL,",\n"); // filename
	if (token){
	  sscanf(token,"%63s",filename);
	  pi.filename = strdup(filename);
	}
	token = strtok(NULL,",\n"); // starttime
	if (token){
	  sscanf(token,"%llu",&pi.starttime);
	}
	token = strtok(NULL,",\n"); // start
	if (token){
	  sscanf(token,"%lf",&pi.start);
	}
	token = strtok(NULL,",\n"); // finish
	if (token){
	  sscanf(token,"%lf",&pi.finish);	  
	}
	token = strtok(NULL,",\n"); // cpu
	if (token){
	  sscanf(token,"%d",&pi.cpu);
	}
	token = strtok(NULL,",\n"); // utime
	if (token){
	  sscanf(token,"%lu",&pi.utime);	  
	}
	token = strtok(NULL,",\n"); // stime
	if (token){
	  sscanf(token,"%lu",&pi.stime);	  
	}	
	token = strtok(NULL,",\n"); // vsize
	if (token){
	  sscanf(token,"%lu",&pi.vsize);	  
	}		
	token = strtok(NULL,",\n"); // rss
	if (token){
	  sscanf(token,"%lu",&pi.rss);	  
	}			
	token = strtok(NULL,",\n"); // minflt
	if (token){
	  sscanf(token,"%lu",&pi.minflt);	  
	}				
	token = strtok(NULL,",\n"); // majflt
	if (token){
	  sscanf(token,"%lu",&pi.majflt);	  
	}					
	token = strtok(NULL,",\n"); // num_counters
	if (token){
	  sscanf(token,"%d",&pi.num_counters);	  
	} else {
	  warning("num_counters missing\n");
	}
	token = strtok(NULL,",\n"); // counter0
	if (token){
	  sscanf(token,"%lu",&pi.counter[0]);	  
	}
	token = strtok(NULL,",\n"); // counter1
	if (token){
	  sscanf(token,"%lu",&pi.counter[1]);	  
	}	
	token = strtok(NULL,",\n"); // counter2
	if (token){
	  sscanf(token,"%lu",&pi.counter[2]);	  
	}	
	token = strtok(NULL,",\n"); // counter3
	if (token){
	  sscanf(token,"%lu",&pi.counter[3]);	  
	}	
	token = strtok(NULL,",\n"); // counter4
	if (token){
	  sscanf(token,"%lu",&pi.counter[4]);	  
	}	
	token = strtok(NULL,",\n"); // counter5
	if (token){
	  sscanf(token,"%lu",&pi.counter[5]);	  
	} else {
	  warning("unable to parse line #%d\n",lineno);
	  return 1;
	}
	add_process_info(&pi);
      }
    }
    fclose(fp);
  } else {
    return 1;
  }
  return 0;
}

int compare_start(const void *pi1,const void *pi2){
  const struct process_info *pi_one = pi1;
  const struct process_info *pi_two = pi2;
  if (pi_one->starttime < pi_two->starttime)
    return -1;
  else if (pi_one->starttime > pi_two->starttime)
    return 1;
  else if (pi_one->pid < pi_two->pid)
    return -1;
  else if (pi_one->pid > pi_two->pid)
    return 1;
  else
    return 0;
}

void add_tree_links(void){
  int i,j;
  for (i=0;i<process_table_size;i++){
    // O(n^2) walk through entire table looking for parents and siblings
    for (j=i-1;j>=0;j--){
      if (process_table[i].ppid == process_table[j].pid){
	process_table[i].parent = &process_table[j];
	if (process_table[j].child1 == NULL){
	  // record the first child
	  process_table[j].child1 = &process_table[i];
	} else {
	  // give the last child a sibling
	  process_table[j].childn->sibling = &process_table[i];
	}
	process_table[j].childn = &process_table[i];
	break;
      }
    }
  }
}

void create_tree_totals(struct process_info *pi,int level){
  int i;
  struct process_info *child;
  int nproc;
  unsigned long total_utime,total_stime,total_minflt,total_majflt;
  unsigned long total_counter[NUM_COUNTERS_PER_PROCESS];
  nproc = 1;
  total_utime = pi->utime;
  total_stime = pi->stime;
  total_minflt = pi->minflt;
  total_majflt = pi->majflt;
  for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++) total_counter[i] = pi->counter[i];
  pi->level = level;
  for (child=pi->child1;child;child=child->sibling){
    create_tree_totals(child,level+1);
    nproc += child->nproc;
    total_utime += child->total_utime;
    total_stime += child->total_stime;
    total_minflt += child->total_minflt;
    total_majflt += child->total_majflt;
    for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++)
      total_counter[i] += child->total_counter[i];    
  }
  pi->nproc = nproc;
  pi->total_utime = total_utime;
  pi->total_stime = total_stime;
  pi->total_minflt = total_minflt;
  pi->total_majflt = total_majflt;
  for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++)
    pi->total_counter[i] = total_counter[i];
}

// look up vendor from /proc/cpuinfo
static char *vendor_id = NULL;
enum processor_type { PROCESSOR_UNKNOWN, PROCESSOR_INTEL, PROCESSOR_AMD } proctype = PROCESSOR_UNKNOWN;
char *lookup_vendor(void){
  FILE *fp;
  char line[1024];
  char *p,*token;
  if (vendor_id == NULL){
    if (fp = fopen("/proc/cpuinfo","r")){
      while (fgets(line,sizeof(line),fp)){
	if (!strncmp(line,"vendor_id",9)){
	  p = strchr(line,':');
	  if ((p = strchr(line,':')) &&
	      (token = strtok(p+1," \t\n"))){
	    vendor_id = strdup(token);
	    if (!strcmp(vendor_id,"GenuineIntel")){
	      proctype = PROCESSOR_INTEL;
	    } else if (!strcmp(vendor_id,"AuthenticAMD")){
	      proctype = PROCESSOR_AMD;
	    }
	  }
	  fclose(fp);
	  return vendor_id;
	}
      }
      fclose(fp);
    }
  }
  return vendor_id;
}


void print_procinfo(struct process_info *pi,int print_children,int indent,double basetime){
  struct process_info *child;
  int i;
  char *p;
  double on_core;
  double on_cpu;
  if (indent){
    for (i=0;i<pi->level;i++) printf("  ");
  }
  printf("%d)",pi->pid);
  if (pi->filename)
    printf(" %s",pi->filename);
  else
    printf(" <none>");
  if (format_specifier){
    for (p=format_specifier;*p;p++){
      switch(*p){
      case 'c':
	printf(" cpu=%d",pi->cpu);
	break;
      case 'f':
	printf(" minflt=%lu majflt=%lu",pi->total_minflt,pi->total_majflt);
	break;
      case 'i':
	if (proctype == PROCESSOR_INTEL){
	  // slots rather than CPU cycles so multiply by 2
	  printf(" ipc=%3.2f",(double) pi->counter[0] / pi->counter[1] * 2);
	} else if (proctype == PROCESSOR_AMD){
	  printf(" ipc=%3.2f",(double) pi->counter[0] / pi->counter[1]);	  
	}
	break;
      case 'I':
	printf(" tipc=%3.2f",(double) pi->total_counter[0] / pi->total_counter[1] * 2);	
	break;
      case 'm':
	if (clocks_per_second == 0)
	  clocks_per_second = sysconf(_SC_CLK_TCK);
	if (num_procs == 0)
	  num_procs = get_nprocs();
	on_core = (double)(pi->total_utime + pi->total_stime) /
	  clocks_per_second /
	  (pi->finish-pi->start);
	on_cpu = on_core / num_procs;
	printf(" on_cpu=%4.3f on_core=%4.3f",on_cpu,on_core);
	break;
      case 'n':
	printf(" pcount=%d",pi->nproc);
	break;
      case 'p':
	printf(" ctr0=%lu ctr1=%lu ctr2=%lu ctr3=%lu ctr4=%lu ctr5=%lu",
	       pi->counter[0],pi->counter[1],pi->counter[2],
	       pi->counter[3],pi->counter[4],pi->counter[5]);
	break;
      case 'P':
	printf(" tctr0=%lu tctr1=%lu tctr2=%lu tctr3=%lu tctr4=%lu tctr5=%lu",
	       pi->total_counter[0],pi->total_counter[1],pi->total_counter[2],
	       pi->total_counter[3],pi->total_counter[4],pi->total_counter[5]);
	break;	
      case 't':
	printf(" elapsed=%3.2f start=%3.2f finish=%3.2f",pi->finish-pi->start,pi->start-basetime,pi->finish-basetime);
	break;
      case 'u':
	if (clocks_per_second == 0)
	  clocks_per_second = sysconf(_SC_CLK_TCK);
	printf(" user=%3.2f sys=%3.2f",
	       (double)pi->utime / clocks_per_second,
	       (double)pi->stime / clocks_per_second);
	break;
      case 'v':
	printf(" vsize=%luK", pi->vsize/1024);
	break;
      }
    }
  }
  
  printf("\n");
  if (print_children){
    for (child = pi->child1;child;child=child->sibling){
      print_procinfo(child,print_children,indent,basetime);
    }
  }
}

void print_metrics(struct process_info *pi){
  double on_cpu,on_core;
  double td_retire,td_spec,td_frontend,td_backend;
  if (clocks_per_second == 0)
    clocks_per_second = sysconf(_SC_CLK_TCK);
  if (num_procs == 0)
    num_procs = get_nprocs();
  on_core = (double)(pi->total_utime + pi->total_stime) /
    clocks_per_second /
    (pi->finish-pi->start);
  on_cpu = on_core / num_procs;
  if (proctype == PROCESSOR_INTEL){
    td_retire = (double) pi->total_counter[5]/pi->total_counter[1];
    td_spec = (double) (pi->total_counter[4] - pi->total_counter[5] + pi->total_counter[3])/pi->total_counter[1];
    td_frontend = (double) pi->total_counter[2] / pi->total_counter[1];
    td_backend = 1 - (td_retire + td_spec + td_frontend);
  } else if (proctype == PROCESSOR_AMD){
    td_frontend = (double) pi->total_counter[2] / pi->total_counter[1];
    td_backend = (double) pi->total_counter[3] / pi->total_counter[1];
  }
  printf("%s - pid %d\n",pi->filename,pi->pid);
  printf("\tOn_CPU   %4.3f\n",on_cpu);
  printf("\tOn_Core  %4.3f\n",on_core);
  printf("\tIPC      %4.3f\n",(double) pi->total_counter[0] / pi->total_counter[1] * 2);
  if (proctype == PROCESSOR_INTEL){
    printf("\tRetire   %4.3f\t(%3.1f%%)\n",td_retire,td_retire*100);
    printf("\tFrontEnd %4.3f\t(%3.1f%%)\n",td_frontend,td_frontend*100);
    printf("\tSpec     %4.3f\t(%3.1f%%)\n",td_spec,td_spec*100);
    printf("\tBackend  %4.3f\t(%3.1f%%)\n",td_backend,td_backend*100);
  } else if (proctype == PROCESSOR_AMD){
    printf("\tFrontEnd %4.3f\t(%3.1f%%)\n",td_frontend,td_frontend*100);
    printf("\tBackend  %4.3f\t(%3.1f%%)\n",td_backend,td_backend*100);    
  }
  printf("\tElapsed  %5.2f\n",pi->finish-pi->start);
  printf("\tProcs    %d\n",pi->nproc);
  printf("\tMinflt   %lu\n",pi->total_minflt);
  printf("\tMajflt   %lu\n",pi->total_majflt);
  printf("\tUtime    %-8.2f\t(%3.1f%%)\n",
	 (double) pi->total_utime / clocks_per_second,
	 (double) pi->total_utime * 100.0 / (pi->total_utime + pi->total_stime));
  printf("\tStime    %-8.2f\t(%3.1f%%)\n",
	 (double) pi->total_stime / clocks_per_second,
	 (double) pi->total_stime * 100.0 / (pi->total_utime + pi->total_stime));
  printf("\tStart    %4.2f\n",pi->start);
  printf("\tFinish   %4.2f\n",pi->finish);
}

int main(int argc,char *const argv[],char *const envp[]){
  int i;
  if (parse_options(argc,argv)){
    fatal("usage: %s [-m][-f filename][-F format][-p pid]\n"
	  "\t-f sets input filename (default processtree.csv)\n"
	  "\t-F is a string of format specifiers:\n"
	  "\t   c - core last run\n"
	  "\t   f - minor and major faults\n"
	  "\t   i - ipc of this process\n"
	  "\t   I - cummulative IPC for process tree\n"
	  "\t   m - On_CPU and On_Core metrics\n"
	  "\t   n - number of processes in tree\n"
	  "\t   p - counters for this process\n"
	  "\t   P - counters for the tree\n"
	  "\t   t - time: elapsed, start and finish\n"
	  "\t   u - user and system times\n"
	  "\t   v - virtual memory sizes\n"
	  "\t-m provides summary metrics\n"
	  "\t-p selects pid to print\n"
	  ,argv[0]);
  }
  if (read_input_file()){
    fatal("unable to read input file: %s\n",input_filename);
  }
  if (lookup_vendor() == NULL){
    fatal("unable to read vendor information\n");
  }
  qsort(process_table,process_table_size,sizeof(struct process_info),compare_start);

  // add parent relationships
  add_tree_links();

  // sum up aggregates
  for (i=0;i<process_table_size;i++){
    // only orphans
    if (process_table[i].parent == NULL){
      create_tree_totals(&process_table[i],0);
    }
  }

  if (pid_root >= 0){
    int found = 0;
    for (i=0;i<process_table_size;i++){
      if (process_table[i].pid == pid_root){
	if (mflag){
	  print_metrics(&process_table[i]);
	} else {
	  print_procinfo(&process_table[i],1,1,process_table[i].start);
	}
	found = 1;
	break;
      }
    }
    if (!found) warning("pid %d not found\n",pid_root);
  } else {
    for (i=0;i<process_table_size;i++){
      if (process_table[i].parent == NULL){
	// print entire trees
	if (mflag){
	  print_metrics(&process_table[i]);	  
	} else {
	  print_procinfo(&process_table[i],1,1,process_table[i].start);
	}
      }
    }
  }
  return 0;
}
