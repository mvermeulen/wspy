/*
 * timer.c - interval timer thread + data collectors enabled
 *
 * Implementation of a thread that wakes up at regular interval to
 * update a timer and then sets condition variables for various collectors
 * when the timer fires.
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <sys/timerfd.h>
#include <sys/time.h>
#include "wspy.h"
#include "error.h"

int timer_fd;
int timer_interval = 1000;
int time_cmd_pipe[2];
FILE *cpuinfo = NULL;
double now = 0;

static void timer_loop(void);

void *timer_start(void *arg){
  int status;
  struct itimerspec itval;

  if (arg){
    now = ((double *) arg)[0];
  }

  pipe(timer_cmd_pipe);

  if (cpuinfo == NULL) cpuinfo = tmpfile();

  // set the timer for once per second
  timer_fd = timerfd_create(CLOCK_MONOTONIC,0);
  itval.it_interval.tv_sec = timer_interval/1000;
  itval.it_interval.tv_nsec = (timer_interval%1000)*1000000;
  itval.it_value.tv_sec = timer_interval/1000;
  itval.it_value.tv_nsec = (timer_interval%1000)*1000000;
  timerfd_settime(timer_fd,0,&itval,NULL);

  // timer_loop()
  timer_loop();

  return NULL;
}

static void timer_loop(void){
  fd_set rdfd_list;
  struct timeval tv;
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
    tv.tv_sec = timer_interval/1000;
    tv.tv_usec = (timer_interval%1000)*1000000;

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
	now = now + ((double) timer_interval)/1000;
	if (flag_cpustats) read_cpustats(now);
	if (flag_diskstats) read_diskstats(now);
	if (flag_memstats) read_memstats(now);
	if (flag_require_perftimer) read_global_perf_counters(now);
      }
    }
  }
}

void read_uptime(double *td){
  double uptime;
  FILE *fp = fopen("/proc/uptime","r");
  if (fp){
    fscanf(fp,"%lf",&uptime);
    if (td){
      *td = uptime;
    }
    fclose(fp);
  }
}
