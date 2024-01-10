/*
 * Program to parse the "haswell_core_v27.tsv" file to create a performance
 * counter definition directory.
 *
 * The *.tsv file was retrieved from https://download.01.org/perfmon/HSW/
 *
 * This can probably be generalized with more info, but saved this way for now
 * to do the job I need. Also relatively little error checking since this was
 * mostly an iterative write and use exercise.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define SOURCEFILE "haswell_core_v27.tsv"
#define OUTPUTDIR "haswell"

int main(void){
  int first = 1;
  int col_event = -1;
  int col_umask = -1;
  int col_cmask = -1;
  int col_name = -1;
  int col_description = -1;
  unsigned long event,umask,cmask;
  char *p;
  int colnum;
  FILE *eventfp,*typefp;
  FILE *fp = fopen(SOURCEFILE,"r");
  char buffer[1024];
  char countername[256];
#if !DONE
  mkdir("haswell",0755);
#endif
  chdir("haswell");
#if !DONE
  mkdir("core",0755);
#endif
  chdir("core");
#if !DONE
  // raw counters
  typefp = fopen("type","w");
  fprintf(typefp,"4");
  fclose(typefp);
  mkdir("events",0755);
#endif
  chdir("events");
  while (fgets(buffer,sizeof(buffer),fp)){
    if (buffer[0] == '#') continue;
    colnum = 0;
    if (first){ // header line
      p = strtok(buffer,"\t\n");
      while (p){
	if (!strncmp(p,"EventCode",9)){
	  col_event = colnum;
	  printf("Column %d is event=\n",colnum);
	} else if (!strncmp(p,"UMask",5)){
	  col_umask = colnum;	  
	  printf("Column %d is umask=\n",colnum);
	} else if (!strncmp(p,"EventName",9)){
	  col_name = colnum;	  	  
	  printf("Column %d is Event Name\n",colnum);	  
	} else if (!strncmp(p,"BriefDescription",16)){
	  col_description = colnum;	  	  	  
	  printf("Column %d is Description\n",colnum);
	} else if (!strncmp(p,"CounterMask",11)){
	  col_cmask = colnum;	  
	  printf("Column %d is cmask=\n",colnum);
	}
	colnum++;
	p = strtok(NULL,"\t\n");
      }
      first = 0;
    } else {
      colnum = 0;
      p = strtok(buffer,"\t\n");
      while (p){
	if (colnum == col_event){
	  if (sscanf(p,"%lx",&event) != 1){
	    printf("event? %s\n",p);
	  }
	} else if (colnum == col_umask){
	  if (sscanf(p,"%lx",&umask) != 1){
	    printf("umask? %s\n",p);
	  }	  
	} else if (colnum == col_cmask){
	  if (sscanf(p,"%ld",&cmask) != 1){
	    printf("cmask? %s\n",p);
	  }	  	  
	} else if (colnum == col_name){
	  if (sscanf(p,"%s",countername) != 1){
	    printf("name? %s\n",p);	    
	  }
	}
	colnum++;
	p = strtok(NULL,"\t\n");
      }
      eventfp = fopen(countername,"w");
      if (eventfp){
	fprintf(eventfp,"event=0x%lx",event);
	if (umask != 0) fprintf(eventfp,",umask=0x%lx",umask);
	if (cmask != 0) fprintf(eventfp,",cmask=0x%lx",cmask);
	fclose(eventfp);
      }
    }
  }
  return 0;
}
