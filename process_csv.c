/*
 * process_csv - read and format data from the "processtree.csv" file.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include "error.h"

#define NUM_COUNTERS_PER_PROCESS 6 // hard coded format for now

char *input_filename = "processtree.csv";
char *format_specifier = 0;

int parse_options(int argc,char *const argv[]){
  int opt;
  while ((opt = getopt(argc,argv,"f:F:")) != -1){
    switch(opt){
    case 'f':
      input_filename = strdup(optarg);
      break;
    case 'F':
      format_specifier = strdup(optarg);
      break;
    default:
      warning("unknown option: %d\n",opt);
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
  struct process_info *child;
  pi->level = level;
  for (child=pi->child1;child;child=child->sibling){
    create_tree_totals(child,level+1);
  }
}

void print_procinfo(struct process_info *pi,int print_children,int indent,double basetime){
  static int clocks_per_second = 0;
  struct process_info *child;
  int i;
  char *p;
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
	// TODO make this cummulative
	printf(" minflt=%lu majflt=%lu",pi->minflt,pi->majflt);
	break;
      case 'i':
	printf(" ipc=%3.2f",(double) pi->counter[0] / pi->counter[1] * 2);
	break;
      case 'I':
	// TODO print cummulative IPC
	break;
      case 'm':
	// TODO print On_CPU and On_Core metrics
	break;
      case 'n':
	// TODO print number of processes in tree
	break;
      case 'p':
	printf(" ctr0=%lu ctr1=%lu ctr2=%lu ctr3=%lu ctr4=%lu ctr5=%lu",
	       pi->counter[0],pi->counter[1],pi->counter[2],
	       pi->counter[3],pi->counter[4],pi->counter[5]);
	break;
      case 'P':
	// TODO print cummulative counters
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

int main(int argc,char *const argv[],char *const envp[]){
  int i;
  if (parse_options(argc,argv)){
    fatal("usage: %s [-f filename][-F format]\n"
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
	  ,argv[0]);
  }
  if (read_input_file()){
    fatal("unable to read input file: %s\n",input_filename);
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

  for (i=0;i<process_table_size;i++){
    if (process_table[i].parent == NULL){
      // print entire trees
      print_procinfo(&process_table[i],1,1,process_table[i].start);
    }
  }
  return 0;
}
