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
int vflag = 0;
int output_width = 80;
int print_cmdline = 0;

/* process_info - maintained for each process */
struct process_info {
  unsigned int pid;
  char *comm; // short version of the command
  char *cmdline; // long command line
  double start,finish;
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

// format: elapsed pid exit </proc/pid/stat>
void handle_exit(double elapsed,unsigned int pid,char *stat){
  debug("handle_exit(%d,%s)\n",elapsed,pid,stat);

  int status;
  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->finish = elapsed;
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

static void print_statistics(){
  printf("%d processes\n",total_processes);
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
    printf(" %s",pinfo->comm);
  } else {
    printf(" ??");
  }
  printf(" start=%5.2f finish=%5.2f",pinfo->start,pinfo->finish);

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
  char buffer[1024];
  double elapsed;
  int event_pid;

  initialize_error_subsystem(argv[0],"-");

  // parse options
  while ((opt = getopt(argc,argv,"+cSsTtvw:")) != -1){
    switch(opt){
    case 'c':
      print_cmdline = 1;
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
      fatal("usage: %s -[sStTv] file\n"
	    "\t-c\tprint command line\n"
	    "\t-S\tturn on summary output\n"
	    "\t-s\tturn off summary output (default)\n"
	    "\t-T\tturn on tree output (default)\n"
	    "\t-t\tturn off tree output\n"
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
    if (p = strchr(buffer,' ')) *p = 0;
    else continue;
    elapsed = atof(buffer);
    p++;
    
    if (p2 = strchr(p,' ')) *p2 = 0;
    else continue;
    event_pid = atoi(p);
    p = p2+1;
    if (p2 = strchr(p,'\n')) *p2 = 0; // zap newline
    
    if (!strncmp(p,"fork",4)){
      handle_fork(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"root",4)){
      handle_root(elapsed,event_pid);
    } else if (!strncmp(p,"comm",4)){
      handle_comm(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"cmdline",7)){
      handle_cmdline(elapsed,event_pid,p+8);      
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
