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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/stat.h>
#include <pwd.h>
#include "wspy.h"
#include "error.h"

int command_line_argc = 0;
char **command_line_argv = NULL;
int flag_set_uid = 0;
int flag_cmd = 0;
int flag_cpustats = 0;
int flag_debug = 0;
int flag_perfctr = 0;
int flag_require_ftrace = 0;
int flag_require_timer = 0;
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
  int longidx;
  FILE *fp;
  static struct option long_options[] = {
    { "cpustats",        no_argument, 0,       'C' },
    { "no-cpustats",     no_argument, 0,       'c' },
    { "perfcounters",    no_argument, 0,       'P' },
    { "no-perfcounters", no_argument, 0,       'p' },
    { "processtree",     no_argument, 0,       'F' },
    { "no-processtree",  no_argument, 0,       'f' },
    { "debug",           no_argument, 0,       'd' },
    { "root",            required_argument, 0, 'r' },
    { "zip",             required_argument, 0, 'z' },
    { 0, 0, 0, 0 },
  };  
  struct passwd *pwd;
  outfile = stdout;
  optind = 1; // reset optind so this function can be called more than once
  while ((opt = getopt_long(argc,argv,"+CcdFfo:Ppr:u:z:?",long_options,NULL)) != -1){
    switch (opt){
    case 'c':
      flag_cpustats = 0;
      break;
    case 'C':
      flag_cpustats = 1;
      break;
    case 'd':
      flag_debug++;
      if (flag_debug>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 'f':
      flag_proctree = 0;
      break;
    case 'F':
      flag_proctree = 1;
      break;
    case 'o':
      if ((fp = fopen(optarg,"a")) == NULL){
	warning("can not open output file: %s\n",optarg);
      } else {
	outfile = fp;
      }
      break;
    case 'p':
      flag_perfctr = 0;
      break;
    case 'P':
      flag_perfctr = 1;
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
  if (flag_cpustats || flag_perfctr){
    flag_require_timer = 1;
  } else {
    flag_require_timer = 0;    
  }
  flag_require_ftrace = flag_proctree;
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

