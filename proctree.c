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
  int new_pid,new_parent;

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
    cmd = p;
    if (p = strchr(cmd,' ')) *p = 0;
    else continue;
    p++;
    if (!strcmp(cmd,"start")){
      if (sscanf(p,"%d %d",&new_pid,&new_parent))
	debug("handle_start(%d,%d)\n",new_pid,new_parent);
    } else if (!strcmp(cmd,"root")){
      if (sscanf(p,"%d",&new_pid))
	debug("handle_root(%d)\n",new_pid);
    } else if (!strcmp(cmd,"comm")){
      if (p2 = strchr(p,'\n')) *p2 = 0;
      debug("handle_comm(%s)\n",p);
    } else if (!strcmp(cmd,"exit")){
      if (p2 = strchr(p,'\n')) *p2 = 0;      
      debug("handle_exit(%s)\n",p);
    } else {
      warning("unknown command: %4.2f %s %s",elapsed,cmd,p);
    }
  }
  return 0;
}
