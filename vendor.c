/*
 * vendor.c - look up processor type
 *
 */
#include <stdio.h>
#include <string.h>
#include "wspy.h"

// look up vendor from /proc/cpuinfo
static char *vendor_id = NULL;
char *lookup_vendor(void){
  FILE *fp;
  char line[1024];
  char *p,*token;
  char *strtokptr = NULL;
  if (vendor_id == NULL){
    if (fp = fopen("/proc/cpuinfo","r")){
      while (fgets(line,sizeof(line),fp)){
	if (!strncmp(line,"vendor_id",9)){
	  p = strchr(line,':');
	  if ((p = strchr(line,':')) &&
	      (token = strtok_r(p+1," \t\n",&strtokptr))){
	    vendor_id = strdup(token);
	  }
	  fclose(fp);
	  return vendor_id;
	}
      }
      fclose(fp);
    }
  }
  return vendor_id;
}
