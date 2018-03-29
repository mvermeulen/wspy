/*
 * diskstats - program to read and process disk status information
 *             Initially looks at /proc/diskstats
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include "wspy.h"
#include "error.h"

FILE *diskfile = NULL;
struct monitor {
  char *name;
  char *statfile;
  int blocksize;
  struct monitor *next;
} *monitor_list = NULL;

void init_diskstats(void){
  FILE *fp;
  char buffer[1024];
  int major,minor,fd;
  long blocks;
  char name[32];
  char sysname[64];
  struct monitor *mon;
  struct stat statbuf;
  diskfile = tmpfile();
  fp = fopen("/proc/partitions","r");
  if (fp){
    while (fgets(buffer,sizeof(buffer),fp) != NULL){
      if (!strncmp(buffer,"major",5)) continue;
      if (sscanf(buffer,"%d %d %ld %32s",&major,&minor,&blocks,name) == 4){
	snprintf(sysname,sizeof(sysname),"/sys/block/%s",name);
	if (stat(sysname,&statbuf) == -1) continue;
	snprintf(sysname,sizeof(sysname),"/sys/block/%s/stat",name);
	mon = calloc(1,sizeof(struct monitor));
	mon->name = strdup(name);
	mon->statfile = strdup(sysname);
	mon->next = monitor_list;
	monitor_list = mon;
	// default blocksize is 512, but if possible see if we can update it
	snprintf(sysname,sizeof(sysname),"/dev/%s",name);
	if ((fd = open(sysname,O_RDONLY|O_NONBLOCK)) != -1){
	  if (ioctl(fd,BLKSSZGET,&mon->blocksize) != 0){
	    mon->blocksize = 512;
	  }
	  close(fd);
	} else {
	  mon->blocksize = 512;
	  debug("unable to read blocksize for %s, default to 512\n",sysname);
	}
      }
    }
    fclose(fp);
  } else {
    error("unable to open /proc/partitions for diskstats\n");
    return;
  }
}

void read_diskstats(double time){
  FILE *fp;
  char buffer[1024];
  struct monitor *mon;
  fprintf(diskfile,"time %f\n",time);
  for (mon = monitor_list;mon;mon = mon->next){
    fp = fopen(mon->statfile,"r");
    if (fp){
      if (fgets(buffer,sizeof(buffer),fp) != NULL){
	fprintf(diskfile,"%s\t",mon->name);
	fputs(buffer,diskfile);
      }
      fclose(fp);
    }
  }
}

void print_diskinfo(struct monitor *mon,char *delim,FILE *output){
  double elapsed = 0;
  char buffer[1024];
  int len = strlen(mon->name);
  int first = 1;
  int count;
  struct statinfo { long value[11]; } last,curr;
  rewind(diskfile);
  while (fgets(buffer,sizeof(buffer),diskfile) != NULL){
    if (!strncmp(buffer,"time",4)){
      sscanf(buffer,"time %lf",&elapsed);
      continue;
    }
    if (!strncmp(buffer,mon->name,len)){
      last = curr;
      count = sscanf(&buffer[len],"%ld %ld %ld %ld %ld %ld %ld"
		     "%ld %ld %ld %ld %ld %ld %ld",
		     &curr.value[0],&curr.value[1],&curr.value[2],
		     &curr.value[3],&curr.value[4],&curr.value[5],
		     &curr.value[6],&curr.value[7],&curr.value[8],
		     &curr.value[9],&curr.value[10],&curr.value[11],
		     &curr.value[12],&curr.value[13]);
      if (first){
	first = 0;
	fprintf(output,"%s%sreads%sblocks%stime%swrites%sblocks%stime%sttime%swtime\n",
		mon->name,delim,
		delim,delim,delim,delim,delim,delim,delim);
		
      } else {
	fprintf(output,"%-10.2f%s%ld%s%ld%s%ld%s%ld%s%ld%s%ld%s%ld%s%ld\n",
		elapsed,delim,
		(curr.value[0]-last.value[0]),   // # reads completed
		delim,
		(curr.value[2]-last.value[2]),   // # blocks read
		delim,
		(curr.value[3]-last.value[3]),   // time reading ms
		delim,
		(curr.value[4]-last.value[4]),   // # writes completed
		delim,
		(curr.value[6]-last.value[6]),   // # blocks written
		delim,
		(curr.value[7]-last.value[7]),   // time writing ms
		delim,
		(curr.value[9]-last.value[9]),   // time spend in I/O
		delim,
		(curr.value[10]-last.value[10]));// weighted time in I/O
      }
    }

  }
}

void print_diskstats(void){
  struct monitor *mon;
  for (mon = monitor_list;mon;mon = mon->next){
    print_diskinfo(mon,"\t",outfile);
  }
}

void print_diskstats_files(void){
  FILE *fp;
  char monfile[32];
  struct monitor *mon;
  for (mon = monitor_list;mon;mon = mon->next){
    snprintf(monfile,32,"disk-%s.csv",mon->name);
    fp = fopen(monfile,"w");
    if (fp){
      print_diskinfo(mon,",",fp);
      fclose(fp);
    }
  }
}
