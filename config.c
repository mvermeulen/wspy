/*
 * config.c - configuration file management
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
#include <sys/stat.h>
#include "wspy.h"
#include "error.h"

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
