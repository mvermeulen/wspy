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
int sumflag = 0;
int vflag = 0;

int main(int argc,char *const argv[],char *const envp[]){
  int opt;
  char *p,*p2,*cmd;
  char *orig_buffer;
  char buffer[1024];
  double elapsed;
  int event_pid;

  initialize_error_subsystem(argv[0],"-");

  // parse options
  while ((opt = getopt(argc,argv,"+SsTtv")) != -1){
    switch(opt){
    case 't':
      treeflag = 0;
      break;
    case 'T':
      treeflag = 1;
      break;
    case 's':
      sumflag = 0;
      break;
    case 'S':
      sumflag = 1;
      break;
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    default:
    usage:
      fatal("usage: %s -[sStTv] file\n"
	    "\t-S\tturn on summary output\n"
	    "\t-s\tturn off summary output (default)\n"
	    "\t-T\tturn on tree output (default)\n"
	    "\t-t\tturn off tree output\n"
	    "\t-v\tverbose messages\n",
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
      notice("handle_fork(%d,%s)\n",elapsed,event_pid,p+5);
    } else if (!strncmp(p,"root",4)){
      notice("handle_root(%d)\n",elapsed,event_pid);      
    } else if (!strncmp(p,"comm",4)){
      notice("handle_comm(%d,%s)\n",elapsed,event_pid,p+5);
    } else if (!strncmp(p,"cmdline",7)){
      notice("handle_cmdline(%d,%s)\n",elapsed,event_pid,p+8);
    } else if (!strncmp(p,"exit",4)){
      notice("handle_exit(%d,%s)\n",elapsed,event_pid,p+8);
    } else {
      warning("unknown command: %4.2f %d %s\n",elapsed,event_pid,p);
    }
  }
  return 0;
}
