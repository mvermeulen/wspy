/*
 * tracecmd.c - invoking and controlling trace-cmd for kernel tracing
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "wspy.h"
#include "error.h"
char *tracecmdline[] = {
  "trace-cmd",
  "record",
  "-e",
  "sched",
  "-o",
  "processtree.dat",
  NULL,
};

pid_t tracecmd_pid = 0;

#define PATHLEN 1024
void *tracecmd_start(void *arg){
  int status;
  int len;
  pid_t child;
  char **envp = arg;
  char pathbuf[PATHLEN];
  char *path,*p;

  notice("NOTE: ignore ^C message coming from trace-cmd\n");
  switch(child = fork()){
  case 0: // child
    // if successful, the execve won't return...
    status = execve(tracecmdline[0],tracecmdline,envp);
    path = strdup(getenv("PATH"));
    len = strlen(tracecmdline[0]);
    p = strtok(path,":\n");
    if (p){
      do {
	strncpy(pathbuf,p,PATHLEN-len-2);
	strcat(pathbuf,"/");
	strcat(pathbuf,tracecmdline[0]);
	status = execve(pathbuf,tracecmdline,envp);
	p = strtok(NULL,":\n");
      } while (p);
    }
    // all the the exec failed
    error("exec of %s failed (errno=%d): %s\n",
	  tracecmdline[0],errno,strerror(errno));
    return 0;
  case -1:
    error("fork for tracecmd failed (errno=%d): %s\n",errno,strerror(errno));
    break;
  default:
    tracecmd_pid = child;
    break;
  }
  pthread_exit(NULL);
}
