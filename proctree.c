/*
 * proctree - process the output format from topdown --tree
 *
 * This includes the following four lines:
 *
 * <time> root <pid>
 * <time> start <pid> <ppid>
 * <time> exit <pid> </proc/<pid>/stat
 * <time> comm <pid> </proc/pid/comm>
 */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include "error.h"

FILE *process_file = NULL;
int treeflag = 1;
int statflag = 1;
int mflag = 0;
int uflag = 0;
int vflag = 0;
int fflag = 1;
int output_width = 80;
int print_cmdline = 0;
int clocks_per_second;

/* process_info - maintained for each process */
struct process_info {
  unsigned int pid;
  char *comm; // short version of the command
  char *cmdline; // long command line
  double start,finish;
  // selected data from /proc/<pid>/stat
  unsigned int utime;
  unsigned int stime;
  unsigned long long starttime;
  unsigned int vsize;
  unsigned int processor;
  unsigned int exit_code;
  struct process_info *parent;
  struct process_info *children; // linked using the "sibling" relationship
  struct process_info *older_sibling; // elder sibling
  struct process_info *younger_sibling; // younger sibling
};

struct process_info *root_process = NULL;

/* proc_table - table of currently running processes
 *    Note: Processes are removed once they exit since the pid can be reused.
 */
#define NUM_PROC_TABLE_BUCKETS 101
int num_processes = 0;
int total_processes = 0;
int max_num_processes = 0;
struct proc_table_entry {
  unsigned int pid;
  struct process_info *pinfo;
  struct proc_table_entry *prev,*next;
};
struct proc_table_entry *proc_table[NUM_PROC_TABLE_BUCKETS] = { 0 };

// lookup an entry and insert if necessary
struct proc_table_entry *lookup_pid(unsigned int pid,int insert){
  int bucket = pid % NUM_PROC_TABLE_BUCKETS;
  struct proc_table_entry *pentry;

  for (pentry=proc_table[bucket];pentry;pentry=pentry->next){
    if (pentry->pid == pid) return pentry;
  }
  // not found
  if (!insert) return NULL;

  pentry = calloc(1,sizeof(struct proc_table_entry));
  pentry->pid = pid;
  pentry->pinfo = calloc(1,sizeof(struct process_info));
  pentry->pinfo->pid = pid;
  if (proc_table[bucket]){
    pentry->next = proc_table[bucket];
    pentry->next->prev = pentry;
  }
  proc_table[bucket] = pentry;
  num_processes++;
  total_processes++;
  if (num_processes > max_num_processes)
    max_num_processes = num_processes;
  return pentry;
}

// remove an entry, return 0 if successful
int remove_pid(unsigned int pid){
  int bucket = pid % NUM_PROC_TABLE_BUCKETS;
  int found = 0;
  
  struct proc_table_entry *pentry;
  for (pentry=proc_table[bucket];pentry;pentry=pentry->next){
    if (pentry->pid == pid){
      found = 1;
      break;
    }
  }
  if (!found) return 1;

  if (pentry->next){
    pentry->next->prev = pentry->prev;
  }
  if (pentry->prev){
    pentry->prev->next = pentry->next;
  } else {
    proc_table[bucket] = pentry->next;
  }
  num_processes--;
  free(pentry);
  return 0;
}

// format: elapsed pid root
void handle_root(double elapsed,unsigned int pid){
  debug("handle_root(%u)\n",pid);

  struct proc_table_entry *pentry = lookup_pid(pid,1);
  root_process = pentry->pinfo;
}

// format: elapsed pid fork child_pid
void handle_fork(double elapsed,unsigned int pid,char *child){
  debug("handle_fork(%d,%s)\n",elapsed,pid,child);  
  unsigned int child_pid = atoi(child);
  struct process_info *pinfo,*parent_pinfo;
  struct proc_table_entry *pentry = lookup_pid(child_pid,1);
  struct proc_table_entry *parent_pentry = lookup_pid(pid,0);
  if (pentry) pentry->pinfo->start = elapsed;
  if (pentry && parent_pentry){
    pinfo = pentry->pinfo;
    parent_pinfo = parent_pentry->pinfo;
    
    pinfo->parent = parent_pinfo;
    
    if (parent_pinfo->children){
      pinfo->older_sibling = parent_pinfo->children;
      pinfo->older_sibling->younger_sibling = pinfo;
    }
    parent_pinfo->children = pinfo;
  }
}

// parse the /proc/<pid>/stat line and add information to the pinfo node

#define NUM_STAT_FIELDS 52
void parse_stat(char *stat,struct process_info *pinfo){
  debug2("parse_stat: %s\n",stat);
  char *p;
  int i;
  int index = 0;
  char *stat_fields[NUM_STAT_FIELDS] = { 0 };
  char *stat_line = strdup(stat);
  p = strtok(stat_line," \n");
  while (p && index < NUM_STAT_FIELDS){
    stat_fields[index] = p;
    p = strtok(NULL," \n");
    index++;
  }
  for (i=0;i<index;i++){
    debug2("\t%d: %s\n",i,stat_fields[i]);
  }
  if (stat_fields[13]) pinfo->utime = atoi(stat_fields[13]); // utime user time in clock ticks
  if (stat_fields[14]) pinfo->stime = atoi(stat_fields[14]); // stime system time in clock ticks
  if (stat_fields[21]) pinfo->starttime = atoll(stat_fields[21]); // starttime since boot in clock ticks
  if (stat_fields[22]) pinfo->vsize = atoi(stat_fields[22]); // vsize - virtual memory in bytes
  if (stat_fields[38]) pinfo->processor = atoi(stat_fields[38]); // last processor
  if (stat_fields[51]) pinfo->exit_code = atoi(stat_fields[51]); // exit code
  free(stat_line);
}

// format: elapsed pid exit </proc/pid/stat>
void handle_exit(double elapsed,unsigned int pid,char *stat){
  debug("handle_exit(%d,%s)\n",elapsed,pid,stat);

  int status;
  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->finish = elapsed;
    parse_stat(stat,pentry->pinfo);
    status = remove_pid(pid);
    if (status) warning("unable to remove process %d?\n",pid);
  }
}

void handle_comm(double elapsed,unsigned int pid,char *comm){
  debug("handle_comm(%d,%s)\n",elapsed,pid,comm);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->comm = strdup(comm);
  }
}

void handle_cmdline(double elapsed,unsigned int pid,char *cmdline){
  debug("handle_cmdline(%d,%s)\n",elapsed,pid,cmdline);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->cmdline = strdup(cmdline);
  }
}

// add totals for processes with the same name
struct comm_info {
  char *comm;
  unsigned int count;
  unsigned int total_utime;
  unsigned int total_stime;
  struct comm_info *next;
};
struct comm_info *comm_totals = NULL;
int num_comm_info;

// walk the tree accumulating totals in comm_totals structure
static void collect_process_totals(struct process_info *pinfo){
  struct comm_info *ci;
  struct process_info *child;

  // look for this process
  int found = 0;
  for (ci = comm_totals;ci;ci=ci->next){
    if (pinfo->comm && !strcmp(pinfo->comm,ci->comm)){
      ci->total_utime += pinfo->utime;
      ci->total_stime += pinfo->stime;
      ci->count++;
      found = 1;
      break;
    }
  }
  // not found, so add an entry
  if (!found && pinfo->comm){
    ci = calloc(1,sizeof(struct comm_info));
    ci->comm = strdup(pinfo->comm);
    ci->next = comm_totals;
    ci->total_utime = pinfo->utime;
    ci->total_stime = pinfo->stime;
    ci->count = 1;
    comm_totals = ci;
    num_comm_info++;
  }
  
  // walk the tree of children (the particular order doesn't matter so go from youngest to oldest
  for (child=pinfo->children;child;child=child->older_sibling)
    collect_process_totals(child);
}

int comm_compare(const void *c1,const void *c2){
  const struct comm_info *comm1 = c1;
  const struct comm_info *comm2 = c2;
  if (comm1->total_utime > comm2->total_utime) return -1;
  else if (comm1->total_utime < comm2->total_utime) return 1;
  else if (comm1->total_stime > comm2->total_stime) return -1;
  else if (comm1->total_stime < comm2->total_stime) return 1;
  else if (comm1->count > comm2->count) return -1;
  else if (comm1->count < comm2->count) return 1;
  else return strcmp(comm1->comm,comm2->comm);
}

static void print_statistics(){
  int count;
  int i;

  printf("%d processes\n",total_processes);
  
  struct comm_info *ci;
  collect_process_totals(root_process);
  // format and sort
  struct comm_info *comm_table = malloc(num_comm_info*sizeof(struct comm_info));
  count = 0;
  for (ci=comm_totals;ci;ci=ci->next){
    *(&comm_table[count]) = *ci;
    count++;
  }
  qsort(comm_table,num_comm_info,sizeof(struct comm_info),comm_compare);
  for (i=0;i<num_comm_info;i++){
    printf("\t%3d %-20s %8.2f %8.2f\n",
	   comm_table[i].count,
	   comm_table[i].comm,
	   (double) comm_table[i].total_utime / clocks_per_second,
	   (double) comm_table[i].total_stime / clocks_per_second);    
  }
  
  printf("%d processes running\n",num_processes);
  printf("%d maximum processes\n",max_num_processes);
  printf("\n");
}

static void print_tree(struct process_info *pinfo,int level){
  int i;
  int comm_width;
  int print_width;
  struct process_info *eldest;
  // print the node
  for (i=0;i<level;i++) printf("  ");
  printf("%d)",pinfo->pid);

  if (print_cmdline && pinfo->cmdline){
    print_width = output_width - 8 - 2*level;
    printf(" %.*s",print_width,pinfo->cmdline);
  } else if (pinfo->comm){
    printf(" %-16s",pinfo->comm);
  } else {
    printf(" ??");
  }
  printf(" cpu=%d",pinfo->processor);
  if (fflag){
    printf(" start=%-5.2f finish=%-5.2f",pinfo->start,pinfo->finish);
  }
  if (mflag){
    printf(" vmsize=%uk",(pinfo->vsize+512)/1024);
  }
  if (uflag){
    printf(" utime=%-4.2f stime=%-4.2f",
	   ((double) pinfo->utime/clocks_per_second),
	   ((double) pinfo->stime/clocks_per_second));
  }

  printf("\n");

  if (!pinfo->children) return;
  
  // find the eldest child and print the children
  for (eldest=pinfo->children;eldest->older_sibling != NULL;eldest=eldest->older_sibling);

  while (eldest){
    print_tree(eldest,level+1);
    eldest = eldest->younger_sibling;
  }
}


int main(int argc,char *const argv[],char *const envp[]){
  int opt;
  int value;
  char *p,*p2,*cmd;
  char *orig_buffer;
  char buffer[8192];
  double elapsed;
  int event_pid;

  clocks_per_second = sysconf(_SC_CLK_TCK);

  initialize_error_subsystem(argv[0],"-");

  // parse options
  while ((opt = getopt(argc,argv,"+CcFfMmSsTtUuvw:")) != -1){
    switch(opt){
    case 'C':
      print_cmdline = 1;
      break;
    case 'c':
      print_cmdline = 0;
      break;
    case 'F':
      fflag = 1;
      break;
    case 'f':
      fflag = 0;
      break;
    case 'M':
      mflag = 1;
      break;
    case 'm':
      mflag = 0;
      break;
    case 't':
      treeflag = 0;
      break;
    case 'T':
      treeflag = 1;
      break;
    case 's':
      statflag = 0;
      break;
    case 'S':
      statflag = 1;
      break;
    case 'U':
      uflag = 1;
      break;
    case 'u':
      uflag = 0;
      break;
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 'w':
      if (sscanf(optarg,"%d",&value) == 1){
	output_width = value;
      } else {
	warning("bad output width:%s ignored\n",optarg);
      }
      break;
    default:
    usage:
      fatal("usage: %s -[CcFfSsTtuv][-w width] file\n"
	    "\t-C\tturn on longer command line\n"
	    "\t-c\tturn on abbreviated command (default)\n"
	    "\t-F\turn on start/finish info (default)\n"
	    "\t-f\tturn off start/finish info\n"
	    "\t-M\tturn on vmsize printing\n"
	    "\t-m\tturn off vmsize printing\n"
	    "\t-S\tturn on summary output\n"
	    "\t-s\tturn off summary output (default)\n"
	    "\t-T\tturn on tree output (default)\n"
	    "\t-t\tturn off tree output\n"
	    "\t-U\tturn off utime in tree\n"
	    "\t-u\tturn on utime in tree\n"
	    "\t-v\tverbose messages\n"
	    "\t-w width\tset command width\n",
	    argv[0]);
      break;
    }
  }

  if (optind >= argc){
    fatal("missing data file\n");
  } else if (argc > optind + 1){
    warning("extra arguments after filename %s ignored\n",argv[optind]);
  }
  if ((process_file = fopen(argv[optind],"r")) == NULL){
    fatal("can not open file %s\n",argv[optind]);
  }

  // parse the input file
  while (fgets(buffer,sizeof(buffer),process_file) != NULL){
    if ((p = strchr(buffer,' '))) *p = 0;
    else continue;
    elapsed = strtod(buffer,&p2);
    if (p2 == buffer) // no value parsed, there can be newlines in input so ignore
      continue;
    
    p++;
    
    if ((p2 = strchr(p,' '))) *p2 = 0;
    else continue;
    event_pid = atoi(p); // no value parsed, there can be newlines in input so ignore
    if (event_pid <= 0) continue;
    p = p2+1;
    if ((p2 = strchr(p,'\n'))) *p2 = 0; // zap newline
    
    if (!strncmp(p,"fork",4)){
      handle_fork(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"root",4)){
      handle_root(elapsed,event_pid);
    } else if (!strncmp(p,"comm",4)){
      handle_comm(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"cmdline",7)){
      handle_cmdline(elapsed,event_pid,p+8);
    } else if (!strncmp(p,"exited",6)){
      // ignore exited events.
    } else if (!strncmp(p,"unknown",7)){
      // ignore unknown events
    } else if (!strncmp(p,"signal",6)){
      // ignore signal events
    } else if (!strncmp(p,"exit",4)){
      handle_exit(elapsed,event_pid,p+5);
    } else {
      warning("unknown command: %4.2f %d %s\n",elapsed,event_pid,p);
    }
  }

  if (statflag) print_statistics();
  if (treeflag) print_tree(root_process,0);

  return 0;
}
