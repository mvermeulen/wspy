/*
 * timer.c - interval timer thread + data collectors enabled
 *
 * Implementation of a thread that wakes up at regular interval to
 * update a timer and then sets condition variables for various collectors
 * when the timer fires.
 */

#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include "wspy.h"
#include "error.h"

int timer_fd;
FILE *cpuinfo = NULL;

static void timer_loop(void);

void *timer_start(void *arg){
  int status;
  struct itimerspec itval;

  pipe(timer_cmd_pipe);

  if (cpuinfo == NULL) cpuinfo = tmpfile();

  // set the timer for once per second
  timer_fd = timerfd_create(CLOCK_MONOTONIC,0);
  itval.it_interval.tv_sec = 1;
  itval.it_interval.tv_nsec = 0;
  itval.it_value.tv_sec = 1;
  itval.it_value.tv_nsec = 0;
  timerfd_settime(timer_fd,0,&itval,NULL);

  // timer_loop()
  timer_loop();

  return NULL;
}

static void timer_loop(void){
  fd_set rdfd_list;
  struct timeval tv,now;
  FILE *cmd_file,*timer_file;
  int maxfd = timer_cmd_pipe[0];
  int status;
  char buffer[1024];
  long long int expirations;

  if (timer_fd > maxfd) maxfd = timer_fd;
  timer_file = fdopen(timer_fd,"r");
  cmd_file = fdopen(timer_cmd_pipe[0],"r");
  
  // loop until a "quit" command is received
  while (1){
    FD_ZERO(&rdfd_list);
    FD_SET(timer_cmd_pipe[0],&rdfd_list);
    FD_SET(timer_fd,&rdfd_list);

    // once a second
    tv.tv_sec = 1;
    tv.tv_usec = 0;

    status = select(maxfd+1,&rdfd_list,NULL,NULL,&tv);

    if (status){
      if (FD_ISSET(timer_cmd_pipe[0],&rdfd_list)){
	fgets(buffer,sizeof(buffer),cmd_file);
	if (!strncmp(buffer,"quit",4)){
	  return;
	}
      }
      if (FD_ISSET(timer_fd,&rdfd_list)){
	read(timer_fd,&expirations,sizeof(expirations));
	read_uptime(&now);
	read_cpustatus(&now);
      }
    }
  }
}
