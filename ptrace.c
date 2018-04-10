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
    for (i=0;i<len;i++){
      if (buffer[i] == '\n') buffer[i] = '\0';
    }
    return buffer;
  } else {
    return NULL;
  }
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
  char *cmdline;
  procinfo *pinfo,*child_pinfo;
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
	pinfo = lookup_process_info(pid,1);
	switch(status>>16){
	case PTRACE_EVENT_FORK:
	case PTRACE_EVENT_VFORK:
	case PTRACE_EVENT_CLONE:
	  // This process had a fork/vfork/clone, get the child process
	  if (ptrace(PTRACE_GETEVENTMSG,pid,NULL,&data) != -1){
	    debug2("pid %d forked to create new pid %ld\n",pid,data);
	    child_pinfo = lookup_process_info(data,1);
	    child_pinfo->ppid = pid;
	    child_pinfo->parent = pinfo;
	    child_pinfo->sibling = pinfo->child;
	    pinfo->child = child_pinfo;
	  }
	  break;
	case PTRACE_EVENT_VFORK_DONE:
	  // At this point nothing unique to do
	  break;
	case PTRACE_EVENT_EXEC:
	  cmdline = lookup_process_comm(pid);
	  if (cmdline) pinfo->filename = strdup(cmdline);
	  break;
	case PTRACE_EVENT_EXIT:
	  if (pinfo->filename == NULL){
	    // typically happens for first process which we get only after exec
	    cmdline = lookup_process_comm(pid);
	    if (cmdline) pinfo->filename = strdup(cmdline);	    
	  }
	  pinfo->exited = 1;
	  break;
	default:
	  debug("pid %d stopped with event %d\n",pid,status>>16);
	  break;
	}
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
