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
  double elapsed = pinfo->time_finish - pinfo->time_start;
  procinfo *child;
  double on_cpu,on_core;
  unsigned long total_time;
  unsigned long fetch_bubbles,total_slots,slots_issued,slots_retired,recovery_bubbles;
  unsigned long cpu_cycles;
  double frontend_bound,retiring,speculation,backend_bound;
  char *vendor;
  vendor = lookup_vendor();
  if (pinfo == NULL) return;
  if (level > 100) return;
  if (clocks_per_second == 0) clocks_per_second = sysconf(_SC_CLK_TCK);

  for (i=0;i<level;i++){
    fprintf(output,"  ");
  }
  if (pinfo->cloned)
    fprintf(output,"(%d)",pinfo->pid);
  else
    fprintf(output,"[%d]",pinfo->pid);
  if (pinfo->filename)
    fprintf(output," %-12s",pinfo->filename);
  else
    fprintf(output," %-12s",pinfo->comm);
  fprintf(output," cpu=%d",pinfo->cpu);
  if (flag_require_perftree && flag_require_ptrace){
    fprintf(output," ipc=%4.2f",
	    (double) pinfo->total_counter[0] / pinfo->total_counter[1]);
    total_time = pinfo->total_utime + pinfo->total_stime;
    if (total_time){
      on_core = (double) total_time/clocks_per_second/elapsed;
      on_cpu = on_core / num_procs;
      fprintf(output," on_cpu=%3.2f on_core=%3.2f",on_cpu,on_core);
    }
    // Intel topdown metrics, note scaling wasn't done yet so do it here
    if (vendor && !strcmp(vendor,"GenuineIntel")){
      total_slots = pinfo->total_counter[1]*2;
      fetch_bubbles = pinfo->total_counter[2];
      recovery_bubbles = pinfo->total_counter[3]*2;
      slots_issued = pinfo->total_counter[4];
      slots_retired = pinfo->total_counter[5];
      frontend_bound = (double) fetch_bubbles / total_slots;
      retiring = (double) slots_retired / total_slots;
      speculation = (double) (slots_issued - slots_retired + recovery_bubbles) / total_slots;
      backend_bound = 1 - (frontend_bound + retiring + speculation);
      fprintf(output," retire=%4.3f",retiring);
      fprintf(output," frontend=%4.3f",frontend_bound);
      fprintf(output," spec=%4.3f",speculation);
      fprintf(output," backend=%4.3f",backend_bound);
    } else if (vendor && !strcmp(vendor,"AuthenticAMD")){
      cpu_cycles = pinfo->total_counter[1];
      frontend_bound = pinfo->total_counter[2];
      backend_bound  = pinfo->total_counter[3];
      fprintf(output," frontend=%4.3f",frontend_bound / cpu_cycles);
      fprintf(output," backend=%4.3f",backend_bound / cpu_cycles);      
    }
    
    if (get_error_level() >= ERROR_LEVEL_DEBUG){
      fprintf(output," inst=%lu cycles=%lu",
	      pinfo->total_counter[0],pinfo->total_counter[1]);
    }
  }
  fprintf(output," elapsed=%5.2f",elapsed);

  if (pinfo->f_exited){
    fprintf(output," start=%5.2f finish=%5.2f",pinfo->time_start-basetime,pinfo->time_finish-basetime);    
  } else if (pinfo->p_exited){
    fprintf(output," user=%4.2f system=%4.2f",
	    pinfo->utime / (double) clocks_per_second,
	    pinfo->stime / (double) clocks_per_second);
  }
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

// dump process csv
// - dump all process info; return count of # of records
int print_all_processes_csv(FILE *output){
  int count = 0;
  int i,j;
  procinfo *pinfo;
  struct counterlist *cl;
  struct proctable_hash_entry *hash;
  fprintf(output,"#pid,ppid,filename,starttime,start,finish,cpu,utime,stime,cutime,cstime,vsize,rss,minflt,majflt,num_counters,");
  for (j=0;j<NUM_COUNTERS_PER_PROCESS;j++){
    fprintf(output,"counter%d,",j);
  }
  fprintf(output,"\n");
  for (i=0;i<HASHBUCKETS;i++){
    for (hash = process_table[i];hash;hash = hash->next){
      pinfo = hash->pinfo;
      fprintf(output,"%d,%d,",pinfo->pid,pinfo->ppid);
      if (pinfo->filename) fprintf(output,"%s,",pinfo->filename);
      else if (pinfo->comm) fprintf(output,"%s,",pinfo->comm);
      else fprintf(output,"?,");
      fprintf(output,"%llu,%6.5f,%6.5f,",pinfo->starttime,pinfo->time_start,pinfo->time_finish);
      fprintf(output,"%d,",pinfo->cpu);
      fprintf(output,"%lu,%lu,%lu,%lu,",pinfo->utime,pinfo->stime,pinfo->cutime,pinfo->cstime);
      fprintf(output,"%lu,%lu,",pinfo->vsize,pinfo->rss);
      fprintf(output,"%lu,%lu,",pinfo->minflt,pinfo->majflt);
      fprintf(output,"%d,",NUM_COUNTERS_PER_PROCESS);
      for (j=0;j<NUM_COUNTERS_PER_PROCESS;j++){
	if ((cl = perf_counters_by_process[j]) && cl->ci->scale){
	  fprintf(output,"%lu,",pinfo->pci.perf_counter[j]*cl->ci->scale);	  
	} else {
	  fprintf(output,"%lu,",pinfo->pci.perf_counter[j]);
	}
      }
      fprintf(output,"\n");
      count++;
    }
  }
  return count;
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
// total the performance counter times
void sum_counts_processes(procinfo *pinfo){
  int i;
  int pcount;
  unsigned long total_utime;
  unsigned long total_stime;
  unsigned long total_counter[NUM_COUNTERS_PER_PROCESS];
  procinfo *child;
  if (pinfo->pcount == 0){
    // not yet counted
    pcount = 1;
    total_utime = pinfo->utime;
    total_stime = pinfo->stime;
    for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++)
      total_counter[i] = pinfo->pci.perf_counter[i];
      
    for (child = pinfo->child;child;child = child->sibling){
      sum_counts_processes(child);
      pcount      += child->pcount;
      total_utime += child->total_utime;
      total_stime += child->total_stime;
      for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++)
	total_counter[i] += child->total_counter[i];
    }
    pinfo->pcount = pcount;
    pinfo->total_utime = total_utime;
    pinfo->total_stime = total_stime;
    for (i=0;i<NUM_COUNTERS_PER_PROCESS;i++)
      pinfo->total_counter[i] = total_counter[i];
  }
}
