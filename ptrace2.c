/*
 * ptrace2.c - new improved process tracing subsystem using ptrace(2)
 *
 * Revised version from the ptrace.c & proctable.c file with the following tradeoffs:
 *
 * 1. Does not build up an internal processtree structure in memory.
 *    - This should allow for faster processing of events.
 *    - This means one must run a command "process-cmd" to format output
 * 2. Creates one static table for all the process information.
 *    - Should be faster, but also take more space.
 *
 * Overall, the tradeoff is towards fast processing of events in real time with
 * more post-processing required.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/user.h>
#include <sys/syscall.h>
#include <errno.h>
#include "wspy.h"
#include "error.h"

struct processinfo {
  pid_t pid,ppid;
  unsigned int cloned           : 1;
  unsigned int fork_event       : 1;
  unsigned int sigstop_event    : 1;
  unsigned int counters_started : 1;
  double start;
  double finish;
  char *comm;
  struct rusage rusage;  
  struct process_counter_info pci;
};

static void start_pid(pid_t pid,pid_t parent,int root);
static void stop_pid(pid_t pid);
char *ptrace2_read_null_terminated_string(pid_t pid,long addr);

static pid_t child_pid = 0;
// Size of the table is same as the maximum number of processes.
// This allows for direct quick lookup w/o searching.  Normally this
// value seems to be 32768 but it can be configured, hence check for
// a builtin maximum to taking too much memory.
int pid_max = 100000;
int max_pid = 0;
static struct processinfo *proc_table = NULL;

FILE *process_csvfile = NULL;
FILE *open_csvfile = NULL;
int process_csvfile_num = 0;

void ptrace2_setup(pid_t child){
  FILE *fp;
  int status;
  if (fp = fopen("/proc/sys/kernel/pid_max","r")){
    if (fscanf(fp,"%d\n",&max_pid) == 1){
      if (max_pid > pid_max){
	fatal("--processtree-engine ptrace2 expects /proc/sys/kernel/pid_max to be < %d\n"
	      "\tset --pid-max option or use --processtree-engine ptrace instead\n",pid_max);
      }
    } else {
      fatal("unable to read /proc/sys/kernel/pid_max\n");
    }
    proc_table = calloc(max_pid,sizeof(struct processinfo));
    fclose(fp);
  } else {
    fatal("unable to read /proc/sys/kernel/pid_max\n");    
  }
  if ((process_csvfile = fopen("processtree2.csv","w")) == NULL){
    fatal("unable to open processtree2.csv\n");
  }

  // header row, must match against "process-csv" program
  fprintf(process_csvfile,"#version %d\n",version);
  fprintf(process_csvfile,"#pid,ppid,filename,start,finish,utime_sec,utime_usec,stime_sec,stime_usec,maxrss,minflt,majflt,inblock,oublock,msgsnd,msgrcv,nsignals,nvcsw,nivcsw,num_counters\n");
  if (flag_syscall){
    if ((open_csvfile = fopen("openfile.csv","w")) == NULL){
      fatal("unable to open openfile.csv\n");      
    }
    fprintf(open_csvfile,"#pid,time,status,filename\n");
  }
  

  child_pid = child;
  debug("ptrace_setup\n");
  waitpid(child_pid,&status,0);
  if (WIFEXITED(status)){
    error("child process exited unexpectedly\n");    
  } else if (WIFSIGNALED(status)){
      error("child process signaled unexpectedly,signal = %d\n",
	    WSTOPSIG(status));    
  }
  status = ptrace(PTRACE_SETOPTIONS,child_pid,0,
		  PTRACE_O_TRACESYSGOOD | // not tracing system calls, but keep more precise events
		  PTRACE_O_EXITKILL  |   // kill child if I exit
		  // various forms of clone
		  PTRACE_O_TRACECLONE|PTRACE_O_TRACEFORK|PTRACE_O_TRACEVFORK|
		  PTRACE_O_TRACEEXIT // exit(0)
		  );
  
  ptrace(flag_syscall?PTRACE_SYSCALL:PTRACE_CONT,child_pid,NULL,NULL);
  start_pid(child_pid,getpid(),1);
}

// loop through and handle ptrace(2) events
void ptrace2_loop(void){
  pid_t pid,child;
  int status;
  long data;
  int cloned;
  struct rusage rusage;
  char *open_filename;
  double now;
  struct user_regs_struct regs;
  static int last_syscall = 0;
  static int syscall_entry = 0;

  while (1){
    pid = wait4(-1,&status,0,&rusage);
    debug("event: pid=%d\n",pid);
    if (pid == -1){
      if (errno == ECHILD){
	break; // no more children to wait
      } else {
	error("wait returns -1 with errno %d: %s\n",errno,strerror(errno));
      }
    }
    if (WIFEXITED(status)){
      proc_table[pid].rusage = rusage;      
      if (proc_table[pid].pid){
	stop_pid(pid);
      }
      debug2("   WIFEXITED(%d)\n",pid);
      if (pid == child_pid) break;
      
    } else if (WIFSIGNALED(status)){
      proc_table[pid].rusage = rusage;
      if (proc_table[pid].pid){
	stop_pid(pid);
      }
      debug2("   WIFSIGNALED(%d)\n",pid);      
    } else if (WIFSTOPPED(status)){
      if (WSTOPSIG(status) == SIGTRAP){
	cloned = 0;
	// we have an event!
	switch(status>>16){
	case PTRACE_EVENT_CLONE:
	  //	  cloned = 1;
	  // fall through
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	  break;
	case PTRACE_EVENT_EXIT:
	  if (!proc_table[pid].comm){
	    char *comm = lookup_process_comm(pid);
	    if (comm){
	      proc_table[pid].comm = strdup(comm);
	    }
	  }
	  break;
	default:
	  debug2("   unknown event %d (%d)\n",status>>16,pid);
	  break;
	}
      } else if (WSTOPSIG(status) == SIGSTOP){
	// SIGSTOP is status for newly created processes after fork/vfork/clone
	// continue without passing along this signal
	proc_table[pid].sigstop_event = 1;
	if (proc_table[pid].pid == 0){
	  // start recording information
	  start_pid(pid,0,0);
	}
	ptrace(PTRACE_CONT,pid,NULL,NULL);
	continue;
      } else if (WSTOPSIG(status) == (SIGTRAP | 0x80)){
	// stopped because of a system call
	ptrace(PTRACE_GETREGS,pid,0,&regs);
	if (last_syscall != regs.orig_rax){
	  syscall_entry = 1;
	} else {
	  syscall_entry = (1-syscall_entry);
	}
	last_syscall = regs.orig_rax;
	if ((syscall_entry == 0) && (last_syscall == SYS_open) && (open_csvfile != NULL)){
	  read_uptime(&now);
	  open_filename = ptrace2_read_null_terminated_string(pid,regs.rdi);
	  fprintf(open_csvfile,"%d,%6.5f,%lld,%s\n",pid,now,regs.rax,open_filename);
	}
	ptrace(PTRACE_SYSCALL,pid,NULL,NULL,NULL);
      } else {
	// pass other signals to the child
	ptrace(PTRACE_CONT,pid,NULL,WSTOPSIG(status));
      }
    } else if (WIFCONTINUED(status)){
      // nothing here
    }
    // let the child go and get the next event
    ptrace(flag_syscall?PTRACE_SYSCALL:PTRACE_CONT,pid,NULL,NULL);
  }
}

void ptrace2_finish(void){
  FILE *fp;
  fclose(process_csvfile);
  if (open_csvfile != NULL)
    fclose(open_csvfile);
}

void cleanup_process_table_entry(pid_t pid){
  // clear out the old data if it exists
}

static void start_pid(pid_t pid,pid_t parent,int root){
  debug("start %d\n",pid);
  cleanup_process_table_entry(pid);
  proc_table[pid].pid = pid;
  if (parent){
    proc_table[pid].ppid = parent;
  } else {
    proc_table[pid].ppid = lookup_process_ppid(pid);
  }
  read_uptime(&proc_table[pid].start);
  if (perfcounter_model == PM_APPLICATION){
    if (root){
      start_process_perf_counters(pid,&proc_table[pid].pci,root);
      proc_table[pid].counters_started = 1;
    }
  } else if (perfcounter_model == PM_PROCESS){
    start_process_perf_counters(pid,&proc_table[pid].pci,root);
    proc_table[pid].counters_started = 1;
  }
}

static void stop_pid(pid_t pid){
  int i;
  struct counterlist *cl;
  debug("stop %d\n",pid);

  read_uptime(&proc_table[pid].finish);
  if (proc_table[pid].counters_started){
    stop_process_perf_counters(pid,&proc_table[pid].pci);
  }

  if (!proc_table[pid].comm){
    char *comm = lookup_process_comm(pid);
    if (comm) proc_table[pid].comm = strdup(comm);
  }

  fprintf(process_csvfile,"%d,%d,%s,%6.5f,%6.5f,%lu,%lu,%lu,%lu,"
	  "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,"
	  "%d,",
	  proc_table[pid].pid,
	  proc_table[pid].ppid,
	  proc_table[pid].comm?proc_table[pid].comm:"?",
	  proc_table[pid].start,proc_table[pid].finish,
	  proc_table[pid].rusage.ru_utime.tv_sec,proc_table[pid].rusage.ru_utime.tv_usec,
	  proc_table[pid].rusage.ru_stime.tv_sec,proc_table[pid].rusage.ru_stime.tv_usec,
	  proc_table[pid].rusage.ru_maxrss,
	  proc_table[pid].rusage.ru_minflt,
	  proc_table[pid].rusage.ru_majflt,
	  proc_table[pid].rusage.ru_inblock,
	  proc_table[pid].rusage.ru_oublock,
	  proc_table[pid].rusage.ru_msgsnd,
	  proc_table[pid].rusage.ru_msgrcv,
	  proc_table[pid].rusage.ru_nsignals,	  
	  proc_table[pid].rusage.ru_nvcsw,	  
	  proc_table[pid].rusage.ru_nivcsw,
	  NUM_COUNTERS_PER_PROCESS
	  );
  for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++){
    cl = perf_counters_by_process[i];
    if (cl && cl->ci->scale){
      fprintf(process_csvfile,"%lu,",proc_table[pid].pci.perf_counter[i]*cl->ci->scale);
    } else {
      fprintf(process_csvfile,"%lu,",proc_table[pid].pci.perf_counter[i]);
    }
  }
  
  fprintf(process_csvfile,"\n");
  if (proc_table[pid].comm) free(proc_table[pid].comm);
  memset(&proc_table[pid],0,sizeof(struct processinfo));
  process_csvfile_num++;
}

// returns pointer to static location
char *ptrace2_read_null_terminated_string(pid_t pid,long addr){
  static char buffer[4096];
  char *bufptr = buffer;
  int i;
  int len = 0;
  long int result;
  do {
    result = ptrace(PTRACE_PEEKDATA,pid,addr,0);
    if ((result == -1) && (errno != 0)){
      return NULL;
    } else {
      ((unsigned int *) &bufptr[len])[0] = result;
    }
    if (bufptr[len] == '\0' ||
	bufptr[len+1] == '\0' ||
	bufptr[len+2] == '\0' ||
	bufptr[len+3] == '\0')
      break;
    len += sizeof(int);
    addr += sizeof(int);
  } while (len < 4096);
  return bufptr;
}
