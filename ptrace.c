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
#include <sys/user.h>
#include <sys/syscall.h>
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

// look up ppid from /proc/[pid]/stat
pid_t lookup_process_ppid(pid_t pid){
  char procfile[32];
  int fd;
  int len;
  char buffer[1024];
  char *p;
  pid_t ppid;
  char state;
  snprintf(procfile,sizeof(procfile),"/proc/%d/stat",pid);
  if ((fd = open(procfile,O_RDONLY)) > -1){
    len = read(fd,buffer,sizeof(buffer));
    p = strchr(buffer,')');
    if (p){
      sscanf(p+2,"%c %d",&state,&ppid);
      if (ppid){
	close(fd);
	return ppid;
      }
    }
    close(fd);
  }
  return 0;
}

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
  char *lparen,*rparen;
  if (line == NULL) return 0;
  if (sscanf(line,"%d",&pi->pid) != 1) return 0;
  lparen = strchr(line,'(');
  rparen = strchr(lparen,')');
  strncpy(pi->comm,lparen+1,rparen-lparen);
  
  count = sscanf(rparen+2,"%c %d %d %d %d %d %u %lu"
		 "%lu %lu %lu %lu %lu %ld %ld %ld %ld %ld"
		 "%ld %llu %lu %ld %lu %lu %lu %lu %lu %lu"
		 "%lu %lu %lu %lu %lu %lu %lu %d %d %u"
		 "%u %llu %lu %ld %lu %lu %lu %lu %lu %lu"
		 "%lu %d",
		 &pi->state,&pi->ppid,&pi->pgrp,
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
  return count+2;
}

// returns pointer to static location
char *ptrace_read_null_terminated_string(pid_t pid,long addr){
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
  
  ptrace(flag_syscall?PTRACE_SYSCALL:PTRACE_CONT,child_pid,NULL,NULL);
}

void ptrace_loop(void){
  pid_t pid;
  int status;
  long data;
  int cloned;
  char *cmdline;
  char *statline;
  procinfo *pinfo,*child_pinfo;
  char *open_filename;
  struct user_regs_struct regs;
  struct procstat_info procstat_info;
  static int last_syscall = 0;
  static int syscall_entry = 0;
  while (1){
    pid = wait(&status);
    debug2("event: pid=%d\n",pid);
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
	//	notice("pid %d SIGTRAP\n",pid);
	if (flag_require_ftrace) pthread_mutex_lock(&event_lock);
	pinfo = lookup_process_info(pid,1);
	cloned = 0;
	switch(status>>16){
	case PTRACE_EVENT_CLONE:
	  cloned = 1; // fall thru
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	  //	  notice("    fork(%d)\n",pid);
	  pinfo->cloned = cloned;
	  // This process had a fork/vfork/clone, get the child process
	  if (ptrace(PTRACE_GETEVENTMSG,pid,NULL,&data) != -1){
	    //	    notice("        child %d\n",data);
	    child_pinfo = lookup_process_info(data,1);
	    child_pinfo->cloned = 1;
	    child_pinfo->ppid = pid;
	    // only create parent/sibling links if not already done e.g. by ftrace
	    if (child_pinfo->parent == NULL){
	      child_pinfo->parent = pinfo;
	      child_pinfo->sibling = pinfo->child;
	      pinfo->child = child_pinfo;
	    }
	    if (child_pinfo->time_start == 0){
	      read_uptime(&child_pinfo->time_start);
	    }
	  }
	  if (flag_require_perftree){
	    start_process_perf_counters(child_pinfo->pid,&child_pinfo->pci,0);
	    child_pinfo->counters_started = 1;
	  }
	  break;
	case PTRACE_EVENT_VFORK_DONE:
	  //	  notice("    forkdone(%d)\n",pid);	  
	  // At this point nothing unique to do
	  break;
	case PTRACE_EVENT_EXEC:
	  //	  notice("    exec(%d)\n",pid);	  	  
	  cmdline = lookup_process_comm(pid);
	  if (cmdline) pinfo->filename = strdup(cmdline);
	  if (pinfo->time_start == 0){
	    read_uptime(&pinfo->time_start);	    
	  }	  
	  break;
	case PTRACE_EVENT_EXIT:
	  //	  notice("    exit(%d)\n",pid);	  	  	  
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
	    pinfo->ppid = procstat_info.ppid;
	    pinfo->minflt = procstat_info.minflt;
	    pinfo->majflt = procstat_info.majflt;
	    pinfo->utime = procstat_info.utime;
	    pinfo->stime = procstat_info.stime;
	    pinfo->cutime = procstat_info.cutime;
	    pinfo->cstime = procstat_info.cstime;
	    pinfo->starttime = procstat_info.starttime;
	    pinfo->vsize = procstat_info.vsize;
	    pinfo->rss = procstat_info.rss;
	    pinfo->cpu = procstat_info.processor;
	  }
	  if (pinfo->time_finish == 0){
	    read_uptime(&pinfo->time_finish);	    
	  }
	  if (pinfo->counters_started)
	    stop_process_perf_counters(pinfo->pid,&pinfo->pci);
	  pinfo->p_exited = 1;
	  break;
	default:
	  //	  notice("    unknown event %d (%d)\n",status>>16,pid);	  	  	  
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
      } else if (WSTOPSIG(status) == (SIGTRAP | 0x80)){
	// Stopped because of a system call
	ptrace(PTRACE_GETREGS,pid,0,&regs);
	if (last_syscall != regs.orig_rax){
	  syscall_entry = 1;
	} else {
	  syscall_entry = (1-syscall_entry);
	}
	last_syscall = regs.orig_rax;
	// Note: Not sure why, but comparing with strace, I notice SYS_open results are off by 1
	//    Negative values -2 rather than -1 and positive values +1 compared to original.
	// Use this hack to correct and it now matches strace(1) output.
	if (regs.orig_rax == SYS_open){
	  if ((long) regs.rax < 0) regs.rax += 1;
	  else regs.rax -= 1;
	}
	if (syscall_entry){
	  switch(regs.orig_rax){
	  case SYS_open:
	    open_filename = ptrace_read_null_terminated_string(pid,regs.rdi);
	    debug2("pid %d entry to open syscall (\"%s\")\n",pid,open_filename);
	    break;
	  default:
	    debug2("pid %d entry to syscall %d\n",pid,regs.orig_rax);
	    break;
	  }
	} else {
	  switch(regs.orig_rax){
	  case SYS_open:
	    open_filename = ptrace_read_null_terminated_string(pid,regs.rdi);
	    pinfo = lookup_process_info(pid,1);
	    add_process_syscall_open(pinfo,open_filename,regs.rax);
	    debug2("pid %d exit from open syscall (\"%s\") = %ld\n",pid,open_filename,(long) regs.rax);
	    break;
	  default:
	    debug2("pid %d exit from syscall %d = %ld\n",pid,regs.orig_rax,regs.rax);	    
	    break;  
	  }
	}
	ptrace(PTRACE_SYSCALL,pid,NULL,NULL,NULL);
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
    ptrace(flag_syscall?PTRACE_SYSCALL:PTRACE_CONT,pid,NULL,NULL);
  }
}
