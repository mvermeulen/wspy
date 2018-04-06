/*
 * Program to generate event files for hardware cache events
 * Copy the result files to the events subdirectory.
 */
#include <stdio.h>
char *cacheid[] = {
  "PERF_COUNT_HW_CACHE_L1D",
  "PERF_COUNT_HW_CACHE_L1I",
  "PERF_COUNT_HW_CACHE_LL",
  "PERF_COUNT_HW_CACHE_DTLB",
  "PERF_COUNT_HW_CACHE_ITLB",
  "PERF_COUNT_HW_CACHE_BPU",
  "PERF_COUNT_HW_CACHE_NODE"
};

char *opid[] = {
  "_RD",
  "_WR",
  "_PF"
};

char *resid[] = {
  "_ACCESS",
  "_MISS"
};

int main(void){
  FILE *fp;
  int i,j,k;
  char filename[128];
  for (i=0;i<sizeof(cacheid)/sizeof(cacheid[0]);i++){
    for (j=0;j<sizeof(opid)/sizeof(opid[0]);j++){
      for (k=0;k<sizeof(resid)/sizeof(resid[0]);k++){
	snprintf(filename,sizeof(filename),"%s%s%s",
		 cacheid[i],opid[j],resid[k]);
	if (fp = fopen(filename,"w")){
	  fprintf(fp,"event=0x%x,umask=0x%x,result=0x%x\n",i,j,k);
	  fclose(fp);
	}
      }
    }
  }
}
  
