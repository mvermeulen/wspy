/*
 * ptrace.c - process tracing subsystem using ptrace(2)
 *
 * NOTE: I tried putting these routines into a separate process, but this didn't work correctly, presumably because of the child relationship.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include "wspy.h"
#include "error.h"

static pid_t child_pid = 0;

// look up filename from /proc/[pid]/filename
char *lookup_process_filename(pid_t pid){
  int len;
  char procfile[32];
  static char buffer[1024];
  snprintf(procfile,sizeof(procfile),"/proc/%d/exe",pid);
  if ((len = readlink(procfile,buffer,sizeof(buffer))) > 0){
    buffer[len] = '\0';
    return buffer;
  } else {
    return NULL;
  }
}

// look up command from /proc/[pid]/comm
char *lookup_process_comm(pid_t pid){
  int fd;
  int len;
  int i;
  char procfile[32];
  static char buffer[1024];
  snprintf(procfile,sizeof(procfile),"/proc/%d/comm",pid);
  if ((fd = open(procfile,O_RDONLY)) > -1){
    len = read(fd,buffer,sizeof(buffer));
    close(fd);
    if (len <= 0) return NULL;
    for (i=0;i<len;i++){
      if (buffer[i] == '\n') buffer[i] = '\0';
    }
    return buffer;
  } else {
    return NULL;
  }
}

// fields from /proc/stat
struct procstat_info {
  /*  1- 5 */ int pid; char comm[32]; char state; int ppid, pgrp;
  /*  6-10 */ int session, tty_nr, tpgid; unsigned int flags; unsigned long minflt;
  /* 11-15 */ unsigned long cminflt, majflt, cmajflt, utime, stime;
  /* 16-20 */ long cutime, cstime, priority, nice, num_threads;
  /* 21-25 */ long itrealvalue; unsigned long long starttime; unsigned long vsize;
              long rss; unsigned long rsslim;
  /* 26-30 */ unsigned long startcode, endcode, startstack, kstkesp, kstkeip;
  /* 31-35 */ unsigned long signal, blocked, sigignore, sigcatch, wchan;
  /* 36-40 */ unsigned long nswap, cnswap; int exit_signal, processor; unsigned rt_priority;
  /* 41-45 */ unsigned policy; unsigned long long delayacct_blkio_ticks;
              unsigned long guest_time, cguest_time, start_data;
  /* 46-50 */ unsigned long end_data, start_brk, arg_start, arg_end, end_start;
  /* 51-52 */ unsigned long env_end; int exit_code;
};

// get static buffer for /proc/[pid]/stat
char *lookup_process_stat(pid_t pid){
  int fd;
  char procfile[32];
  int len;
  static char buffer[1024];
  snprintf(procfile,sizeof(procfile),"/proc/%d/stat",pid);
  if ((fd = open(procfile,O_RDONLY)) > -1){
    len = read(fd,buffer,sizeof(buffer));
    close(fd);
    if (len <= 0) return NULL;
    return buffer;
  }
  return NULL;
}

// get static buffer for /proc/[pid]/task/[pid]/stat
char *lookup_process_task_stat(pid_t pid){
  int fd;
  char procfile[32];
  int len;
  static char buffer[1024];
  snprintf(procfile,sizeof(procfile),"/proc/%d/task/%d/stat",pid,pid);
  if ((fd = open(procfile,O_RDONLY)) > -1){
    len = read(fd,buffer,sizeof(buffer));
    close(fd);
    if (len <= 0) return NULL;
    return buffer;
  }
  return NULL;
}


/* Turn the stat information into a structure */
int parse_process_stat(char *line,struct procstat_info *pi){
  int count;
  if (line == NULL) return 0;
  count = sscanf(line,"%d %32s %c %d %d %d %d %d %u %lu"
		 "%lu %lu %lu %lu %lu %ld %ld %ld %ld %ld"
		 "%ld %llu %lu %ld %lu %lu %lu %lu %lu %lu"
		 "%lu %lu %lu %lu %lu %lu %lu %d %d %u"
		 "%u %llu %lu %ld %lu %lu %lu %lu %lu %lu"
		 "%lu %d",
		 &pi->pid,pi->comm,&pi->state,&pi->ppid,&pi->pgrp,
		 &pi->session,&pi->tty_nr,&pi->tpgid,&pi->flags,&pi->minflt,
		 &pi->cminflt,&pi->majflt,&pi->cmajflt,&pi->utime,&pi->stime,
		 &pi->cutime,&pi->cstime,&pi->priority,&pi->nice,&pi->num_threads,
		 &pi->itrealvalue,&pi->starttime,&pi->vsize,&pi->rss,&pi->rsslim,
		 &pi->startcode,&pi->endcode,&pi->startstack,&pi->kstkesp,&pi->kstkeip,
		 &pi->signal,&pi->blocked,&pi->sigignore,&pi->sigcatch,&pi->wchan,
		 &pi->nswap,&pi->cnswap,&pi->exit_signal,&pi->processor,&pi->rt_priority,
		 &pi->policy,&pi->delayacct_blkio_ticks,&pi->guest_time,&pi->cguest_time,&pi->start_data,
		 &pi->end_data,&pi->start_brk,&pi->arg_start,&pi->arg_end,&pi->end_start,
		 &pi->env_end,&pi->exit_code);
  return count;
}

void ptrace_setup(pid_t child){
  int status;
  
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
		  PTRACE_O_TRACECLONE|PTRACE_O_TRACEFORK|PTRACE_O_TRACEVFORK|PTRACE_O_TRACEVFORKDONE|
		  PTRACE_O_TRACEEXEC |   // exec(2)
		  PTRACE_O_TRACEEXIT);   // exit(2)
  
  ptrace(PTRACE_CONT,child_pid,NULL,NULL);
}

void ptrace_loop(void){
  pid_t pid;
  int status;
  long data;
  int cloned;
  char *cmdline;
  char *statline;
  procinfo *pinfo,*child_pinfo;
  struct procstat_info procstat_info;
  while (1){
    pid = wait(&status);
    if (pid == -1){
      if (errno == ECHILD) break; // no more children to wait
      error("wait returns -1 with errno %d: %s\n",errno,strerror(errno));
    }
    if (WIFEXITED(status)){
      // we can ignore the exit operations since already handled when we get an event
      debug2("pid %d exited\n",pid);
      if (pid == child_pid) break; // my immediate child quit
    } else if (WIFSIGNALED(status)){
      // we can ignore the exit operations since already handled when we get an event      
      debug2("pid %d signaled\n",pid);
    } else if (WIFSTOPPED(status)){
      if (WSTOPSIG(status) == SIGTRAP){
	if (flag_require_ftrace) pthread_mutex_lock(&event_lock);
	pinfo = lookup_process_info(pid,1);
	cloned = 0;
	switch(status>>16){
	case PTRACE_EVENT_CLONE:
	  cloned = 1; // fall thru
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	  pinfo->cloned = cloned;
	  // This process had a fork/vfork/clone, get the child process
	  if (ptrace(PTRACE_GETEVENTMSG,pid,NULL,&data) != -1){
	    debug2("pid %d forked to create new pid %ld\n",pid,data);
	    child_pinfo = lookup_process_info(data,1);
	    child_pinfo->cloned = 1;
	    child_pinfo->ppid = pid;
	    // only create parent/sibling links if not already done e.g. by ftrace
	    if (child_pinfo->parent == NULL){
	      child_pinfo->parent = pinfo;
	      child_pinfo->sibling = pinfo->child;
	      pinfo->child = child_pinfo;
	    }
	    if (!flag_require_ftrace){
	      // read the time if the ftrace isn't going to give it
	      read_uptime(&child_pinfo->time_fork);
	    }
	  }
	  break;
	case PTRACE_EVENT_VFORK_DONE:
	  // At this point nothing unique to do
	  break;
	case PTRACE_EVENT_EXEC:
	  cmdline = lookup_process_comm(pid);
	  if (cmdline) pinfo->filename = strdup(cmdline);
	  if (!flag_require_ftrace){
	    read_uptime(&pinfo->time_exec);	    
	  }
	  break;
	case PTRACE_EVENT_EXIT:
	  if (pinfo->filename == NULL){
	    // typically happens for first process which we get only after exec
	    cmdline = lookup_process_comm(pid);
	    if (cmdline) pinfo->filename = strdup(cmdline);	    
	  }
	  if (pinfo->cloned){
	    if ((statline = lookup_process_task_stat(pid)) == NULL){
	      statline = lookup_process_stat(pid);
	    }
	  } else {
	    statline = lookup_process_stat(pid);	    
	  }
	  if (parse_process_stat(statline,&procstat_info)){
	    pinfo->cpu = procstat_info.processor;
	    pinfo->utime = procstat_info.utime;
	    pinfo->stime = procstat_info.stime;
	    pinfo->vsize = procstat_info.vsize;
	  }
	  if (!flag_require_ftrace){
	    read_uptime(&pinfo->time_exit);	    
	  }	  
	  pinfo->p_exited = 1;
	  break;
	default:
	  debug("pid %d stopped with event %d\n",pid,status>>16);
	  break;
	}
	if (flag_require_ftrace) pthread_mutex_unlock(&event_lock);	
      } else if (WSTOPSIG(status) == SIGSTOP){
	// SIGSTOP is given as status for newly created processes after fork/vfork/clone
	// it can't be delivered to the child
	debug2("pid %d stopped with signal SIGSTOP\n",pid,WSTOPSIG(status));	
	ptrace(PTRACE_CONT,pid,NULL,NULL);
	continue;
      } else {
	// Other signals should be passed along to the child
	debug2("pid %d stopped with signal %d\n",pid,WSTOPSIG(status));
	ptrace(PTRACE_CONT,pid,NULL,WSTOPSIG(status));
	continue;
      }
    } else if (WIFCONTINUED(status)){
      debug("pid %d continued\n",pid);      
    }
    ptrace(PTRACE_CONT,pid,NULL,NULL);
  }
}
