/*
 * memstats - program to read and process memory status information
 *             Initially looks at /proc/meminfo
 */

#include <stdio.h>
#include <string.h>
#include "wspy.h"
#include "error.h"

FILE *memfile = NULL;

void init_memstats(void){
  memfile = tmpfile();
}

void read_memstats(double time){
  FILE *fp = fopen("/proc/meminfo","r");
  char buffer[1024];

  debug("read_memstats(%5.2lf)\n",time);
  
  fprintf(memfile,"time %f\n",time);
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    fputs(buffer,memfile);
  }
  fclose(fp);
}

char *labels[] = {
  "MemTotal",
  "MemFree",
  "Buffers",
  "Cached",
  "Active(anon)",
  "Inactive(anon)",
  "Active(file)",
  "Inactive(file)",
  "SwapTotal",
  "SwapFree",
  "Dirty",
  "Writeback",
  "AnonPages",
  "Mapped",
  "Shmem",
  "Slab",
  "CommitLimit",
  "Committed_AS"
};
int label_len[sizeof(labels)/sizeof(labels[0])];

void print_meminfo(char *delim,FILE *output){
  int i;
  int first,found;
  char *p;
  char buffer[1024];
  double elapsed;
  long int value;
  long int values[sizeof(labels)/sizeof(labels[0])];
  
  rewind(memfile);
  fputs("time",output);
  for (i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
    label_len[i] = strlen(labels[i]);
    fputs(delim,output);
    fputs(labels[i],output);
    values[i] = 0;
  }
  fputs("\n",output);

  first = 1;
  while (fgets(buffer,sizeof(buffer),memfile) != NULL){
    if (!strncmp(buffer,"time",4)){
      if (first){
	first = 0;
      } else {
	fprintf(output,"%-10.2f",elapsed);
	for (i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
	  fprintf(output,"%s%lu",delim,values[i]);
	}
	fprintf(output,"\n");
      }
      sscanf(buffer,"time %lf",&elapsed);
      continue;
    }
    found = 0;
    for (i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
      if (!strncmp(buffer,labels[i],label_len[i])){
	p = strchr(buffer,':');
	p++;
	if (sscanf(p,"%lu",&value) == 1){
	  values[i] = value;
	  found = 1;
	  break;
	}
      }
    }
    if (!found){
      debug2("unexpected memstat line: %s",buffer);
    }
  }
  fprintf(output,"%-10.2f",elapsed);
  for (i=0;i<sizeof(labels)/sizeof(labels[0]);i++){
    fprintf(output,"%s%lu",delim,values[i]);
  }
  fprintf(output,"\n");  
}

void print_memstats(void){
  print_meminfo(" \t",outfile);
}

void print_memstats_files(void){
  FILE *fp = fopen("meminfo.csv","w");
  print_meminfo(",",fp);
  fclose(fp);
}
