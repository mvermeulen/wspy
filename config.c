/*
 * config.c - configuration file management and options processing
 *
 * By default check for two configuration files in following order:
 *
 *    $HOME/.wspy/config
 *    /usr/share/wspy/config
 *
 * If the first is found, don't look at the second.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <pwd.h>
#include "wspy.h"
#include "error.h"

static int parse_cpumask(char *arg,cpu_set_t *mask);
int command_line_argc = 0;
char **command_line_argv = NULL;
int flag_set_uid = 0;
int flag_cmd = 0;
int flag_cpustats = 0;
int flag_diskstats = 0;
int flag_memstats = 0;
int flag_netstats = 0;
int flag_debug = 0;
int flag_perfctr = 0;
int flag_proctree = 0;
enum proctree_engine proctree_engine = PT_DEFAULT;
enum perfcounter_model perfcounter_model = PM_DEFAULT;
int flag_require_ftrace = 0;
int flag_require_ptrace = 0;
int flag_require_timer = 0;
int flag_require_counters = 0;
int flag_require_perftree = 0;
int flag_require_perftimer = 0;
int flag_set_cpumask = 0;
int flag_zip = 0;
int uid_value;
char *zip_archive_name = NULL;
char *command_name = NULL;

/* Opens config file, if present and returns file pointer, NULL if not found */
FILE *open_config_file(void){
  FILE *fp;
  char *p;
  char buffer[1024];
  int len = sizeof(buffer);
  int status;
  struct stat statbuf;

  if (p = getenv("HOME")){
    strncpy(buffer,p,1024);
    len -= strlen(p);
  } else {
    buffer[0] = '\0';
  }
  strncat(buffer,"/.wspy/config",len);
  if (stat(buffer,&statbuf) != -1){
    debug("found config file %s",buffer);
    fp = fopen(buffer,"r");
    return fp;
  }
  if (stat("/usr/share/wspy/config",&statbuf) != -1){
    debug("found config file /usr/share/wspy/config",buffer);
    fp = fopen(buffer,"r");
    return fp;    
  }
  notice("No config file found in $HOME/.wspy/config or /usr/share/wspy/config\n");
  return NULL;
}


// reads config file and processes commands
#define MAXARGS 256 // maximum # of saved command line args
void read_config_file(void){
  char buffer[1024];
  int len;
  int cmdargcnt = 0;
  char *cmdarg[MAXARGS]; // use stack space for fixed # of args
  
  FILE *fp = open_config_file();
  char *p;
  if (fp){
    while (fgets(buffer,sizeof(buffer),fp) != NULL){
      if (buffer[0] == '#') continue;
      if (!strncmp(buffer,"command",7)){
	cmdargcnt = 0;
	cmdarg[0] = "wspy";
	cmdargcnt++;
	len = strlen(&buffer[7]);
	p = strtok(&buffer[7]," \t\n");
	while (p){
	  // check just in case, but don't expect this to happen
	  if (cmdargcnt>=MAXARGS)
	    fatal("exceeded argument count for 'command' in config file\n");
	  cmdarg[cmdargcnt] = strdup(p);
	  cmdargcnt++;
	  p = strtok(NULL," \t\n");
	}
	parse_options(cmdargcnt,cmdarg);
      }
    }
    fclose(fp); 
  }
}

// options processing
// long options
int parse_options(int argc,char *const argv[]){
  int opt;
  int i;
  int value;
  int longidx;
  cpu_set_t mask;
  char *p,*arg;
  FILE *fp;
  struct counterinfo *ci;
  static struct option long_options[] = {
    { "cpustats",        no_argument, 0,       10 },
    { "no-cpustats",     no_argument, 0,       11 },
    { "diskstats",       no_argument, 0,       12 },
    { "no-diskstats",    no_argument, 0,       13 },
    { "memstats",        no_argument, 0,       14 },
    { "no-memstats",     no_argument, 0,       15 },
    { "netstats",        no_argument, 0,       16 },
    { "no-netstats",     no_argument, 0,       17 },    
    { "perfcounters",    no_argument, 0,       18 },
    { "no-perfcounters", no_argument, 0,       19 },
    { "processtree",     no_argument, 0,       20 },
    { "no-processtree",  no_argument, 0,       21 },
    { "set-cpumask",     required_argument, 0, 22 },
    { "show-counters",   no_argument, 0,       23 },
    { "no-show-counters",no_argument, 0,       24 },
    { "set-counters",    required_argument, 0, 25 },
    { "counterinfo",     required_argument, 0, 26 },
    { "interval",        required_argument, 0, 27 },
    { "processtree-engine", required_argument,0,28 },
    { "perfcounter-model", required_argument,0,29 },
    { "debug",           no_argument, 0,       'd' },
    { "root",            required_argument, 0, 'r' },
    { "zip",             required_argument, 0, 'z' },
    { "output",          required_argument, 0, 'o' },
    { "error-output",    required_argument, 0,  9 },
    { 0, 0, 0, 0 },
  };  
  struct passwd *pwd;
  outfile = stdout;
  optind = 1; // reset optind so this function can be called more than once
  while ((opt = getopt_long(argc,argv,"+do:r:u:z:?",long_options,NULL)) != -1){
    switch (opt){
    case 10: flag_cpustats = 1;  break;
    case 11: flag_cpustats = 0;  break;
    case 12: flag_diskstats = 1; break;
    case 13: flag_diskstats = 0; break;
    case 14: flag_memstats = 1;  break;
    case 15: flag_memstats = 0;  break;
    case 16: flag_netstats = 1;  break;
    case 17: flag_netstats = 0;  break;
    case 18: flag_perfctr = 1;   break;
    case 19: flag_perfctr = 0;   break;
    case 20: flag_proctree = 1;  break;
    case 21: flag_proctree = 0;  break;
    case 22: flag_setcpumask = 1;
      if (parse_cpumask(optarg,&mask)){
	warning("invalid argument to --set-cpumask, ignored\n");
	flag_setcpumask = 0;
      } else {
	cpumask = mask;
      }
      break;
    case 23: flag_showcounters = 1; break;
    case 24: flag_showcounters = 0; break;
    case 25:
      // just in time, read the built-in counters
      if (flag_require_counters == 0){
	  inventory_counters(0);
	  flag_require_counters = 1;
      }
      if (p = strchr(optarg,':')){
	// cpulist
	*p = 0;
	arg = p+1;
	parse_cpumask(optarg,&mask);
      } else {
	arg = optarg;
	for (i=0;i<num_procs;i++){
	  CPU_SET(i,&mask);
	}
      }
      // delete the old counter lists (for now set to zero, slight memory leak)
      for (i=0;i<num_procs;i++){
	if (CPU_ISSET(i,&mask))
	  perf_counters_by_cpu[i] = 0;
      }
      
      p = strtok(arg,", \t\n");
      while (p){
	if ((ci = counterinfo_lookup(p,0,0)) == NULL){
	  warning("unknown performance counter, ignored: %s\n",p);
	} else {
	  for (i=0;i<num_procs;i++){
	    if (CPU_ISSET(i,&mask)){
	      // build up lists in reverse order
	      struct counterlist *cl = calloc(1,sizeof(struct counterlist));
	      cl->name = strdup(p);
	      cl->next = perf_counters_by_cpu[i];
	      perf_counters_by_cpu[i] = cl;
	    }
	  }
	}
	p = strtok(NULL," ,\t\n");
      }
      break;
    case 26:
      inventory_counters(optarg);
      break;
    case 27:
      if ((sscanf(optarg,"%d",&value) != 1)||(value < 1)){
	warning("invalid interval, ignored: %s\n",optarg);
      } else {
	timer_interval = value;
      }
      break;
    case 28:
      if (!strncmp(optarg,"ftrace",6)){
	proctree_engine = PT_FTRACE;
      } else if (!strncmp(optarg,"ptrace",6)){
	proctree_engine = PT_PTRACE;
      } else if (!strncmp(optarg,"all",3)){
	proctree_engine = PT_ALL;
      } else {
	warning("invalid argument to --processtree-engine, ignored: %s\n"
		"\texpecting either ftrace or ptrace\n"
		,optarg);
      }
      break;
    case 29:
      if (!strncmp(optarg,"core",4)){
	perfcounter_model = PM_CORE;
      } else if (!strncmp(optarg,"process",7)){
	perfcounter_model = PM_PROCESS;
      } else {
	warning("invalid argument to --perfcounter-model, ignored: %s\n"
		"\texpecting either core or process\n",
		optarg);
      }
      break;
    case 'd':
      flag_debug++;
      if (flag_debug>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 9:
      if ((fp = fopen(optarg,"a")) == NULL){
	warning("can not open error output file: %s\n",optarg);	
      } else {
	
      }
      break;
    case 'o':
      if ((fp = fopen(optarg,"a")) == NULL){
	warning("can not open output file: %s\n",optarg);
      } else {
	set_error_stream(fp);
      }
      break;
    case 'r':
      flag_cmd = 1;
      command_name = strdup(optarg);
      break;
    case 'u':
      flag_set_uid = 1;
      if (strspn(optarg,"0123456789") == strlen(optarg)){
	uid_value = atoi(optarg);
	if (uid_value < 0){
	  error("-u %d is less than 0, ignored\n");
	  flag_set_uid = 0;
	}
      } else {
	pwd = getpwnam(optarg);
	if (pwd == NULL){
	  error("-u %s user not found, ignored\n",optarg);
	  flag_set_uid = 0;
	} else {
	  uid_value = pwd->pw_uid;
	}
      }
      break;
    case 'z':
      flag_zip = 1;
      zip_archive_name = strdup(optarg);
      break;
    case '?':
      return 1;
    default:
      warning("unknown option: '%c'\n",opt);
      break;
    }
  }
  if (flag_proctree){
    switch(proctree_engine){
    case PT_DEFAULT:
    case PT_ALL:
      flag_require_ftrace = 1;
      flag_require_ptrace = 1;
      break;
    case PT_FTRACE:
      flag_require_ftrace = 1;
      flag_require_ptrace = 0;
      break;
    case PT_PTRACE:
      flag_require_ftrace = 0;
      flag_require_ptrace = 1;
      break;
    }
  }
  if (flag_perfctr){
    switch(perfcounter_model){
    case PM_DEFAULT:
    case PM_CORE:
      flag_require_perftimer = 1;
      flag_require_perftree = 0;
      break;
    case PM_PROCESS:
      flag_require_perftimer = 0;
      flag_require_perftree = 1;
      break;
    }
  } else {
    flag_require_perftimer = 0;
    flag_require_perftree = 0;
  }
  if (flag_cpustats || flag_diskstats || flag_require_perftimer){
    flag_require_timer = 1;
  } else {
    flag_require_timer = 0;    
  }
  if (argc > optind){
    if (command_line_argc != 0){
      warning("command line given twice\n");
      notice_noprogram("\toriginal:");
      for (i=0;i<command_line_argc;i++){
	notice_noprogram(" %s",command_line_argv[i]);
      }
      notice_noprogram("\n");
      notice_noprogram("\tnew     :");
      for (i=optind;i<argc;i++){
	notice_noprogram(" %s",argv[i]);
      }
      notice_noprogram("\n");      
    }
    command_line_argv = calloc(argc-optind+1,sizeof(char *));
    command_line_argc = argc - optind;
    for (i=0;i<command_line_argc;i++){
      command_line_argv[i] = argv[i+optind];
    }
  }

  return 0;
}

static int parse_cpumask(char *arg,cpu_set_t *mask){
  int i;
  char *buffer = strdup(arg);
  int start,stop;
  char *p,*p2;
  int anyset;
  cpu_set_t new_mask;
  CPU_ZERO(&new_mask);
  p = strtok(buffer,",\n");
  while (p){
    if (sscanf(p,"%d",&start) == 1){
      if ((p2 = strchr(p,'-')) &&
	  (sscanf(p2+1,"%d",&stop) == 1)){
	// range
	for (i=start;i<=stop;i++){
	  CPU_SET(i,&new_mask);
	}
      } else if ((start >= 0) && (start < num_procs)){
	CPU_SET(start,&new_mask);
      } else {
	return 1;
      }
    } else {
      return 1;
    }
    p = strtok(NULL,",\n");
  }
  free(buffer);
  anyset = 0;
  debug("affinity_mask:\n");
  for (i=0;i<num_procs;i++){
    if (CPU_ISSET(i,&new_mask)){
      debug("\t%d\n",i);
      anyset = 1;
    }
  }
  if (anyset){
    *mask = new_mask;
  }
  else return 1;
  return 0;
}
