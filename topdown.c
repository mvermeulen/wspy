/*
 * topdown.c - topdown performance counter program
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include "error.h"

int cflag = 0;
int oflag = 0;
enum areamode {
  AREA_ALL,
  AREA_BACKEND,
  AREA_FRONTEND,
  AREA_RETIRE,
  AREA_SPEC,
  AREA_IPC
} area = AREA_ALL;

#define USE_IPC   0x1
#define USE_L1    0x2
#define USE_L2b   0x4
#define USE_L2f   0x8
#define USE_L2r   0x10
#define USE_L2s   0x20
struct counterdef {
  char *name;
  unsigned int event;
  unsigned int umask;
  unsigned int use;
};
struct counterdef counters[] = {
  // name                       event umask  use
  { "instructions",             0xc0, 0,     USE_IPC },
  { "cpu-cycles",               0x76, 0,     USE_IPC },
  { "topdown-total-slots",      0,    0,     USE_L1  },
  { "topdown-fetch-bubbles",    0,    0,     USE_L1  },
  { "topdown-recovery-bubbles", 0,    0,     USE_L1  },
  { "topdown-slots-issued",     0,    0,     USE_L1  },
  { "topdown-slots-retired",    0,    0,     USE_L1  },
};
struct countergroup {
  char *label;
  int num_counters;
  char *names[6];
  unsigned int use;
};
struct countergroup groups[] = {
  { "ipc",
    2,
    { "instructions",
      "cpu-cycles" },
    USE_IPC },
  { "level1",
    5,
    { "topdown-total-slots",
      "topdown-fetch-bubbles",
      "topdown-recovery-bubbles",
      "topdown-slots-issued",
      "topdown-slots-retired" },
    USE_L1,
  },
};
  
int command_line_argc;
char **command_line_argv;
pid_t child_pid = 0;

int level = 1;
FILE *outfile;

int parse_options(int argc,char *const argv[]){
  FILE *fp;
  int opt;
  int i;
  unsigned int lev;
  while ((opt = getopt(argc,argv,"abcfil:o:rs")) != -1){
    switch (opt){
    case 'a':
      area = AREA_ALL;
      break;
    case 'b':
      area = AREA_BACKEND;
      break;
    case 'c':
      cflag = 1;
      break;
    case 'f':
      area = AREA_FRONTEND;
      break;
    case 'i':
      area = AREA_IPC;
      break;
    case 'l':
      if (sscanf(optarg,"%u",&lev) == 1){
	if (lev >= 1 && lev <= 4){
	  level = lev;
	} else {
	  error("incorrect option to -l: %u, ignored\n",lev);
	}
      } else {
	error("incorrect option to -l: %s, ignored\n",optarg);	
      }
      break;
    case 'o':
      fp = fopen(optarg,"w");
      if (!fp){
	error("can not open file: %s\n",optarg);
      } else {
	outfile = fp;
	oflag = 1;
      }
      break;
    case 'r':
      area = AREA_RETIRE;
      break;
    case 's':
      area = AREA_SPEC;
      break;
    default:
      warning("unknown option: %d\n",opt);
      return 1;
    }
  }
  if (optind >= argc){
    warning("missing command after options\n");
    return 1;
  }
  command_line_argv = calloc(argc-optind+1,sizeof(char *));
  command_line_argc = argc - optind;
  for (i=0;i<command_line_argc;i++){
    command_line_argv[i] = argv[i+optind];
  }
  return 0;
}

int launch_child(int argc,char *const argv[],char *const envp[]){
  pid_t child;
  int len;
  char *p,*path;
  char pathbuf[1024];
  
  switch(child = fork()){
  case 0: // child
    execve(argv[0],argv,envp);
    // if argv[0] fails, look in path
    path = strdup(getenv("PATH"));
    len = strlen(argv[0]);
    p = strtok(path,":\n");
    if (p){
      do {
	strncpy(pathbuf,p,sizeof(pathbuf)-len-2);
	strcat(pathbuf,"/");
	strcat(pathbuf,argv[0]);
	execve(pathbuf,argv,envp);
	p = strtok(NULL,":\n");
      } while (p);
      // exec failed
      return 1;
    }
    break;
  case -1:
    fatal("fork failed\n");
    return 1;
  default:
    // parent
    child_pid = child;
    break;
  }
  return 0;
}

int main(int argc,char *const argv[],char *const envp[]){
  int status;
  outfile = stdout;
  if (parse_options(argc,argv)){
      fatal("usage: %s -[abcfrs][-l <1|2|3|4>][-o <file>] <cmd><args>...\n"
	    "\t-l <level> - expand out <level> levels (default 1)\n"
	    "\t-c         - show cores as separate\n"
	    "\t-o <file>  - send output to <file>\n"
	    "\t-a         - expand all areas\n"
	    "\t-b         - expand backend stalls area\n"
	    "\t-f         - expand frontend stalls area\n"
	    "\t-r         - expand retiring area\n"
	    "\t-s         - expand speculation area\n"
	    ,argv[0]);
  }
  if (launch_child(command_line_argc,command_line_argv,envp)){
    fatal("unable to launch %s\n",command_line_argv[0]);
  }
  waitpid(child_pid,&status,0);
  if (oflag) fclose(outfile);
  return 0;
}
