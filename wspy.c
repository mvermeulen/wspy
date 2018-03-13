/*
 * wspy.c - workload spy
 *
 * This file is the main driver for wspy program, responsible for parsing
 * arguments, starting processes and overall control.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <pwd.h>
#include "wspy.h"
#include "error.h"

pid_t child_pid = 0;
procinfo *child_procinfo = NULL;
pthread_t ktrace_thread;
pthread_t timer_thread;
int cflag = 1;
int fflag = 1;
int rflag = 0;
int uflag = 0;
int uvalue = 0;
char *rvalue = NULL;
int missing_command = 0;
char *default_command[] = { "sleep", "30", NULL };

int parse_options(int argc,char *const argv[],int *program_idx){
  int opt;
  int i;
  FILE *fp;
  struct passwd *pwd;
  outfile = stdout;
  while ((opt = getopt(argc,argv,"+CcFfo:r:u:?")) != -1){
    switch (opt){
    case 'c':
      cflag = 0;
      break;
    case 'C':
      cflag = 1;
      break;
    case 'f':
      fflag = 0;
      break;
    case 'F':
      fflag = 1;
      break;
    case 'o':
      if ((fp = fopen(optarg,"a")) == NULL){
	warning("can not open output file: %s\n",optarg);
      } else {
	outfile = fp;
      }
      break;
    case 'r':
      rflag = 1;
      rvalue = strdup(optarg);
      break;
    case 'u':
      uflag = 1;
      if (strspn(optarg,"0123456789") == strlen(optarg)){
	uvalue = atoi(optarg);
	if (uvalue < 0){
	  error("-u %d is less than 0, ignored\n");
	  uflag = 0;
	}
      } else {
	pwd = getpwnam(optarg);
	if (pwd == NULL){
	  error("-u %s user not found, ignored\n",optarg);
	  uflag = 0;
	} else {
	  uvalue = pwd->pw_uid;
	}
      }
      break;
    case '?':
      return 1;
    default:
      warning("unknown option: '%c'\n",opt);
      break;
    }
  }
  if (optind >= argc){
    warning("missing command, assuming");
    for (i=0;i<sizeof(default_command)/sizeof(default_command[0])-1;i++){
      notice(" %s",default_command[i]);
    }
    notice("\n");
    missing_command = 1;
  } else {
    *program_idx = optind;
  }

  return 0;
}

#define PATHLEN 1024
int child_pipe[2];
int setup_child_process(int argc,char *const argv[],char *const envp[]){
  pid_t child;
  char pathbuf[PATHLEN];
  char *path,*p;
  int len;
  if (pipe(child_pipe) == -1) fatal("pipe creation failed\n");
  switch(child = fork()){
  case 0:
    if (uflag) setuid(uvalue);
    close(child_pipe[1]); // close writing end
    read(child_pipe[0],pathbuf,PATHLEN); // wait until parent has written
    execve(argv[0],argv,envp);
    // if argv[0] fails, try looking in the path
    path = strdup(getenv("PATH"));
    len = strlen(argv[0]);
    p = strtok(path,":\n");
    if (p){
      do {
	strncpy(pathbuf,p,PATHLEN-len-2);
	strcat(pathbuf,"/");
	strcat(pathbuf,argv[0]);
	execve(pathbuf,argv,envp);
	p = strtok(NULL,":\n");
      } while (p);
      // exec failed
      return 1;
    }
    break;
  default:
    close(child_pipe[0]); // close reading end
    child_pid = child;
  }
  return 0;
}

int main(int argc,char *const argv[],char *const envp[]){
  int command_idx = 0;
  int status;
  double basetime = 0;
  pid_t child;
  sigset_t signal_mask;
  initialize_error_subsystem(argv[0],"-");

  if (parse_options(argc,argv,&command_idx)){
    fatal("usage: %s [CcFf][-r name][-u uid] <cmd><args>...\n"
	  "\t-C\tturn on CPU usage tracing (default = on)\n"
	  "\t-c\tturn off CPU usage tracing (default = on)\n"	  
	  "\t-F\tturn on kernel scheduler tracing (default = on)\n"
	  "\t-f\tturn off kernel scheduler tracing (default = on)\n"
	  "\t-r\tfilter for name of process tree root\n"
	  "\t-u\trun <cmd> as user <uid>\n");
  }

  if (missing_command){
    if (setup_child_process(2,default_command,envp)){
      fatal("unable to launch %s\n",default_command[0]);
    }    
  } else {
    if (setup_child_process(argc-command_idx,&argv[command_idx],envp)){
      fatal("unable to launch %s\n",argv[command_idx]);
    }
  }

  // let ^C go to children
  signal(SIGINT,SIG_IGN);

  if (fflag){
    // kernel tracing
    pthread_create(&ktrace_thread,NULL,ktrace_start,&child_pid);
  }
  if (cflag){
    init_cpustatus();
    // periodic timer
    pthread_create(&timer_thread,NULL,timer_start,NULL);
  }

  // wait 5 seconds to allow subsystems to start
  sleep(5);
  child_procinfo = lookup_process_info(child_pid,1);
  read_uptime(&child_procinfo->time_fork);
  basetime = child_procinfo->time_fork.tv_sec + child_procinfo->time_fork.tv_usec / 1000000.0; 
  
  // let the child proceed
  write(child_pipe[1],"start\n",6);

  notice("running until %s completes\n",argv[command_idx]);
  child = waitpid(child_pid,&status,0);
  if (WIFEXITED(status)){
    if (WEXITSTATUS(status) != 0){
      notice("child exited with status %d\n",WEXITSTATUS(status));
    }
  } else if (WIFSIGNALED(status)){
    notice("child signaled %d\n",WTERMSIG(status));
  }

  // wait 5 seconds to allow subsystems to start
  sleep(5);  

  if (fflag){
    write(ktrace_cmd_pipe[1],"quit\n",5);
    pthread_join(ktrace_thread,NULL);
  }

  if (cflag){
    write(timer_cmd_pipe[1],"quit\n",5);
    pthread_join(timer_thread,NULL);
  }

  sleep(5);

  finalize_process_tree();
  if (rflag) basetime = find_first_process_time(rvalue);
  if (fflag)
    print_all_process_trees(basetime,rvalue);
  if (cflag)
    print_cpustatus(basetime);
  return 0;
}

void read_uptime(struct timeval *tm){
  double uptime;
  FILE *fp = fopen("/proc/uptime","r");
  if (fp){
    fscanf(fp,"%lf",&uptime);
    tm->tv_sec = trunc(uptime);
    tm->tv_usec = (uptime - tm->tv_sec)*1000000;
    fclose(fp);
  }
}
