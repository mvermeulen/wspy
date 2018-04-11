/*
 * ftrace.c - kernel tracing subsystem
 *
 * Implementation of a thread to monitor kernel "ftrace" tracing capability.
 * Initial implementation views kernel sched scheduling.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <unistd.h>
#include <errno.h>
#include "wspy.h"
#include "error.h"

#define TRACEFS "/sys/kernel/debug/tracing"
int ftrace_cmd_pipe[2]; // command pipe
int timer_cmd_pipe[2];  // command pipe
static void ftrace_enable_tracing(void);
static void ftrace_disable_tracing(void);
static void ftrace_loop(void);
static pid_t trace_pid = 0;


void *ftrace_start(void *arg){
  pipe(ftrace_cmd_pipe);

  trace_pid = ((pid_t *) arg)[0];

  ftrace_enable_tracing();

  ftrace_loop();
  
  ftrace_disable_tracing();
  return NULL;
}

static void ftrace_set_variable(char *filename,char *value){
  FILE *fp = fopen(filename,"w+");
  if (fp){
    fputs(value,fp);
    fclose(fp);
  } else {
    warning("unable to open %s for tracing\n",filename);
  }
}

// turn on tracing
static void ftrace_enable_tracing(void){
  char buffer[16];
  struct stat statbuf;
  if ((stat(TRACEFS,&statbuf) != 0) || ! S_ISDIR(statbuf.st_mode)){
    if ((errno == EACCES) && (getuid() != 0)){
      error("permission denied for kernel tracing at %s\n",TRACEFS);
    } else {      
      error("unable to find kernel tracing at %s\n",TRACEFS);
    } 
    pthread_exit(NULL);
  }
  chdir(TRACEFS);

  ftrace_set_variable("events/sched/sched_process_exec/enable","1\n");
  ftrace_set_variable("events/sched/sched_process_exit/enable","1\n");
  ftrace_set_variable("events/sched/sched_process_fork/enable","1\n");
  ftrace_set_variable("tracing_on","1\n");  
}

// turn off tracing
static void ftrace_disable_tracing(void){
  chdir(TRACEFS);
  ftrace_set_variable("tracing_on","0\n");
  ftrace_set_variable("events/sched/enable","0\n");
}

static void ftrace_parse_line(char *line){
  char *p,*p2;
  int num_cpu = 0;
  int num_secs = 0;
  int num_usecs = 0;
  int len = 0;
  pid_t npid = 0;
  char *function = NULL;
  char *comm;
  procinfo *pinfo;
  static int param_init = 0;
  static int position_lbracket = 0;
  static int position_rbracket = 0;
  static int position_timestamp = 0;
  static int position_function = 0;
  debug2("line: %s",line);
  // not clear if this happens in the pipe, but just in case
  if (line[0] == '#') return;
  // verify that there is still a ":" at the function line
  // one way this can happen is the timestamp adds a digit
  if (param_init == 0 || (line[position_function-2] != ':')){
    p = strchr(line,'[');
    position_lbracket = p - line;
    p = strchr(line,']');
    position_rbracket = p - line;
    position_timestamp = position_rbracket + 6;
    p = strchr(&line[position_timestamp],':');
    position_function = p - line + 2;
    param_init = 1;
  }
  sscanf(&line[position_lbracket+1],"%d",&num_cpu);
  sscanf(&line[position_timestamp],"%d\056%d",&num_secs,&num_usecs);
  function = &line[position_function];
  p = strstr(&function[20],"pid=");
  if (p){
    sscanf(p+4,"%d",&npid);
  }
  pinfo = lookup_process_info(npid,1);

  p = strstr(&function[20],"comm=");
  if (p){
    comm = p+5;
    len = strcspn(comm," \t\n");
    if ((pinfo->comm == NULL) ||
	(strncmp(comm,pinfo->comm,5) != 0)){
      pinfo->comm = strndup(comm,len);
    }
  }
  if (!strncmp(function,"sched_process_fork",18)){
    pid_t child_pid = 0;
    p = strstr(&function[20],"child_pid=");
    if (p) sscanf(p+10,"%d",&child_pid);
    procinfo *child_pinfo = lookup_process_info(child_pid,1);

    pinfo->cpu = num_cpu;
    
    child_pinfo->ppid = npid;
    child_pinfo->time_fork.tv_sec = num_secs;
    child_pinfo->time_fork.tv_usec = num_usecs;
    // only create parent/sibling links if not already done e.g. by ptrace
    if (child_pinfo->parent == NULL){
      child_pinfo->parent = pinfo;
      child_pinfo->sibling = pinfo->child;
      pinfo->child = child_pinfo;
    }

    debug("fork(pid=%d,cpu=%d,time=%d\056%d,child_pid=%d)\n",
	  npid,num_cpu,num_secs,num_usecs,child_pid);
  } else if (!strncmp(function,"sched_process_exec",18)){
    char *filename = "";
    p = strstr(&function[20],"filename=");
    if (p){
      filename = p+9;
      len = strcspn(filename," \t\n");
    }

    pinfo->cpu = num_cpu;
    pinfo->time_exec.tv_sec = num_secs;
    pinfo->time_exec.tv_usec = num_usecs;
    pinfo->filename = strndup(filename,len);

    debug("exec(pid=%d,cpu=%d,time=%d\056%d,filename=%.*s)\n",
	  npid,num_cpu,num_secs,num_usecs,len,filename);
  } else if (!strncmp(function,"sched_process_exit",18)){
    pinfo->cpu = num_cpu;
    pinfo->time_exit.tv_sec = num_secs;
    pinfo->time_exit.tv_usec = num_usecs;
    pinfo->f_exited = 1;

    debug("exit(pid=%d,cpu=%d,time=%d\056%d)\n",
	  npid,num_cpu,num_secs,num_usecs);
  } else {
    warning("unknown trace line\n");
    fputs(line,stderr);
  }
}

static void ftrace_loop(void){
  fd_set rdfd_list;
  struct timeval tv;
  FILE *cmd_file,*trace_file;
  char buffer[1024],*p,*ptr;
  int status;
  int ftrace_fd;
  int maxfd = ftrace_cmd_pipe[0];

  // open the trace file system pipe
  chdir(TRACEFS);
  if ((ftrace_fd = open("trace_pipe",O_RDONLY)) == -1){
    error("unable to open trace_pipe\n");
    pthread_exit(NULL);
  }
  if (ftrace_fd > maxfd) maxfd = ftrace_fd;
  trace_file = fdopen(ftrace_fd,"r");
  
  cmd_file = fdopen(ftrace_cmd_pipe[0],"r");

  // loop until a "quit" command is received
  while (1){
    FD_ZERO(&rdfd_list);
    FD_SET(ftrace_cmd_pipe[0],&rdfd_list);
    FD_SET(ftrace_fd,&rdfd_list);

    // check once a second
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    status = select(maxfd+1,&rdfd_list,NULL,NULL,&tv);

    if (status){
      if (FD_ISSET(ftrace_cmd_pipe[0],&rdfd_list)){
	fgets(buffer,sizeof(buffer),cmd_file);
	if (!strncmp(buffer,"quit",4)){
	  return;
	}
      }
      if (FD_ISSET(ftrace_fd,&rdfd_list)){
	fgets(buffer,sizeof(buffer),trace_file);
	if (flag_require_ptrace) pthread_mutex_lock(&event_lock);
	ftrace_parse_line(buffer);
	if (flag_require_ptrace) pthread_mutex_unlock(&event_lock);
      }
    }
  }
}
