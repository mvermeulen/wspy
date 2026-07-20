/*
 * proctree - process the output format from topdown --tree
 *
 * Every line is "<time> <pid> <event> [args...]" (time and pid always come
 * first, before the event keyword -- not "<time> <event> <pid>"). Lines this
 * parser acts on:
 *
 * <time> <pid> root
 * <time> <pid> fork <child_pid>
 * <time> <pid> comm <name>
 * <time> <pid> cmdline <arg0> <arg1> ...
 * <time> <pid> exec <resolved /proc/<pid>/exe target, or "?">
 * <time> <pid> exit <pid> <contents of /proc/<pid>/stat>
 * <time> <pid> futex <blocking_wait_count> <total_wait_seconds>
 * <time> <pid> io_wait <read_count> <read_seconds> <write_count> <write_seconds>
 * <time> <pid> io <rchar> <wchar> <syscr> <syscw> <read_bytes> <write_bytes> <cancelled_write_bytes>
 * <time> <pid> schedstat <cpu_seconds> <run_delay_seconds> <nr_timeslices>
 * <time> <pid> vmsize <vm_hwm_kb> <rss_anon_kb> <rss_file_kb> <rss_shmem_kb> <vm_swap_kb>
 * <time> <pid> connect <count> <seconds>
 * <time> <pid> nanosleep <count> <seconds>
 * <time> <pid> wait <count> <seconds>
 * <time> <pid> poll <count> <seconds>
 *
 * The "futex"/"io_wait"/"io"/"schedstat"/"vmsize"/"connect"/"nanosleep"/
 * "wait"/"poll" lines (--tree-futex/--tree-io-wait/--tree-io/
 * --tree-schedstat/--tree-vmsize/--tree-connect/--tree-nanosleep/
 * --tree-wait/--tree-poll, at most one line per pid/thread per flag) are
 * written *before* that pid's "exit" line in the raw file, not after -- see
 * handle_exit()'s comment on why that ordering matters to this parser
 * specifically. "io_wait" is checked before "io" in the dispatch loop below
 * since "io" is a prefix of "io_wait" (same reason "exited" is checked
 * before "exit").
 *
 * Lines this parser reads but deliberately ignores (see the main parse loop
 * below): "<time> <pid> exited", "<time> <pid> signal <n>", "<time> <pid>
 * unknown <status hex>". wspy also writes "<time> <pid> open <path>"
 * (--tree-open) and "<time> <pid> continued" lines; neither is recognized
 * by name here, so each one logs an "unknown command" warning rather than
 * being silently skipped. A trailing "# ptrace-summary ..." comment line
 * closes the file.
 */

#include <stdio.h>
#include <unistd.h>
#include <getopt.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "error.h"
#include "json_util.h"
#include "json_reader.h"

/* SemVer of the --json/--diff --json document *shape* (independent of any
 * wspy manifest/run-index schema version) -- bump when fields are added/
 * removed/renamed in print_json()/run_diff()'s JSON output, matching
 * MANIFEST_SCHEMA_VERSION's (manifest.h) versioning convention. */
#define PROCTREE_JSON_SCHEMA_VERSION "1.0.0"

/* getopt_long() values for --json/--diff/--diff-threshold: deliberately
 * >=256 rather than reusing a short-option letter (the short optstring
 * below is nearly fully allocated) -- this avoids the exact getopt_long
 * val-collision bug CLAUDE.md documents was found and fixed in wspy.c
 * (--no-phase-detect/--tree-connect had colliding vals with '?'/a stray
 * 'S', so a malformed flag matched the wrong case instead of falling
 * through to the usage error). */
#define OPT_JSON 256
#define OPT_DIFF 257
#define OPT_DIFF_THRESHOLD 258

FILE *process_file = NULL;
int treeflag = 1;
int statflag = 1;
int mflag = 0;
int uflag = 0;
int vflag = 0;
int fflag = 1;
int nflag = 0;
int pflag = 0;
int futexflag = 0;
int iflag = 0;
int bflag = 0;
int dflag = 0;
int rflag = 0;
int kflag = 0;
int jflag = 0;
int lflag = 0;
int zflag = 0;
int json_flag = 0; // --json: emit one JSON document instead of the text tree/summary
int diff_flag = 0; // --diff: run-to-run tree diff mode (takes two --json-exported files)
double diff_threshold = 0.01; // --diff-threshold <seconds>: matched-node |delta utime+stime| above which a node counts as "changed"
int output_width = 80;
int print_cmdline = 0;
int clocks_per_second;
long page_size = 4096; // sysconf(_SC_PAGESIZE) at startup; rss (/proc/<pid>/stat) is in pages, not bytes

/* process_info - maintained for each process */
struct process_info {
  unsigned int pid;
  char *comm; // short version of the command
  char *cmdline; // long command line
  double start,finish;
  // selected data from /proc/<pid>/stat
  unsigned int utime;
  unsigned int stime;
  unsigned long long starttime;
  unsigned int vsize;
  unsigned int processor;
  unsigned int exit_code;
  unsigned int ppid;
  unsigned int num_threads;
  unsigned long rss; // pages -- multiply by page_size for bytes
  // --tree-futex (topdown.c): count/total duration of blocking futex waits,
  // present only if wspy was run with --tree-futex -- 0/0.0 otherwise,
  // same as any pid that made no blocking futex calls at all.
  unsigned long long futex_wait_count;
  double futex_wait_seconds;
  // --tree-io-wait (topdown.c): count/total duration of blocking read-side
  // vs write-side syscalls, split into two buckets -- present only if wspy
  // was run with --tree-io-wait, same "0/0.0 otherwise" convention as futex.
  unsigned long long io_read_wait_count;
  double io_read_wait_seconds;
  unsigned long long io_write_wait_count;
  double io_write_wait_seconds;
  // --tree-io (topdown.c): /proc/<pid>/io byte-volume counters, present
  // only if wspy was run with --tree-io -- 0 otherwise, indistinguishable
  // from a genuinely idle process's real zero counts (same convention as
  // futex/io_wait: this parser can't tell "not collected" from "measured
  // zero" any more than the raw file itself can).
  unsigned long long io_rchar,io_wchar;
  unsigned long long io_syscr,io_syscw;
  unsigned long long io_read_bytes,io_write_bytes,io_cancelled_write_bytes;
  // --tree-schedstat (topdown.c): run-queue delay + timeslice count from
  // /proc/<pid>/schedstat, present only if wspy was run with
  // --tree-schedstat -- 0/0.0 otherwise, same convention as futex/io_wait/
  // io above. sched_cpu_seconds is parsed and kept here but deliberately
  // never printed (see print_tree()'s comment) -- -U's utime/stime already
  // cover "how much CPU time", this field would just be a second,
  // differently-quantized measurement of nearly the same thing.
  double sched_cpu_seconds;
  double sched_rundelay_seconds;
  unsigned long long sched_nr_timeslices;
  // --tree-vmsize (topdown.c): peak RSS + anon/file/shmem RSS composition +
  // swap from /proc/<pid>/status, present only if wspy was run with
  // --tree-vmsize -- 0 otherwise, same convention as futex/io_wait/io/
  // schedstat above. All in kB, the file's own native unit.
  unsigned long vm_hwm_kb,rss_anon_kb,rss_file_kb,rss_shmem_kb,vm_swap_kb;
  // --tree-connect/--tree-nanosleep/--tree-wait/--tree-poll (topdown.c):
  // count/total duration of each syscall family, present only if wspy was
  // run with the matching flag -- 0/0.0 otherwise, same convention as
  // futex/io_wait above.
  unsigned long long connect_count;
  double connect_seconds;
  unsigned long long nanosleep_count;
  double nanosleep_seconds;
  unsigned long long wait_count;
  double wait_seconds;
  unsigned long long poll_count;
  double poll_seconds;
  struct process_info *parent;
  struct process_info *children; // linked using the "sibling" relationship
  struct process_info *older_sibling; // elder sibling
  struct process_info *younger_sibling; // younger sibling
};

struct process_info *root_process = NULL;

/* proc_table - table of currently running processes
 *    Note: Processes are removed once they exit since the pid can be reused.
 */
#define NUM_PROC_TABLE_BUCKETS 101
int num_processes = 0;
int total_processes = 0;
int max_num_processes = 0;
struct proc_table_entry {
  unsigned int pid;
  struct process_info *pinfo;
  struct proc_table_entry *prev,*next;
};
struct proc_table_entry *proc_table[NUM_PROC_TABLE_BUCKETS] = { 0 };

// lookup an entry and insert if necessary
struct proc_table_entry *lookup_pid(unsigned int pid,int insert){
  int bucket = pid % NUM_PROC_TABLE_BUCKETS;
  struct proc_table_entry *pentry;

  for (pentry=proc_table[bucket];pentry;pentry=pentry->next){
    if (pentry->pid == pid) return pentry;
  }
  // not found
  if (!insert) return NULL;

  pentry = calloc(1,sizeof(struct proc_table_entry));
  pentry->pid = pid;
  pentry->pinfo = calloc(1,sizeof(struct process_info));
  pentry->pinfo->pid = pid;
  if (proc_table[bucket]){
    pentry->next = proc_table[bucket];
    pentry->next->prev = pentry;
  }
  proc_table[bucket] = pentry;
  num_processes++;
  total_processes++;
  if (num_processes > max_num_processes)
    max_num_processes = num_processes;
  return pentry;
}

// remove an entry, return 0 if successful
int remove_pid(unsigned int pid){
  int bucket = pid % NUM_PROC_TABLE_BUCKETS;
  int found = 0;
  
  struct proc_table_entry *pentry;
  for (pentry=proc_table[bucket];pentry;pentry=pentry->next){
    if (pentry->pid == pid){
      found = 1;
      break;
    }
  }
  if (!found) return 1;

  if (pentry->next){
    pentry->next->prev = pentry->prev;
  }
  if (pentry->prev){
    pentry->prev->next = pentry->next;
  } else {
    proc_table[bucket] = pentry->next;
  }
  num_processes--;
  free(pentry);
  return 0;
}

// format: elapsed pid root
void handle_root(double elapsed,unsigned int pid){
  debug("handle_root(%u)\n",pid);

  struct proc_table_entry *pentry = lookup_pid(pid,1);
  root_process = pentry->pinfo;
}

// format: elapsed pid fork child_pid
void handle_fork(double elapsed,unsigned int pid,char *child){
  debug("handle_fork(%d,%s)\n",elapsed,pid,child);  
  unsigned int child_pid = atoi(child);
  struct process_info *pinfo,*parent_pinfo;
  struct proc_table_entry *pentry = lookup_pid(child_pid,1);
  struct proc_table_entry *parent_pentry = lookup_pid(pid,1);
  if (pentry) pentry->pinfo->start = elapsed;
  if (pentry && parent_pentry){
    pinfo = pentry->pinfo;
    parent_pinfo = parent_pentry->pinfo;

    if (pinfo->parent != NULL){
      if (pinfo->parent == parent_pinfo){
	debug2("duplicate fork edge ignored for pid=%u parent=%u\n",child_pid,pid);
	return;
      }
	warning("pid %u already has parent %u, ignoring alternate parent %u\n",
		child_pid,pinfo->parent->pid,pid);
	return;
	}
    
    pinfo->parent = parent_pinfo;
    
    if (parent_pinfo->children){
      pinfo->older_sibling = parent_pinfo->children;
      pinfo->older_sibling->younger_sibling = pinfo;
    }
    parent_pinfo->children = pinfo;
  }
}

// parse the /proc/<pid>/stat line and add information to the pinfo node

#define NUM_STAT_FIELDS 52
void parse_stat(char *stat,struct process_info *pinfo){
  debug2("parse_stat: %s\n",stat);
  char *p;
  int i;
  int index = 0;
  char *stat_fields[NUM_STAT_FIELDS] = { 0 };
  char *stat_line = strdup(stat);
  p = strtok(stat_line," \n");
  while (p && index < NUM_STAT_FIELDS){
    stat_fields[index] = p;
    p = strtok(NULL," \n");
    index++;
  }
  for (i=0;i<index;i++){
    debug2("\t%d: %s\n",i,stat_fields[i]);
  }
  if (stat_fields[3]) pinfo->ppid = atoi(stat_fields[3]); // parent pid
  if (stat_fields[13]) pinfo->utime = atoi(stat_fields[13]); // utime user time in clock ticks
  if (stat_fields[14]) pinfo->stime = atoi(stat_fields[14]); // stime system time in clock ticks
  if (stat_fields[19]) pinfo->num_threads = atoi(stat_fields[19]); // thread count at exit
  if (stat_fields[21]) pinfo->starttime = atoll(stat_fields[21]); // starttime since boot in clock ticks
  if (stat_fields[22]) pinfo->vsize = atoi(stat_fields[22]); // vsize - virtual memory in bytes
  if (stat_fields[23]) pinfo->rss = atoll(stat_fields[23]); // rss - resident set size, in pages
  if (stat_fields[38]) pinfo->processor = atoi(stat_fields[38]); // last processor
  if (stat_fields[51]) pinfo->exit_code = atoi(stat_fields[51]); // exit code
  free(stat_line);
}

// format: elapsed pid futex <blocking_wait_count> <total_wait_seconds>
// Must be looked up (not inserted) like handle_comm()/handle_cmdline() --
// this line only ever arrives for a pid already in the table, and always
// before that pid's "exit" line (topdown.c writes it there specifically so
// handle_exit()'s remove_pid() below hasn't yet dropped this pid from
// proc_table by the time this runs).
void handle_futex(double elapsed,unsigned int pid,char *rest){
  debug("handle_futex(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf",&pentry->pinfo->futex_wait_count,&pentry->pinfo->futex_wait_seconds);
  }
}

// format: elapsed pid io_wait <read_count> <read_seconds> <write_count> <write_seconds>
// Same lookup/ordering rules as handle_futex() above.
void handle_io_wait(double elapsed,unsigned int pid,char *rest){
  debug("handle_io_wait(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf %llu %lf",
	   &pentry->pinfo->io_read_wait_count,&pentry->pinfo->io_read_wait_seconds,
	   &pentry->pinfo->io_write_wait_count,&pentry->pinfo->io_write_wait_seconds);
  }
}

// format: elapsed pid io <rchar> <wchar> <syscr> <syscw> <read_bytes> <write_bytes> <cancelled_write_bytes>
// Same lookup/ordering rules as handle_futex() above.
void handle_io(double elapsed,unsigned int pid,char *rest){
  debug("handle_io(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %llu %llu %llu %llu %llu %llu",
	   &pentry->pinfo->io_rchar,&pentry->pinfo->io_wchar,
	   &pentry->pinfo->io_syscr,&pentry->pinfo->io_syscw,
	   &pentry->pinfo->io_read_bytes,&pentry->pinfo->io_write_bytes,
	   &pentry->pinfo->io_cancelled_write_bytes);
  }
}

// format: elapsed pid schedstat <cpu_seconds> <run_delay_seconds> <nr_timeslices>
// Same lookup/ordering rules as handle_futex() above.
void handle_schedstat(double elapsed,unsigned int pid,char *rest){
  debug("handle_schedstat(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%lf %lf %llu",
	   &pentry->pinfo->sched_cpu_seconds,&pentry->pinfo->sched_rundelay_seconds,
	   &pentry->pinfo->sched_nr_timeslices);
  }
}

// format: elapsed pid vmsize <vm_hwm_kb> <rss_anon_kb> <rss_file_kb> <rss_shmem_kb> <vm_swap_kb>
// Same lookup/ordering rules as handle_futex() above.
void handle_vmdetail(double elapsed,unsigned int pid,char *rest){
  debug("handle_vmdetail(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%lu %lu %lu %lu %lu",
	   &pentry->pinfo->vm_hwm_kb,&pentry->pinfo->rss_anon_kb,
	   &pentry->pinfo->rss_file_kb,&pentry->pinfo->rss_shmem_kb,
	   &pentry->pinfo->vm_swap_kb);
  }
}

// format: elapsed pid connect <count> <seconds>
// Same lookup/ordering rules as handle_futex() above.
void handle_connect(double elapsed,unsigned int pid,char *rest){
  debug("handle_connect(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf",&pentry->pinfo->connect_count,&pentry->pinfo->connect_seconds);
  }
}

// format: elapsed pid nanosleep <count> <seconds>
// Same lookup/ordering rules as handle_futex() above.
void handle_nanosleep(double elapsed,unsigned int pid,char *rest){
  debug("handle_nanosleep(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf",&pentry->pinfo->nanosleep_count,&pentry->pinfo->nanosleep_seconds);
  }
}

// format: elapsed pid wait <count> <seconds>
// Same lookup/ordering rules as handle_futex() above.
void handle_wait(double elapsed,unsigned int pid,char *rest){
  debug("handle_wait(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf",&pentry->pinfo->wait_count,&pentry->pinfo->wait_seconds);
  }
}

// format: elapsed pid poll <count> <seconds>
// Same lookup/ordering rules as handle_futex() above.
void handle_poll(double elapsed,unsigned int pid,char *rest){
  debug("handle_poll(%d,%s)\n",elapsed,pid,rest);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    sscanf(rest,"%llu %lf",&pentry->pinfo->poll_count,&pentry->pinfo->poll_seconds);
  }
}

// format: elapsed pid exit </proc/pid/stat>
void handle_exit(double elapsed,unsigned int pid,char *stat){
  debug("handle_exit(%d,%s)\n",elapsed,pid,stat);

  int status;
  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->finish = elapsed;
    parse_stat(stat,pentry->pinfo);
    status = remove_pid(pid);
    if (status) warning("unable to remove process %d?\n",pid);
  }
}

void handle_comm(double elapsed,unsigned int pid,char *comm){
  debug("handle_comm(%d,%s)\n",elapsed,pid,comm);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->comm = strdup(comm);
  }
}

void handle_cmdline(double elapsed,unsigned int pid,char *cmdline){
  debug("handle_cmdline(%d,%s)\n",elapsed,pid,cmdline);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  if (pentry){
    pentry->pinfo->cmdline = strdup(cmdline);
  }
}

// format: elapsed pid exec </proc/pid/exe target, or "?" if unresolved>
// Gives a still-running (never exited/signaled) process a real comm instead
// of "??" -- comm/cmdline are otherwise only populated at exit time (see
// handle_exit()/the WIFSIGNALED path in topdown.c), which a process that's
// still alive when the trace ends never reaches. A later "comm" line (from
// that same pid's eventual exit) overwrites this with the authoritative
// kernel-truncated name, so this is only ever a fallback.
void handle_exec(double elapsed,unsigned int pid,char *path){
  debug("handle_exec(%d,%s)\n",elapsed,pid,path);

  struct proc_table_entry *pentry = lookup_pid(pid,0);
  char *slash;
  if (pentry){
    free(pentry->pinfo->comm);
    slash = strrchr(path,'/');
    pentry->pinfo->comm = strdup(slash ? slash+1 : path);
  }
}

// add totals for processes with the same name
struct comm_info {
  char *comm;
  unsigned int count;
  unsigned int total_utime;
  unsigned int total_stime;
  unsigned long long total_futex_wait_count;
  double total_futex_wait_seconds;
  unsigned long long total_io_read_wait_count;
  double total_io_read_wait_seconds;
  unsigned long long total_io_write_wait_count;
  double total_io_write_wait_seconds;
  unsigned long long total_io_read_bytes,total_io_write_bytes;
  double total_sched_rundelay_seconds;
  unsigned long long total_sched_nr_timeslices;
  unsigned long total_vm_hwm_kb,total_rss_anon_kb,total_rss_file_kb,total_rss_shmem_kb,total_vm_swap_kb;
  unsigned long long total_connect_count;
  double total_connect_seconds;
  unsigned long long total_nanosleep_count;
  double total_nanosleep_seconds;
  unsigned long long total_wait_count;
  double total_wait_seconds;
  unsigned long long total_poll_count;
  double total_poll_seconds;
  struct comm_info *next;
};
struct comm_info *comm_totals = NULL;
int num_comm_info;

// walk the tree accumulating totals in comm_totals structure
static void collect_process_totals(struct process_info *pinfo){
  struct comm_info *ci;
  struct process_info *child;

  // look for this process
  int found = 0;
  for (ci = comm_totals;ci;ci=ci->next){
    if (pinfo->comm && !strcmp(pinfo->comm,ci->comm)){
      ci->total_utime += pinfo->utime;
      ci->total_stime += pinfo->stime;
      ci->total_futex_wait_count += pinfo->futex_wait_count;
      ci->total_futex_wait_seconds += pinfo->futex_wait_seconds;
      ci->total_io_read_wait_count += pinfo->io_read_wait_count;
      ci->total_io_read_wait_seconds += pinfo->io_read_wait_seconds;
      ci->total_io_write_wait_count += pinfo->io_write_wait_count;
      ci->total_io_write_wait_seconds += pinfo->io_write_wait_seconds;
      ci->total_io_read_bytes += pinfo->io_read_bytes;
      ci->total_io_write_bytes += pinfo->io_write_bytes;
      ci->total_sched_rundelay_seconds += pinfo->sched_rundelay_seconds;
      ci->total_sched_nr_timeslices += pinfo->sched_nr_timeslices;
      ci->total_vm_hwm_kb += pinfo->vm_hwm_kb;
      ci->total_rss_anon_kb += pinfo->rss_anon_kb;
      ci->total_rss_file_kb += pinfo->rss_file_kb;
      ci->total_rss_shmem_kb += pinfo->rss_shmem_kb;
      ci->total_vm_swap_kb += pinfo->vm_swap_kb;
      ci->total_connect_count += pinfo->connect_count;
      ci->total_connect_seconds += pinfo->connect_seconds;
      ci->total_nanosleep_count += pinfo->nanosleep_count;
      ci->total_nanosleep_seconds += pinfo->nanosleep_seconds;
      ci->total_wait_count += pinfo->wait_count;
      ci->total_wait_seconds += pinfo->wait_seconds;
      ci->total_poll_count += pinfo->poll_count;
      ci->total_poll_seconds += pinfo->poll_seconds;
      ci->count++;
      found = 1;
      break;
    }
  }
  // not found, so add an entry
  if (!found && pinfo->comm){
    ci = calloc(1,sizeof(struct comm_info));
    ci->comm = strdup(pinfo->comm);
    ci->next = comm_totals;
    ci->total_utime = pinfo->utime;
    ci->total_stime = pinfo->stime;
    ci->total_futex_wait_count = pinfo->futex_wait_count;
    ci->total_futex_wait_seconds = pinfo->futex_wait_seconds;
    ci->total_io_read_wait_count = pinfo->io_read_wait_count;
    ci->total_io_read_wait_seconds = pinfo->io_read_wait_seconds;
    ci->total_io_write_wait_count = pinfo->io_write_wait_count;
    ci->total_io_write_wait_seconds = pinfo->io_write_wait_seconds;
    ci->total_io_read_bytes = pinfo->io_read_bytes;
    ci->total_io_write_bytes = pinfo->io_write_bytes;
    ci->total_sched_rundelay_seconds = pinfo->sched_rundelay_seconds;
    ci->total_sched_nr_timeslices = pinfo->sched_nr_timeslices;
    ci->total_vm_hwm_kb = pinfo->vm_hwm_kb;
    ci->total_rss_anon_kb = pinfo->rss_anon_kb;
    ci->total_rss_file_kb = pinfo->rss_file_kb;
    ci->total_rss_shmem_kb = pinfo->rss_shmem_kb;
    ci->total_vm_swap_kb = pinfo->vm_swap_kb;
    ci->total_connect_count = pinfo->connect_count;
    ci->total_connect_seconds = pinfo->connect_seconds;
    ci->total_nanosleep_count = pinfo->nanosleep_count;
    ci->total_nanosleep_seconds = pinfo->nanosleep_seconds;
    ci->total_wait_count = pinfo->wait_count;
    ci->total_wait_seconds = pinfo->wait_seconds;
    ci->total_poll_count = pinfo->poll_count;
    ci->total_poll_seconds = pinfo->poll_seconds;
    ci->count = 1;
    comm_totals = ci;
    num_comm_info++;
  }
  
  // walk the tree of children (the particular order doesn't matter so go from youngest to oldest
  for (child=pinfo->children;child;child=child->older_sibling)
    collect_process_totals(child);
}

int comm_compare(const void *c1,const void *c2){
  const struct comm_info *comm1 = c1;
  const struct comm_info *comm2 = c2;
  if (comm1->total_utime > comm2->total_utime) return -1;
  else if (comm1->total_utime < comm2->total_utime) return 1;
  else if (comm1->total_stime > comm2->total_stime) return -1;
  else if (comm1->total_stime < comm2->total_stime) return 1;
  else if (comm1->count > comm2->count) return -1;
  else if (comm1->count < comm2->count) return 1;
  else return strcmp(comm1->comm,comm2->comm);
}

/* Runs collect_process_totals() over the whole tree and returns the
 * resulting per-comm rollup as a sorted (comm_compare()), malloc'd array of
 * *count_out entries -- shared by print_statistics()'s text summary and
 * print_summary_json()'s JSON summary array so the two can't drift apart
 * on what "totals per command name" means. Caller frees the returned array. */
static struct comm_info *build_comm_table(int *count_out){
  struct comm_info *ci;
  int count = 0;

  collect_process_totals(root_process);
  struct comm_info *comm_table = malloc(num_comm_info*sizeof(struct comm_info));
  for (ci=comm_totals;ci;ci=ci->next){
    comm_table[count] = *ci;
    count++;
  }
  qsort(comm_table,num_comm_info,sizeof(struct comm_info),comm_compare);
  *count_out = count;
  return comm_table;
}

static void print_statistics(){
  int count;
  int i;
  // run-wide totals, accumulated from the same per-comm sums printed below --
  // these overlap across processes running in parallel, so they're not a
  // critical-path duration, just a "how much of this happened in total" figure.
  unsigned long long total_futex_wait_count = 0;
  double total_futex_wait_seconds = 0;
  unsigned long long total_io_read_wait_count = 0,total_io_write_wait_count = 0;
  double total_io_read_wait_seconds = 0,total_io_write_wait_seconds = 0;
  unsigned long long total_io_read_bytes = 0,total_io_write_bytes = 0;
  double total_sched_rundelay_seconds = 0;
  unsigned long long total_sched_nr_timeslices = 0;
  // Unlike the durations above, these are point-in-time absolute values,
  // not accumulated time -- summing them across processes has no "may
  // overlap in parallel" caveat, same straight-sum framing as the io-byte
  // totals just below.
  unsigned long total_vm_hwm_kb = 0,total_rss_anon_kb = 0,total_rss_file_kb = 0;
  unsigned long total_rss_shmem_kb = 0,total_vm_swap_kb = 0;
  unsigned long long total_connect_count = 0,total_nanosleep_count = 0;
  unsigned long long total_wait_count = 0,total_poll_count = 0;
  double total_connect_seconds = 0,total_nanosleep_seconds = 0;
  double total_wait_seconds = 0,total_poll_seconds = 0;

  printf("%d processes\n",total_processes);

  struct comm_info *comm_table = build_comm_table(&count);
  for (i=0;i<count;i++){
    printf("\t%3d %-20s %8.2f %8.2f",
	   comm_table[i].count,
	   comm_table[i].comm,
	   (double) comm_table[i].total_utime / clocks_per_second,
	   (double) comm_table[i].total_stime / clocks_per_second);
    if (futexflag){
      printf(" futex_wait=%8.3f (%llu waits)",
	     comm_table[i].total_futex_wait_seconds,
	     comm_table[i].total_futex_wait_count);
      total_futex_wait_seconds += comm_table[i].total_futex_wait_seconds;
      total_futex_wait_count += comm_table[i].total_futex_wait_count;
    }
    if (bflag){
      printf(" io_wait=%8.3fr/%8.3fw (%llu reads, %llu writes)",
	     comm_table[i].total_io_read_wait_seconds,
	     comm_table[i].total_io_write_wait_seconds,
	     comm_table[i].total_io_read_wait_count,
	     comm_table[i].total_io_write_wait_count);
      total_io_read_wait_seconds += comm_table[i].total_io_read_wait_seconds;
      total_io_write_wait_seconds += comm_table[i].total_io_write_wait_seconds;
      total_io_read_wait_count += comm_table[i].total_io_read_wait_count;
      total_io_write_wait_count += comm_table[i].total_io_write_wait_count;
    }
    if (iflag){
      printf(" io_bytes=%llur/%lluw",
	     comm_table[i].total_io_read_bytes,
	     comm_table[i].total_io_write_bytes);
      total_io_read_bytes += comm_table[i].total_io_read_bytes;
      total_io_write_bytes += comm_table[i].total_io_write_bytes;
    }
    if (dflag){
      printf(" run_delay=%8.3f (%llu timeslices)",
	     comm_table[i].total_sched_rundelay_seconds,
	     comm_table[i].total_sched_nr_timeslices);
      total_sched_rundelay_seconds += comm_table[i].total_sched_rundelay_seconds;
      total_sched_nr_timeslices += comm_table[i].total_sched_nr_timeslices;
    }
    if (rflag){
      printf(" vm_hwm=%luk rss_anon=%luk rss_file=%luk rss_shmem=%luk vm_swap=%luk",
	     comm_table[i].total_vm_hwm_kb,comm_table[i].total_rss_anon_kb,
	     comm_table[i].total_rss_file_kb,comm_table[i].total_rss_shmem_kb,
	     comm_table[i].total_vm_swap_kb);
      total_vm_hwm_kb += comm_table[i].total_vm_hwm_kb;
      total_rss_anon_kb += comm_table[i].total_rss_anon_kb;
      total_rss_file_kb += comm_table[i].total_rss_file_kb;
      total_rss_shmem_kb += comm_table[i].total_rss_shmem_kb;
      total_vm_swap_kb += comm_table[i].total_vm_swap_kb;
    }
    if (kflag){
      printf(" connect=%8.3f (%llu calls)",
	     comm_table[i].total_connect_seconds,comm_table[i].total_connect_count);
      total_connect_seconds += comm_table[i].total_connect_seconds;
      total_connect_count += comm_table[i].total_connect_count;
    }
    if (zflag){
      printf(" nanosleep=%8.3f (%llu calls)",
	     comm_table[i].total_nanosleep_seconds,comm_table[i].total_nanosleep_count);
      total_nanosleep_seconds += comm_table[i].total_nanosleep_seconds;
      total_nanosleep_count += comm_table[i].total_nanosleep_count;
    }
    if (jflag){
      printf(" wait=%8.3f (%llu calls)",
	     comm_table[i].total_wait_seconds,comm_table[i].total_wait_count);
      total_wait_seconds += comm_table[i].total_wait_seconds;
      total_wait_count += comm_table[i].total_wait_count;
    }
    if (lflag){
      printf(" poll=%8.3f (%llu calls)",
	     comm_table[i].total_poll_seconds,comm_table[i].total_poll_count);
      total_poll_seconds += comm_table[i].total_poll_seconds;
      total_poll_count += comm_table[i].total_poll_count;
    }
    printf("\n");
  }

  printf("%d processes running\n",num_processes);
  printf("%d maximum processes\n",max_num_processes);
  if (futexflag){
    printf("total futex_wait=%.3f (%llu waits) -- summed across processes, may overlap in parallel\n",
	   total_futex_wait_seconds,total_futex_wait_count);
  }
  if (bflag){
    printf("total io_wait=%.3fr/%.3fw (%llu reads, %llu writes) -- summed across processes, may overlap in parallel\n",
	   total_io_read_wait_seconds,total_io_write_wait_seconds,
	   total_io_read_wait_count,total_io_write_wait_count);
  }
  if (iflag){
    printf("total io_bytes=%llur/%lluw\n",
	   total_io_read_bytes,total_io_write_bytes);
  }
  if (dflag){
    printf("total run_delay=%.3f (%llu timeslices) -- summed across processes, may overlap in parallel\n",
	   total_sched_rundelay_seconds,total_sched_nr_timeslices);
  }
  if (rflag){
    printf("total vm_hwm=%luk rss_anon=%luk rss_file=%luk rss_shmem=%luk vm_swap=%luk\n",
	   total_vm_hwm_kb,total_rss_anon_kb,total_rss_file_kb,total_rss_shmem_kb,total_vm_swap_kb);
  }
  if (kflag){
    printf("total connect=%.3f (%llu calls) -- summed across processes, may overlap in parallel\n",
	   total_connect_seconds,total_connect_count);
  }
  if (zflag){
    printf("total nanosleep=%.3f (%llu calls) -- summed across processes, may overlap in parallel\n",
	   total_nanosleep_seconds,total_nanosleep_count);
  }
  if (jflag){
    printf("total wait=%.3f (%llu calls) -- summed across processes, may overlap in parallel\n",
	   total_wait_seconds,total_wait_count);
  }
  if (lflag){
    printf("total poll=%.3f (%llu calls) -- summed across processes, may overlap in parallel\n",
	   total_poll_seconds,total_poll_count);
  }
  printf("\n");
  free(comm_table);
}

static void print_tree(struct process_info *pinfo,int level){
  int i;
  int comm_width;
  int print_width;
  struct process_info *eldest;
  // print the node
  for (i=0;i<level;i++) printf("  ");
  printf("%d)",pinfo->pid);

  if (print_cmdline && pinfo->cmdline){
    print_width = output_width - 8 - 2*level;
    printf(" %.*s",print_width,pinfo->cmdline);
  } else if (pinfo->comm){
    printf(" %-16s",pinfo->comm);
  } else {
    printf(" ??");
  }
  printf(" cpu=%d",pinfo->processor);
  if (fflag){
    printf(" start=%-5.2f finish=%-5.2f",pinfo->start,pinfo->finish);
  }
  if (mflag){
    printf(" vmsize=%uk rss=%luk",(pinfo->vsize+512)/1024,
	   (pinfo->rss*page_size+512)/1024);
  }
  if (uflag){
    printf(" utime=%-4.2f stime=%-4.2f",
	   ((double) pinfo->utime/clocks_per_second),
	   ((double) pinfo->stime/clocks_per_second));
  }
  if (pflag){
    printf(" ppid=%u",pinfo->ppid);
  }
  if (nflag){
    printf(" threads=%u",pinfo->num_threads);
  }
  if (futexflag){
    printf(" futex_waits=%llu futex_wait_time=%.3f",
	   pinfo->futex_wait_count,pinfo->futex_wait_seconds);
  }
  if (bflag){
    printf(" io_read_waits=%llu io_read_wait_time=%.3f io_write_waits=%llu io_write_wait_time=%.3f",
	   pinfo->io_read_wait_count,pinfo->io_read_wait_seconds,
	   pinfo->io_write_wait_count,pinfo->io_write_wait_seconds);
  }
  if (iflag){
    printf(" io_rchar=%llu io_wchar=%llu io_syscr=%llu io_syscw=%llu"
	   " io_read_bytes=%llu io_write_bytes=%llu io_cancelled_write_bytes=%llu",
	   pinfo->io_rchar,pinfo->io_wchar,pinfo->io_syscr,pinfo->io_syscw,
	   pinfo->io_read_bytes,pinfo->io_write_bytes,pinfo->io_cancelled_write_bytes);
  }
  if (dflag){
    // sched_cpu_seconds is deliberately not printed here -- see struct
    // process_info's comment on why it'd just be a second, confusing
    // measurement of what -U's utime/stime already show.
    printf(" run_delay=%.3f timeslices=%llu",
	   pinfo->sched_rundelay_seconds,pinfo->sched_nr_timeslices);
  }
  if (rflag){
    printf(" vm_hwm=%luk rss_anon=%luk rss_file=%luk rss_shmem=%luk vm_swap=%luk",
	   pinfo->vm_hwm_kb,pinfo->rss_anon_kb,pinfo->rss_file_kb,
	   pinfo->rss_shmem_kb,pinfo->vm_swap_kb);
  }
  if (kflag){
    printf(" connect_calls=%llu connect_time=%.3f",pinfo->connect_count,pinfo->connect_seconds);
  }
  if (zflag){
    printf(" nanosleep_calls=%llu nanosleep_time=%.3f",pinfo->nanosleep_count,pinfo->nanosleep_seconds);
  }
  if (jflag){
    printf(" wait_calls=%llu wait_time=%.3f",pinfo->wait_count,pinfo->wait_seconds);
  }
  if (lflag){
    printf(" poll_calls=%llu poll_time=%.3f",pinfo->poll_count,pinfo->poll_seconds);
  }

  printf("\n");

  if (!pinfo->children) return;
  
  // find the eldest child and print the children
  for (eldest=pinfo->children;eldest->older_sibling != NULL;eldest=eldest->older_sibling);

  while (eldest){
    print_tree(eldest,level+1);
    eldest = eldest->younger_sibling;
  }
}

/* ---- --json export (INVESTIGATION.md 4.2 Tier 1, "proctree JSON export + interactive viewer + run-to-run diff") ----
 *
 * Every field on struct process_info is emitted unconditionally, unlike
 * text mode's -C/-M/-N/-P/-U/-X/-B/-I/-D/-R/-K/-J/-L/-Z toggles (which only
 * gate what gets *printed*, never what's collected) -- the whole point of
 * JSON is letting a viewer toggle columns client-side rather than baking a
 * fixed set of annotations in at generation time, so nothing is
 * pre-filtered here. A field that was never collected this run (e.g.
 * futex_wait_seconds without --tree-futex) is simply 0/null, the same
 * "indistinguishable from a genuine zero" convention documented on struct
 * process_info's own fields.
 */

static void json_write_string_or_null(FILE *out,const char *s){
  if (s) json_write_string(out,s);
  else fprintf(out,"null");
}

static void print_process_json(FILE *out,struct process_info *pinfo){
  struct process_info *eldest;
  int first;

  fprintf(out,"{");
  fprintf(out,"\"pid\":%u,",pinfo->pid);
  fprintf(out,"\"comm\":"); json_write_string_or_null(out,pinfo->comm); fprintf(out,",");
  fprintf(out,"\"cmdline\":"); json_write_string_or_null(out,pinfo->cmdline); fprintf(out,",");
  fprintf(out,"\"ppid\":%u,",pinfo->ppid);
  fprintf(out,"\"cpu\":%u,",pinfo->processor);
  fprintf(out,"\"start\":%.6f,",pinfo->start);
  fprintf(out,"\"finish\":%.6f,",pinfo->finish);
  fprintf(out,"\"utime_seconds\":%.6f,",(double) pinfo->utime/clocks_per_second);
  fprintf(out,"\"stime_seconds\":%.6f,",(double) pinfo->stime/clocks_per_second);
  fprintf(out,"\"vsize_kb\":%lu,",(unsigned long)((pinfo->vsize+512)/1024));
  fprintf(out,"\"rss_kb\":%lu,",(unsigned long)((pinfo->rss*page_size+512)/1024));
  fprintf(out,"\"num_threads\":%u,",pinfo->num_threads);
  fprintf(out,"\"exit_code\":%u,",pinfo->exit_code);
  fprintf(out,"\"futex_wait_count\":%llu,",pinfo->futex_wait_count);
  fprintf(out,"\"futex_wait_seconds\":%.6f,",pinfo->futex_wait_seconds);
  fprintf(out,"\"io_read_wait_count\":%llu,",pinfo->io_read_wait_count);
  fprintf(out,"\"io_read_wait_seconds\":%.6f,",pinfo->io_read_wait_seconds);
  fprintf(out,"\"io_write_wait_count\":%llu,",pinfo->io_write_wait_count);
  fprintf(out,"\"io_write_wait_seconds\":%.6f,",pinfo->io_write_wait_seconds);
  fprintf(out,"\"io_rchar\":%llu,",pinfo->io_rchar);
  fprintf(out,"\"io_wchar\":%llu,",pinfo->io_wchar);
  fprintf(out,"\"io_syscr\":%llu,",pinfo->io_syscr);
  fprintf(out,"\"io_syscw\":%llu,",pinfo->io_syscw);
  fprintf(out,"\"io_read_bytes\":%llu,",pinfo->io_read_bytes);
  fprintf(out,"\"io_write_bytes\":%llu,",pinfo->io_write_bytes);
  fprintf(out,"\"io_cancelled_write_bytes\":%llu,",pinfo->io_cancelled_write_bytes);
  fprintf(out,"\"sched_rundelay_seconds\":%.6f,",pinfo->sched_rundelay_seconds);
  fprintf(out,"\"sched_nr_timeslices\":%llu,",pinfo->sched_nr_timeslices);
  fprintf(out,"\"vm_hwm_kb\":%lu,",pinfo->vm_hwm_kb);
  fprintf(out,"\"rss_anon_kb\":%lu,",pinfo->rss_anon_kb);
  fprintf(out,"\"rss_file_kb\":%lu,",pinfo->rss_file_kb);
  fprintf(out,"\"rss_shmem_kb\":%lu,",pinfo->rss_shmem_kb);
  fprintf(out,"\"vm_swap_kb\":%lu,",pinfo->vm_swap_kb);
  fprintf(out,"\"connect_count\":%llu,",pinfo->connect_count);
  fprintf(out,"\"connect_seconds\":%.6f,",pinfo->connect_seconds);
  fprintf(out,"\"nanosleep_count\":%llu,",pinfo->nanosleep_count);
  fprintf(out,"\"nanosleep_seconds\":%.6f,",pinfo->nanosleep_seconds);
  fprintf(out,"\"wait_count\":%llu,",pinfo->wait_count);
  fprintf(out,"\"wait_seconds\":%.6f,",pinfo->wait_seconds);
  fprintf(out,"\"poll_count\":%llu,",pinfo->poll_count);
  fprintf(out,"\"poll_seconds\":%.6f,",pinfo->poll_seconds);

  fprintf(out,"\"children\":[");
  first = 1;
  if (pinfo->children){
    for (eldest=pinfo->children;eldest->older_sibling != NULL;eldest=eldest->older_sibling);
    while (eldest){
      if (!first) fprintf(out,",");
      print_process_json(out,eldest);
      first = 0;
      eldest = eldest->younger_sibling;
    }
  }
  fprintf(out,"]");
  fprintf(out,"}");
}

static void print_comm_info_json(FILE *out,struct comm_info *ci){
  fprintf(out,"{");
  fprintf(out,"\"comm\":"); json_write_string(out,ci->comm); fprintf(out,",");
  fprintf(out,"\"count\":%u,",ci->count);
  fprintf(out,"\"total_utime_seconds\":%.6f,",(double) ci->total_utime/clocks_per_second);
  fprintf(out,"\"total_stime_seconds\":%.6f,",(double) ci->total_stime/clocks_per_second);
  fprintf(out,"\"total_futex_wait_count\":%llu,",ci->total_futex_wait_count);
  fprintf(out,"\"total_futex_wait_seconds\":%.6f,",ci->total_futex_wait_seconds);
  fprintf(out,"\"total_io_read_wait_count\":%llu,",ci->total_io_read_wait_count);
  fprintf(out,"\"total_io_read_wait_seconds\":%.6f,",ci->total_io_read_wait_seconds);
  fprintf(out,"\"total_io_write_wait_count\":%llu,",ci->total_io_write_wait_count);
  fprintf(out,"\"total_io_write_wait_seconds\":%.6f,",ci->total_io_write_wait_seconds);
  fprintf(out,"\"total_io_read_bytes\":%llu,",ci->total_io_read_bytes);
  fprintf(out,"\"total_io_write_bytes\":%llu,",ci->total_io_write_bytes);
  fprintf(out,"\"total_sched_rundelay_seconds\":%.6f,",ci->total_sched_rundelay_seconds);
  fprintf(out,"\"total_sched_nr_timeslices\":%llu,",ci->total_sched_nr_timeslices);
  fprintf(out,"\"total_vm_hwm_kb\":%lu,",ci->total_vm_hwm_kb);
  fprintf(out,"\"total_rss_anon_kb\":%lu,",ci->total_rss_anon_kb);
  fprintf(out,"\"total_rss_file_kb\":%lu,",ci->total_rss_file_kb);
  fprintf(out,"\"total_rss_shmem_kb\":%lu,",ci->total_rss_shmem_kb);
  fprintf(out,"\"total_vm_swap_kb\":%lu,",ci->total_vm_swap_kb);
  fprintf(out,"\"total_connect_count\":%llu,",ci->total_connect_count);
  fprintf(out,"\"total_connect_seconds\":%.6f,",ci->total_connect_seconds);
  fprintf(out,"\"total_nanosleep_count\":%llu,",ci->total_nanosleep_count);
  fprintf(out,"\"total_nanosleep_seconds\":%.6f,",ci->total_nanosleep_seconds);
  fprintf(out,"\"total_wait_count\":%llu,",ci->total_wait_count);
  fprintf(out,"\"total_wait_seconds\":%.6f,",ci->total_wait_seconds);
  fprintf(out,"\"total_poll_count\":%llu,",ci->total_poll_count);
  fprintf(out,"\"total_poll_seconds\":%.6f",ci->total_poll_seconds);
  fprintf(out,"}");
}

static void print_summary_json(FILE *out){
  int count,i;
  struct comm_info *comm_table = build_comm_table(&count);

  fprintf(out,"[");
  for (i=0;i<count;i++){
    if (i) fprintf(out,",");
    print_comm_info_json(out,&comm_table[i]);
  }
  fprintf(out,"]");
  free(comm_table);
}

static void print_json(FILE *out,const char *source_file){
  fprintf(out,"{");
  fprintf(out,"\"schema_version\":"); json_write_string(out,PROCTREE_JSON_SCHEMA_VERSION); fprintf(out,",");
  fprintf(out,"\"source_file\":"); json_write_string(out,source_file); fprintf(out,",");
  fprintf(out,"\"process_count\":%d,",total_processes);
  fprintf(out,"\"max_concurrent_processes\":%d,",max_num_processes);
  fprintf(out,"\"summary\":");
  print_summary_json(out);
  fprintf(out,",\"tree\":");
  if (root_process) print_process_json(out,root_process);
  else fprintf(out,"null");
  fprintf(out,"}\n");
}

/* ---- --diff: run-to-run tree diff (INVESTIGATION.md 4.2 Tier 1, same item) ----
 *
 * Takes two files already produced by --json (not raw wspy --tree output --
 * see run_diff()) and matches subtrees structurally: pids never correspond
 * across two separate runs, so matching is instead by ancestor-comm-path,
 * disambiguated by same-comm sibling occurrence order (implemented as a
 * direct recursive merge below, not a flattened global signature map).
 *
 * Add an entry here to diff another per-process metric -- mirrors this
 * codebase's sanity_bounds[]/multipass_group_names[] table-driven idiom.
 */
struct diff_metric { const char *json_key; const char *label; };
static const struct diff_metric diff_metrics[] = {
  {"utime_seconds","utime"},
  {"stime_seconds","stime"},
  {"futex_wait_seconds","futex_wait"},
  {"io_read_wait_seconds","io_read_wait"},
  {"io_write_wait_seconds","io_write_wait"},
  {"io_read_bytes","io_read_bytes"},
  {"io_write_bytes","io_write_bytes"},
  {"sched_rundelay_seconds","run_delay"},
  {"connect_seconds","connect"},
  {"nanosleep_seconds","nanosleep"},
  {"wait_seconds","wait"},
  {"poll_seconds","poll"},
};
#define NUM_DIFF_METRICS (sizeof(diff_metrics)/sizeof(diff_metrics[0]))
#define DIFF_METRIC_UTIME 0
#define DIFF_METRIC_STIME 1

struct diff_node {
  char *path;
  char *comm;
  const struct json_value *a; // node object from run A, or NULL if added
  const struct json_value *b; // node object from run B, or NULL if removed
  const char *status; // "matched", "added", or "removed"
  int changed; // matched only: 1 if |delta utime+stime| exceeds the threshold
  double delta[NUM_DIFF_METRICS]; // matched only: b-a per diff_metrics[] entry
  struct diff_node **children;
  size_t num_children;
};

// dynamically growable list of matched diff_node*, used to sort/cap the
// human report's "changed" section without needing a second tree walk
struct diff_node_list { struct diff_node **items; size_t count,capacity; };

static void diff_node_list_add(struct diff_node_list *list,struct diff_node *node){
  if (list->count == list->capacity){
    list->capacity = list->capacity ? list->capacity*2 : 16;
    list->items = realloc(list->items,list->capacity*sizeof(*list->items));
  }
  list->items[list->count++] = node;
}

static const char *diff_node_display_status(struct diff_node *dn){
  if (strcmp(dn->status,"matched")) return dn->status;
  return dn->changed ? "changed" : "same";
}

// one comm value's children on one side, in original (eldest-to-youngest) order
struct comm_group { char *comm; const struct json_value **items; size_t count,capacity; };
struct comm_group_table { struct comm_group *groups; size_t count,capacity; };

static struct comm_group *comm_group_table_find_or_add(struct comm_group_table *t,const char *comm){
  size_t i;
  for (i=0;i<t->count;i++) if (!strcmp(t->groups[i].comm,comm)) return &t->groups[i];
  if (t->count == t->capacity){
    t->capacity = t->capacity ? t->capacity*2 : 8;
    t->groups = realloc(t->groups,t->capacity*sizeof(*t->groups));
  }
  struct comm_group *g = &t->groups[t->count++];
  g->comm = strdup(comm);
  g->items = NULL;
  g->count = 0;
  g->capacity = 0;
  return g;
}

static void comm_group_add_item(struct comm_group *g,const struct json_value *item){
  if (g->count == g->capacity){
    g->capacity = g->capacity ? g->capacity*2 : 8;
    g->items = realloc(g->items,g->capacity*sizeof(*g->items));
  }
  g->items[g->count++] = item;
}

static void build_comm_group_table(const struct json_value *children_array,struct comm_group_table *t){
  size_t n = json_array_len(children_array);
  size_t i;
  for (i=0;i<n;i++){
    const struct json_value *child = json_array_get(children_array,i);
    const char *comm = json_get_string(child,"comm","?");
    comm_group_add_item(comm_group_table_find_or_add(t,comm),child);
  }
}

static void free_comm_group_table(struct comm_group_table *t){
  size_t i;
  for (i=0;i<t->count;i++){
    free(t->groups[i].comm);
    free(t->groups[i].items);
  }
  free(t->groups);
}

static struct diff_node *merge_nodes(const struct json_value *a,const struct json_value *b,
				     const char *parent_path,int occurrence,double threshold,
				     struct diff_node_list *flat){
  struct diff_node *node = calloc(1,sizeof(*node));
  const char *comm = json_get_string(a ? a : b,"comm","?");
  char path[1024];
  size_t i;

  if (occurrence > 0) snprintf(path,sizeof(path),"%s/%s#%d",parent_path,comm,occurrence);
  else snprintf(path,sizeof(path),"%s/%s",parent_path,comm);
  node->path = strdup(path);
  node->comm = strdup(comm);
  node->a = a;
  node->b = b;

  if (a && b){
    node->status = "matched";
    for (i=0;i<NUM_DIFF_METRICS;i++){
      node->delta[i] = json_get_number(b,diff_metrics[i].json_key,0) -
			json_get_number(a,diff_metrics[i].json_key,0);
    }
    node->changed = fabs(node->delta[DIFF_METRIC_UTIME]) +
		     fabs(node->delta[DIFF_METRIC_STIME]) > threshold;
    diff_node_list_add(flat,node);
  } else if (a){
    node->status = "removed";
  } else {
    node->status = "added";
  }

  struct comm_group_table ta = {0},tb = {0};
  if (a) build_comm_group_table(json_object_get(a,"children"),&ta);
  if (b) build_comm_group_table(json_object_get(b,"children"),&tb);

  size_t cap = 8,n = 0;
  struct diff_node **children = malloc(cap*sizeof(*children));
  size_t gi,idx;

  for (gi=0;gi<ta.count;gi++){
    struct comm_group *ga = &ta.groups[gi];
    struct comm_group *gb = NULL;
    size_t bi;
    for (bi=0;bi<tb.count;bi++){
      if (!strcmp(tb.groups[bi].comm,ga->comm)){ gb = &tb.groups[bi]; break; }
    }
    size_t count_a = ga->count,count_b = gb ? gb->count : 0;
    size_t maxc = count_a > count_b ? count_a : count_b;
    for (idx=0;idx<maxc;idx++){
      const struct json_value *ca = idx < count_a ? ga->items[idx] : NULL;
      const struct json_value *cb = (gb && idx < count_b) ? gb->items[idx] : NULL;
      if (n == cap){ cap *= 2; children = realloc(children,cap*sizeof(*children)); }
      children[n++] = merge_nodes(ca,cb,node->path,(int)idx,threshold,flat);
    }
  }
  for (gi=0;gi<tb.count;gi++){
    struct comm_group *gb = &tb.groups[gi];
    int found_in_a = 0;
    size_t ai;
    for (ai=0;ai<ta.count;ai++) if (!strcmp(ta.groups[ai].comm,gb->comm)){ found_in_a = 1; break; }
    if (found_in_a) continue;
    for (idx=0;idx<gb->count;idx++){
      if (n == cap){ cap *= 2; children = realloc(children,cap*sizeof(*children)); }
      children[n++] = merge_nodes(NULL,gb->items[idx],node->path,(int)idx,threshold,flat);
    }
  }
  free_comm_group_table(&ta);
  free_comm_group_table(&tb);

  node->children = children;
  node->num_children = n;
  return node;
}

static void collect_topmost_changes(struct diff_node *dn,struct diff_node_list *added,struct diff_node_list *removed){
  size_t i;
  if (!strcmp(dn->status,"added")){ diff_node_list_add(added,dn); return; }
  if (!strcmp(dn->status,"removed")){ diff_node_list_add(removed,dn); return; }
  for (i=0;i<dn->num_children;i++) collect_topmost_changes(dn->children[i],added,removed);
}

static void diff_node_subtree_totals(struct diff_node *dn,int *count,double *cpu_seconds){
  const struct json_value *side = dn->a ? dn->a : dn->b;
  size_t i;
  (*count)++;
  if (side){
    *cpu_seconds += json_get_number(side,"utime_seconds",0) + json_get_number(side,"stime_seconds",0);
  }
  for (i=0;i<dn->num_children;i++) diff_node_subtree_totals(dn->children[i],count,cpu_seconds);
}

static int changed_node_compare(const void *v1,const void *v2){
  struct diff_node *const *n1 = v1;
  struct diff_node *const *n2 = v2;
  double c1 = fabs((*n1)->delta[DIFF_METRIC_UTIME]) + fabs((*n1)->delta[DIFF_METRIC_STIME]);
  double c2 = fabs((*n2)->delta[DIFF_METRIC_UTIME]) + fabs((*n2)->delta[DIFF_METRIC_STIME]);
  if (c1 > c2) return -1;
  if (c1 < c2) return 1;
  return 0;
}

#define MAX_DIFF_REPORT_ROWS 50

static void print_diff_side_json(FILE *out,const struct json_value *node){
  size_t i;
  if (!node){ fprintf(out,"null"); return; }
  fprintf(out,"{");
  fprintf(out,"\"pid\":%.0f,",json_get_number(node,"pid",0));
  fprintf(out,"\"comm\":"); json_write_string_or_null(out,json_get_string(node,"comm",NULL));
  fprintf(out,",\"cmdline\":"); json_write_string_or_null(out,json_get_string(node,"cmdline",NULL));
  for (i=0;i<NUM_DIFF_METRICS;i++){
    fprintf(out,",\"%s\":%.6f",diff_metrics[i].json_key,json_get_number(node,diff_metrics[i].json_key,0));
  }
  fprintf(out,"}");
}

static void print_diff_node_json(FILE *out,struct diff_node *dn){
  size_t i;
  fprintf(out,"{");
  fprintf(out,"\"path\":"); json_write_string(out,dn->path); fprintf(out,",");
  fprintf(out,"\"comm\":"); json_write_string(out,dn->comm); fprintf(out,",");
  fprintf(out,"\"status\":\"%s\",",diff_node_display_status(dn));
  fprintf(out,"\"a\":"); print_diff_side_json(out,dn->a); fprintf(out,",");
  fprintf(out,"\"b\":"); print_diff_side_json(out,dn->b);
  if (!strcmp(dn->status,"matched")){
    fprintf(out,",\"delta\":{");
    for (i=0;i<NUM_DIFF_METRICS;i++){
      if (i) fprintf(out,",");
      fprintf(out,"\"%s\":%.6f",diff_metrics[i].json_key,dn->delta[i]);
    }
    fprintf(out,"}");
  }
  fprintf(out,",\"children\":[");
  for (i=0;i<dn->num_children;i++){
    if (i) fprintf(out,",");
    print_diff_node_json(out,dn->children[i]);
  }
  fprintf(out,"]}");
}

// joins the two files' top-level "summary" arrays by comm name (not tree
// position) -- a quick "did this command's aggregate behavior change at
// all" overview, complementary to (not a replacement for) the structural
// diff_tree above.
static void print_summary_diff_json(FILE *out,const struct json_value *summary_a,const struct json_value *summary_b){
  size_t na = json_array_len(summary_a),nb = json_array_len(summary_b);
  int *b_matched = calloc(nb ? nb : 1,sizeof(int));
  int first = 1;
  size_t i,j;

  fprintf(out,"[");
  for (i=0;i<na;i++){
    const struct json_value *ea = json_array_get(summary_a,i);
    const char *comm = json_get_string(ea,"comm","?");
    const struct json_value *eb = NULL;
    for (j=0;j<nb;j++){
      const struct json_value *cand = json_array_get(summary_b,j);
      if (!strcmp(json_get_string(cand,"comm","?"),comm)){ eb = cand; b_matched[j] = 1; break; }
    }
    if (!first) fprintf(out,",");
    first = 0;
    fprintf(out,"{\"comm\":"); json_write_string(out,comm); fprintf(out,",");
    fprintf(out,"\"status\":\"%s\",",eb ? "matched" : "removed");
    fprintf(out,"\"count_a\":%.0f,",json_get_number(ea,"count",0));
    fprintf(out,"\"count_b\":%.0f,",eb ? json_get_number(eb,"count",0) : 0.0);
    fprintf(out,"\"total_utime_seconds_a\":%.6f,",json_get_number(ea,"total_utime_seconds",0));
    fprintf(out,"\"total_utime_seconds_b\":%.6f,",eb ? json_get_number(eb,"total_utime_seconds",0) : 0.0);
    fprintf(out,"\"total_stime_seconds_a\":%.6f,",json_get_number(ea,"total_stime_seconds",0));
    fprintf(out,"\"total_stime_seconds_b\":%.6f",eb ? json_get_number(eb,"total_stime_seconds",0) : 0.0);
    fprintf(out,"}");
  }
  for (j=0;j<nb;j++){
    if (b_matched[j]) continue;
    const struct json_value *eb = json_array_get(summary_b,j);
    if (!first) fprintf(out,",");
    first = 0;
    fprintf(out,"{\"comm\":"); json_write_string(out,json_get_string(eb,"comm","?")); fprintf(out,",");
    fprintf(out,"\"status\":\"added\",\"count_a\":0,");
    fprintf(out,"\"count_b\":%.0f,",json_get_number(eb,"count",0));
    fprintf(out,"\"total_utime_seconds_a\":0,\"total_utime_seconds_b\":%.6f,",json_get_number(eb,"total_utime_seconds",0));
    fprintf(out,"\"total_stime_seconds_a\":0,\"total_stime_seconds_b\":%.6f",json_get_number(eb,"total_stime_seconds",0));
    fprintf(out,"}");
  }
  fprintf(out,"]");
  free(b_matched);
}

static int print_diff_text_summary(FILE *out,const struct json_value *summary_a,const struct json_value *summary_b){
  size_t na = json_array_len(summary_a),nb = json_array_len(summary_b);
  int *b_matched = calloc(nb ? nb : 1,sizeof(int));
  int any_diff = 0;
  size_t i,j;

  fprintf(out,"Summary diff (by command name, not tree position):\n");
  for (i=0;i<na;i++){
    const struct json_value *ea = json_array_get(summary_a,i);
    const char *comm = json_get_string(ea,"comm","?");
    const struct json_value *eb = NULL;
    for (j=0;j<nb;j++){
      const struct json_value *cand = json_array_get(summary_b,j);
      if (!strcmp(json_get_string(cand,"comm","?"),comm)){ eb = cand; b_matched[j] = 1; break; }
    }
    if (!eb){
      fprintf(out,"  - %-20s count=%.0f (removed)\n",comm,json_get_number(ea,"count",0));
      any_diff = 1;
      continue;
    }
    double ua = json_get_number(ea,"total_utime_seconds",0),ub = json_get_number(eb,"total_utime_seconds",0);
    double sa = json_get_number(ea,"total_stime_seconds",0),sb = json_get_number(eb,"total_stime_seconds",0);
    double ca = json_get_number(ea,"count",0),cb = json_get_number(eb,"count",0);
    if (ca != cb || fabs(ub-ua) > 1e-9 || fabs(sb-sa) > 1e-9){
      fprintf(out,"  ~ %-20s count=%.0f->%.0f utime=%.3f->%.3f stime=%.3f->%.3f\n",
	      comm,ca,cb,ua,ub,sa,sb);
      any_diff = 1;
    }
  }
  for (j=0;j<nb;j++){
    if (b_matched[j]) continue;
    const struct json_value *eb = json_array_get(summary_b,j);
    fprintf(out,"  + %-20s count=%.0f (added)\n",json_get_string(eb,"comm","?"),json_get_number(eb,"count",0));
    any_diff = 1;
  }
  free(b_matched);
  fprintf(out,"\n");
  return any_diff;
}

static void print_added_removed_subtree(FILE *out,struct diff_node *dn,const char *label){
  int count = 0;
  double cpu_seconds = 0;
  diff_node_subtree_totals(dn,&count,&cpu_seconds);
  fprintf(out,"  %s %s -- %d process%s, cpu_time=%.3fs\n",
	  label,dn->path,count,count == 1 ? "" : "es",cpu_seconds);
}

// runs the full diff between two --json-exported files and writes either the
// default human-readable report or, with json_flag, the merged diff tree as
// JSON. Returns 0 if the two trees matched exactly (no added/removed/changed
// nodes), 1 if any difference was found -- diff(1)'s own exit-code
// convention, since a run-to-run diff with no differences is itself a
// meaningful (and testable) answer.
static int run_diff(const char *file_a,const char *file_b,int json_out,double threshold){
  char errbuf[256];
  struct json_value *root_a = json_parse_file(file_a,errbuf,sizeof(errbuf));
  if (!root_a){
    fatal("unable to parse %s as JSON (%s) -- did you forget to run '%s --json' first?\n",
	  file_a,errbuf,"proctree");
  }
  struct json_value *root_b = json_parse_file(file_b,errbuf,sizeof(errbuf));
  if (!root_b){
    fatal("unable to parse %s as JSON (%s) -- did you forget to run '%s --json' first?\n",
	  file_b,errbuf,"proctree");
  }

  const struct json_value *tree_a = json_object_get(root_a,"tree");
  const struct json_value *tree_b = json_object_get(root_b,"tree");
  const struct json_value *summary_a = json_object_get(root_a,"summary");
  const struct json_value *summary_b = json_object_get(root_b,"summary");

  struct diff_node_list flat = {0};
  struct diff_node *root_diff = merge_nodes(tree_a,tree_b,"",0,threshold,&flat);

  int any_changed = 0;
  size_t i;
  for (i=0;i<flat.count;i++) if (flat.items[i]->changed) any_changed = 1;

  struct diff_node_list added = {0},removed = {0};
  collect_topmost_changes(root_diff,&added,&removed);
  int any_diff = any_changed || added.count > 0 || removed.count > 0;

  if (json_out){
    fprintf(stdout,"{");
    fprintf(stdout,"\"schema_version\":"); json_write_string(stdout,PROCTREE_JSON_SCHEMA_VERSION); fprintf(stdout,",");
    fprintf(stdout,"\"run_a\":{\"source_file\":"); json_write_string(stdout,file_a);
    fprintf(stdout,",\"process_count\":%.0f,\"max_concurrent_processes\":%.0f},",
	    json_get_number(root_a,"process_count",0),json_get_number(root_a,"max_concurrent_processes",0));
    fprintf(stdout,"\"run_b\":{\"source_file\":"); json_write_string(stdout,file_b);
    fprintf(stdout,",\"process_count\":%.0f,\"max_concurrent_processes\":%.0f},",
	    json_get_number(root_b,"process_count",0),json_get_number(root_b,"max_concurrent_processes",0));
    fprintf(stdout,"\"diff_metrics\":[");
    for (i=0;i<NUM_DIFF_METRICS;i++){
      if (i) fprintf(stdout,",");
      json_write_string(stdout,diff_metrics[i].json_key);
    }
    fprintf(stdout,"],");
    fprintf(stdout,"\"summary_diff\":"); print_summary_diff_json(stdout,summary_a,summary_b); fprintf(stdout,",");
    fprintf(stdout,"\"diff_tree\":"); print_diff_node_json(stdout,root_diff);
    fprintf(stdout,"}\n");
  } else {
    fprintf(stdout,"proctree diff: %s vs %s\n",file_a,file_b);
    fprintf(stdout,"run A: %.0f processes (max concurrent %.0f)\n",
	    json_get_number(root_a,"process_count",0),json_get_number(root_a,"max_concurrent_processes",0));
    fprintf(stdout,"run B: %.0f processes (max concurrent %.0f)\n\n",
	    json_get_number(root_b,"process_count",0),json_get_number(root_b,"max_concurrent_processes",0));

    if (print_diff_text_summary(stdout,summary_a,summary_b)) any_diff = 1;

    if (added.count || removed.count){
      fprintf(stdout,"Added/removed subtrees:\n");
      for (i=0;i<added.count;i++) print_added_removed_subtree(stdout,added.items[i],"+ added subtree:");
      for (i=0;i<removed.count;i++) print_added_removed_subtree(stdout,removed.items[i],"- removed subtree:");
      fprintf(stdout,"\n");
    }

    qsort(flat.items,flat.count,sizeof(*flat.items),changed_node_compare);
    size_t num_changed = 0;
    for (i=0;i<flat.count;i++) if (flat.items[i]->changed) num_changed++;
    if (num_changed){
      fprintf(stdout,"Changed (sorted by |delta utime+stime|, top %d):\n",MAX_DIFF_REPORT_ROWS);
      size_t shown = 0;
      for (i=0;i<flat.count && shown<MAX_DIFF_REPORT_ROWS;i++){
	struct diff_node *dn = flat.items[i];
	if (!dn->changed) continue;
	fprintf(stdout,"  ~ %-40s utime=%.3f->%.3f (%+.3f) stime=%.3f->%.3f (%+.3f)\n",
		dn->path,
		json_get_number(dn->a,"utime_seconds",0),json_get_number(dn->b,"utime_seconds",0),
		dn->delta[DIFF_METRIC_UTIME],
		json_get_number(dn->a,"stime_seconds",0),json_get_number(dn->b,"stime_seconds",0),
		dn->delta[DIFF_METRIC_STIME]);
	shown++;
      }
      if (num_changed > shown){
	fprintf(stdout,"  (+%zu more)\n",num_changed-shown);
      }
      fprintf(stdout,"\n");
    }

    fprintf(stdout,"summary: %zu subtree%s added, %zu removed, %zu matched (%zu changed beyond %.3fs threshold)\n",
	    added.count,added.count == 1 ? "" : "s",removed.count,flat.count,num_changed,threshold);
  }

  return any_diff ? 1 : 0;
}


#ifndef TEST_PROCTREE
int main(int argc,char *const argv[],char *const envp[]){
#else
static int original_main(int argc,char *const argv[],char *const envp[]){
#endif
  int opt;
  int value;
  char *p,*p2,*cmd;
  char *orig_buffer;
  char buffer[8192];
  double elapsed;
  int event_pid;

  clocks_per_second = sysconf(_SC_CLK_TCK);
  page_size = sysconf(_SC_PAGESIZE);

  initialize_error_subsystem(argv[0],"-");

  // parse options
  static struct option long_options[] = {
    {"json",no_argument,0,OPT_JSON},
    {"diff",no_argument,0,OPT_DIFF},
    {"diff-threshold",required_argument,0,OPT_DIFF_THRESHOLD},
    {0,0,0,0}
  };
  while ((opt = getopt_long(argc,argv,"+BbCcDdFfIiJjKkLlMmNnPpRrSsTtUuvw:XxZz",long_options,NULL)) != -1){
    switch(opt){
    case OPT_JSON:
      json_flag = 1;
      break;
    case OPT_DIFF:
      diff_flag = 1;
      break;
    case OPT_DIFF_THRESHOLD:
      if (sscanf(optarg,"%lf",&diff_threshold) != 1){
	warning("bad diff threshold:%s ignored\n",optarg);
      }
      break;
    case 'B':
      bflag = 1;
      break;
    case 'b':
      bflag = 0;
      break;
    case 'C':
      print_cmdline = 1;
      break;
    case 'c':
      print_cmdline = 0;
      break;
    case 'D':
      dflag = 1;
      break;
    case 'd':
      dflag = 0;
      break;
    case 'F':
      fflag = 1;
      break;
    case 'f':
      fflag = 0;
      break;
    case 'I':
      iflag = 1;
      break;
    case 'i':
      iflag = 0;
      break;
    case 'J':
      jflag = 1;
      break;
    case 'j':
      jflag = 0;
      break;
    case 'K':
      kflag = 1;
      break;
    case 'k':
      kflag = 0;
      break;
    case 'L':
      lflag = 1;
      break;
    case 'l':
      lflag = 0;
      break;
    case 'M':
      mflag = 1;
      break;
    case 'm':
      mflag = 0;
      break;
    case 'N':
      nflag = 1;
      break;
    case 'n':
      nflag = 0;
      break;
    case 'P':
      pflag = 1;
      break;
    case 'p':
      pflag = 0;
      break;
    case 'R':
      rflag = 1;
      break;
    case 'r':
      rflag = 0;
      break;
    case 't':
      treeflag = 0;
      break;
    case 'T':
      treeflag = 1;
      break;
    case 's':
      statflag = 0;
      break;
    case 'S':
      statflag = 1;
      break;
    case 'U':
      uflag = 1;
      break;
    case 'u':
      uflag = 0;
      break;
    case 'v':
      vflag++;
      if (vflag>1) set_error_level(ERROR_LEVEL_DEBUG2);
      else set_error_level(ERROR_LEVEL_DEBUG);
      break;
    case 'X':
      futexflag = 1;
      break;
    case 'x':
      futexflag = 0;
      break;
    case 'w':
      if (sscanf(optarg,"%d",&value) == 1){
	output_width = value;
      } else {
	warning("bad output width:%s ignored\n",optarg);
      }
      break;
    case 'Z':
      zflag = 1;
      break;
    case 'z':
      zflag = 0;
      break;
    default:
    usage:
      fatal("usage: %s -[BbCcDdFfIiJjKkLlMmNnPpRrSsTtUuvXxZz][-w width] [--json] file\n"
	    "       %s --diff [--json] [--diff-threshold secs] a.json b.json\n"
	    "\t-B\tturn on I/O-wait-time printing (per-pid and summary totals)\n"
	    "\t-b\tturn off I/O-wait-time printing (default)\n"
	    "\t-C\tturn on longer command line\n"
	    "\t-c\tturn on abbreviated command (default)\n"
	    "\t-D\tturn on run-queue-delay printing (per-pid and summary totals)\n"
	    "\t-d\tturn off run-queue-delay printing (default)\n"
	    "\t-F\tturn on start/finish info (default)\n"
	    "\t-f\tturn off start/finish info\n"
	    "\t-I\tturn on /proc/<pid>/io byte-counter printing (per-pid and summary totals)\n"
	    "\t-i\tturn off /proc/<pid>/io byte-counter printing (default)\n"
	    "\t-J\tturn on wait4/waitid blocking-time printing (per-pid and summary totals)\n"
	    "\t-j\tturn off wait4/waitid blocking-time printing (default)\n"
	    "\t-K\tturn on connect() latency printing (per-pid and summary totals)\n"
	    "\t-k\tturn off connect() latency printing (default)\n"
	    "\t-L\tturn on poll/select/epoll_wait blocking-time printing (per-pid and summary totals)\n"
	    "\t-l\tturn off poll/select/epoll_wait blocking-time printing (default)\n"
	    "\t-M\tturn on vmsize/rss printing\n"
	    "\t-m\tturn off vmsize/rss printing (default)\n"
	    "\t-N\tturn on thread-count printing\n"
	    "\t-n\tturn off thread-count printing (default)\n"
	    "\t-P\tturn on parent-pid printing\n"
	    "\t-p\tturn off parent-pid printing (default)\n"
	    "\t-R\tturn on peak-RSS/anon-file-shmem-RSS/swap printing (per-pid and summary totals)\n"
	    "\t-r\tturn off peak-RSS/anon-file-shmem-RSS/swap printing (default)\n"
	    "\t-S\tturn on summary output (default)\n"
	    "\t-s\tturn off summary output\n"
	    "\t-T\tturn on tree output (default)\n"
	    "\t-t\tturn off tree output\n"
	    "\t-U\tturn on utime in tree\n"
	    "\t-u\tturn off utime in tree\n"
	    "\t-v\tverbose messages\n"
	    "\t-w width\tset command width\n"
	    "\t-X\tturn on futex-wait-time printing (per-pid and summary totals)\n"
	    "\t-x\tturn off futex-wait-time printing (default)\n"
	    "\t-Z\tturn on nanosleep/clock_nanosleep time printing (per-pid and summary totals)\n"
	    "\t-z\tturn off nanosleep/clock_nanosleep time printing (default)\n"
	    "-C/-M/-N/-P/-U only control what gets *printed* -- ppid/thread-count/\n"
	    "vmsize/rss/utime/stime are always present in the tree file regardless\n"
	    "of what flags wspy --tree was run with, so any combination of these\n"
	    "works on an existing file with no need to re-run wspy. The one\n"
	    "exception is -C's full command line, which only prints anything if\n"
	    "wspy was run with --tree-cmdline in the first place -- and likewise\n"
	    "-X's futex data, -B's I/O-wait data, -I's I/O byte-counter data,\n"
	    "-D's run-queue-delay data, -R's peak-RSS/RSS-composition/swap data,\n"
	    "-K's connect() latency, -Z's nanosleep time, -J's wait4/waitid time,\n"
	    "and -L's poll/select/epoll_wait time, each of which only prints\n"
	    "anything (beyond zeroes) if wspy was run with --tree-futex,\n"
	    "--tree-io-wait, --tree-io, --tree-schedstat, --tree-vmsize,\n"
	    "--tree-connect, --tree-nanosleep, --tree-wait, or --tree-poll\n"
	    "respectively.\n"
	    "\t--json\temit one JSON document (tree + per-comm summary) instead of\n"
	    "\t\tthe text tree/summary above; ignores every -[BbCcDdFfIiJjKkLlMmNnPpRrRsSTtUuvXxZz]\n"
	    "\t\ttoggle (JSON always includes every field, so a viewer can choose\n"
	    "\t\twhich columns to show)\n"
	    "\t--diff\trun-to-run tree diff: takes exactly two --json-exported files\n"
	    "\t\t(not raw wspy --tree output) as the trailing arguments and\n"
	    "\t\tmatches subtrees by ancestor-comm-path + sibling occurrence\n"
	    "\t\torder; add --json for a machine-readable merged diff tree\n"
	    "\t\tinstead of the default human-readable report; exits 1 if any\n"
	    "\t\tdifference was found, 0 if the two trees matched exactly\n"
	    "\t--diff-threshold secs\tminimum |delta utime+stime| for a matched\n"
	    "\t\tnode to count as \"changed\" rather than \"same\" (default 0.01)\n",
	    argv[0],argv[0]);
      break;
    }
  }

  if (diff_flag){
    if (optind >= argc){
      fatal("missing first JSON file (usage: %s --diff [--json] <a.json> <b.json>)\n",argv[0]);
    }
    if (optind+1 >= argc){
      fatal("missing second JSON file (usage: %s --diff [--json] <a.json> <b.json>)\n",argv[0]);
    }
    if (argc > optind+2){
      warning("extra arguments after %s %s ignored\n",argv[optind],argv[optind+1]);
    }
    return run_diff(argv[optind],argv[optind+1],json_flag,diff_threshold);
  }

  if (optind >= argc){
    fatal("missing data file\n");
  } else if (argc > optind + 1){
    warning("extra arguments after filename %s ignored\n",argv[optind]);
  }
  if ((process_file = fopen(argv[optind],"r")) == NULL){
    fatal("can not open file %s\n",argv[optind]);
  }

  // parse the input file
  while (fgets(buffer,sizeof(buffer),process_file) != NULL){
    if ((p = strchr(buffer,' '))) *p = 0;
    else continue;
    elapsed = strtod(buffer,&p2);
    if (p2 == buffer) // no value parsed, there can be newlines in input so ignore
      continue;
    
    p++;
    
    if ((p2 = strchr(p,' '))) *p2 = 0;
    else continue;
    event_pid = atoi(p); // no value parsed, there can be newlines in input so ignore
    if (event_pid <= 0) continue;
    p = p2+1;
    if ((p2 = strchr(p,'\n'))) *p2 = 0; // zap newline
    
    if (!strncmp(p,"fork",4)){
      handle_fork(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"root",4)){
      handle_root(elapsed,event_pid);
    } else if (!strncmp(p,"comm",4)){
      handle_comm(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"cmdline",7)){
      handle_cmdline(elapsed,event_pid,p+8);
    } else if (!strncmp(p,"exec",4)){
      handle_exec(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"exited",6)){
      // ignore exited events.
    } else if (!strncmp(p,"unknown",7)){
      // ignore unknown events
    } else if (!strncmp(p,"signal",6)){
      // ignore signal events
    } else if (!strncmp(p,"futex",5)){
      handle_futex(elapsed,event_pid,p+6);
    } else if (!strncmp(p,"io_wait",7)){
      handle_io_wait(elapsed,event_pid,p+8);
    } else if (!strncmp(p,"io",2)){
      handle_io(elapsed,event_pid,p+3);
    } else if (!strncmp(p,"schedstat",9)){
      handle_schedstat(elapsed,event_pid,p+10);
    } else if (!strncmp(p,"vmsize",6)){
      handle_vmdetail(elapsed,event_pid,p+7);
    } else if (!strncmp(p,"connect",7)){
      handle_connect(elapsed,event_pid,p+8);
    } else if (!strncmp(p,"nanosleep",9)){
      handle_nanosleep(elapsed,event_pid,p+10);
    } else if (!strncmp(p,"wait",4)){
      handle_wait(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"poll",4)){
      handle_poll(elapsed,event_pid,p+5);
    } else if (!strncmp(p,"exit",4)){
      handle_exit(elapsed,event_pid,p+5);
    } else {
      warning("unknown command: %4.2f %d %s\n",elapsed,event_pid,p);
    }
  }

  if (json_flag){
    print_json(stdout,argv[optind]);
  } else {
    if (statflag) print_statistics();
    if (treeflag) print_tree(root_process,0);
  }

  return 0;
}
