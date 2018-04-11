#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include "wspy.h"
#include "error.h"

#define HASHBUCKETS 127
struct proctable_hash_entry {
  procinfo *pinfo;
  struct proctable_hash_entry *next;
};

struct proctable_hash_entry *process_table[HASHBUCKETS] = { 0 };

procinfo *lookup_process_info(pid_t pid,int insert){
  struct proctable_hash_entry *hentry;
  procinfo *pinfo,*ppinfo,*pinfo_sibling;
  for (hentry = process_table[pid%HASHBUCKETS];hentry != NULL;hentry = hentry->next){
    // pid's can wrap around, so skip processes if they've exited
    if ((!flag_require_ptrace || hentry->pinfo->p_exited) &&
	(!flag_require_ftrace || hentry->pinfo->f_exited)) continue;
    //    if (hentry->pinfo->exited) continue;
    if (hentry->pinfo->pid == pid){
      return hentry->pinfo;
    }
  }
  // not found
  if (insert == 0){
    return NULL;
  }

  // splice in a new entry
  pinfo = calloc(1,sizeof(procinfo));
  hentry = calloc(1,sizeof(struct proctable_hash_entry));
  hentry->pinfo = pinfo;
  hentry->next = process_table[pid%HASHBUCKETS];
  process_table[pid%HASHBUCKETS] = hentry;

  pinfo->pid = pid;
  return pinfo;
}

double elapsed_time(procinfo *pinfo){
  struct timeval *start;
  struct timeval *finish;
  double elapsed;
  if (pinfo->time_fork.tv_sec || pinfo->time_fork.tv_usec)
    start = &pinfo->time_fork;
  else
    start = &pinfo->time_exec;
  if (pinfo->time_exit.tv_sec || pinfo->time_exit.tv_usec)
    finish = &pinfo->time_exit;
  else
    finish = &pinfo->time_exec;

  elapsed = (finish->tv_sec - start->tv_sec)+(finish->tv_usec - start->tv_usec)/1000000.0;
  return elapsed;
}

procinfo *reverse_siblings(procinfo *p){
  procinfo *new_p = NULL;
  procinfo *next;
  while (p){
    next = p->sibling;
    p->sibling = new_p;
    new_p = p;
    p = next;
  }
  return new_p;
}

static int clocks_per_second = 0;
void print_process_tree(FILE *output,procinfo *pinfo,int level,double basetime){
  int i;
  double elapsed = elapsed_time(pinfo);
  procinfo *child;
  double on_cpu,on_core;
  unsigned long total_time;
  if (pinfo == NULL) return;
  if (level > 100) return;
  if (clocks_per_second == 0) clocks_per_second = sysconf(_SC_CLK_TCK);

  for (i=0;i<level;i++){
    fprintf(output,"  ");
  }
  fprintf(output,"[%d] cpu=%d",pinfo->pid,pinfo->cpu);
  fprintf(output," elapsed=%5.2f",elapsed);
  if (pinfo->f_exited){
    if ((pinfo->time_fork.tv_sec + pinfo->time_fork.tv_usec) &&
	(pinfo->time_exit.tv_sec + pinfo->time_exit.tv_usec)){
      fprintf(output," start=%5.2f finish=%5.2f",
	      (pinfo->time_fork.tv_sec + pinfo->time_fork.tv_usec / 1000000.0)-basetime,
	      (pinfo->time_exit.tv_sec + pinfo->time_exit.tv_usec / 1000000.0)-basetime);
    }
  }
  if (pinfo->p_exited){
    total_time = pinfo->total_utime + pinfo->total_stime;
    if (total_time){
      on_core = (double) total_time/clocks_per_second/elapsed;
      on_cpu = on_core / num_procs;
      fprintf(output," on_cpu=%3.2f on_core=%3.2f",on_cpu,on_core);
    }
    fprintf(output," user=%4.2f system=%4.2f",
	    pinfo->utime / (double) clocks_per_second,
	    pinfo->stime / (double) clocks_per_second);
    fprintf(output," vsize=%4.0fk",pinfo->vsize/1024.0);
  }
  if (pinfo->filename)
    fprintf(output," %s",pinfo->filename);
  else
    fprintf(output," %s",pinfo->comm);
  fprintf(output," pcount=%d",pinfo->pcount);
  fprintf(output,"\n");
  pinfo->printed = 1;
  for (child = pinfo->child;child;child = child->sibling){
    print_process_tree(output,child,level+1,basetime);
  }
}

// print all the rooted trees
//    if "name" != NULL, then print roots starting at name
//    otherwise print general roots
void print_all_process_trees(FILE *output,double basetime,char *name){
  int i;
  struct proctable_hash_entry *hash;
  for (i=0;i<HASHBUCKETS;i++){
    for (hash = process_table[i];hash;hash = hash->next){
      if (hash->pinfo->printed) continue;
      if (name != NULL){
	if (!hash->pinfo->filename && !hash->pinfo->comm) continue;
	if (hash->pinfo->filename){
	  if (!strcmp(hash->pinfo->filename,name) &&
	      !hash->pinfo->printed){
	    print_process_tree(output,hash->pinfo,0,basetime);
	  }
	} else if (hash->pinfo->comm){
	  if (!strcmp(hash->pinfo->comm,name) &&
	      ((hash->pinfo->parent == NULL) ||
	       (hash->pinfo->parent->comm == NULL) ||
	       (strcmp(hash->pinfo->parent->comm,name) != 0))){
	    print_process_tree(output,hash->pinfo,0,basetime);	    
	  }
	}
      } else {
	// only print top level trees
	if (hash->pinfo->parent) continue;
	print_process_tree(output,hash->pinfo,0,basetime);
      }
    }
  }
}

void sum_counts_processes(procinfo *pinfo);
// make remaining tweaks before printing, e.g. reverse sibling order
void finalize_process_tree(void){
  int i;
  struct proctable_hash_entry *hash;

  // fix the sibling orders
  for (i=0;i<HASHBUCKETS;i++){
    for (hash = process_table[i];hash;hash = hash->next){
      // update the counts
      sum_counts_processes(hash->pinfo);
      // fix up any children
      if (hash->pinfo->child && !hash->pinfo->sibling_order){
	hash->pinfo->child = reverse_siblings(hash->pinfo->child);
	hash->pinfo->sibling_order = 1;
      }
    }
  }
}

// count # of children of a process (including self)
// total the utime and stime
void sum_counts_processes(procinfo *pinfo){
  int pcount;
  unsigned long total_utime;
  unsigned long total_stime;
  procinfo *child;
  if (pinfo->pcount == 0){
    // not yet counted
    pcount = 1;
    total_utime = pinfo->utime;
    total_stime = pinfo->stime;
      
    for (child = pinfo->child;child;child = child->sibling){
      sum_counts_processes(child);
      pcount      += child->pcount;
      total_utime += child->total_utime;
      total_stime += child->total_stime;
    }
    pinfo->pcount = pcount;
    pinfo->total_utime = total_utime;
    pinfo->total_stime = total_stime;
  }
}

// find and return the start time of first process that matches name
double find_first_process_time(char *name){
  int found = 0;
  double first_time,start_time;
  int i;
  struct proctable_hash_entry *hash;
  printf("find_first_process_time(%s)\n",name);
  if (name == NULL) return 0.0;
  for (i=0;i<HASHBUCKETS;i++){
    for (hash = process_table[i];hash;hash = hash->next){
      if (!hash->pinfo->filename && !hash->pinfo->comm) continue;
      if (!strcmp(name,hash->pinfo->filename?
		  hash->pinfo->filename:hash->pinfo->comm)){
	start_time = hash->pinfo->time_fork.tv_sec +
	  hash->pinfo->time_fork.tv_usec / 1000000.0;
	if (!found){
	  first_time = start_time;
	  found = 1;
	} else if (start_time < first_time){
	  first_time = start_time;
	}
      }
    }
  }
  if (found)
    return first_time;
  else
    return 0.0;
}
