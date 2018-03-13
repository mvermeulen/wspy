#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (hentry->pinfo->pid == pid) return hentry->pinfo;
  }
  // not found
  if (insert == 0) return NULL;

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

void print_process_tree(procinfo *pinfo,int level,double basetime){
  int i;
  double elapsed = elapsed_time(pinfo);
  procinfo *child;
  if (pinfo == NULL) return;
  for (i=0;i<level;i++){
    fprintf(outfile,"  ");
  }
  fprintf(outfile,"[%d] cpu=%d elapsed=%5.2f",pinfo->pid,pinfo->cpu,elapsed);
  if ((pinfo->time_fork.tv_sec + pinfo->time_fork.tv_usec) &&
      (pinfo->time_exit.tv_sec + pinfo->time_exit.tv_usec)){
    fprintf(outfile," start=%5.2f finish=%5.2f",
	    (pinfo->time_fork.tv_sec + pinfo->time_fork.tv_usec / 1000000.0)-basetime,
	    (pinfo->time_exit.tv_sec + pinfo->time_exit.tv_usec / 1000000.0)-basetime);
  }
  if (pinfo->filename)
    fprintf(outfile," %s",pinfo->filename);
  else
    fprintf(outfile," %s",pinfo->comm);
  fprintf(outfile,"\n");
  pinfo->printed = 1;
  for (child = pinfo->child;child;child = child->sibling){
    print_process_tree(child,level+1,basetime);
  }
}

// print all the rooted trees
//    if "name" != NULL, then print roots starting at name
//    otherwise print general roots
void print_all_process_trees(double basetime,char *name){
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
	    print_process_tree(hash->pinfo,0,basetime);
	  }
	} else if (hash->pinfo->comm){
	  if (!strcmp(hash->pinfo->comm,name) &&
	      ((hash->pinfo->parent == NULL) ||
	       (hash->pinfo->parent->comm == NULL) ||
	       (strcmp(hash->pinfo->parent->comm,name) != 0))){
	    print_process_tree(hash->pinfo,0,basetime);	    
	  }
	}
      } else {
	// only print top level trees
	if (hash->pinfo->parent) continue;
	print_process_tree(hash->pinfo,0,basetime);
      }
    }
  }
}

// make remaining tweaks before printing, e.g. reverse sibling order
void finalize_process_tree(void){
  // fix the sibling orders
  int i;
  struct proctable_hash_entry *hash;
  for (i=0;i<HASHBUCKETS;i++){
    for (hash = process_table[i];hash;hash = hash->next){
      // fix up any children
      if (hash->pinfo->child && !hash->pinfo->sibling_order){
	hash->pinfo->child = reverse_siblings(hash->pinfo->child);
	hash->pinfo->sibling_order = 1;
      }
    }
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
