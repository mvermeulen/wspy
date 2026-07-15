/*
 * wspy.h - common definitions
 */

#include <stdio.h>
#include <time.h>
#include <sys/resource.h>
#include "cpu_info.h"

#define WSPY_VERSION_MAJOR 4
#define WSPY_VERSION_MINOR 1

extern FILE *outfile;

extern int aflag;
extern int oflag;
extern int sflag;
extern int vflag;
extern int xflag;
extern int csvflag;
extern int interval;
extern int phase_flag;
extern int treeflag;
extern int tree_cmdline;
extern int tree_open;
extern int tree_vmsize;
extern int trace_syscall;
extern int versionflag;
extern FILE *treefile;
extern char *outfile_path;
extern char *tree_output_path;
extern char *manifest_path;
extern char *run_index_path;

extern int num_procs;
extern int clocks_per_second;
extern struct timespec start_time,finish_time;

#define COUNTER_IPC         0x1
#define COUNTER_TOPDOWN     0x2
#define COUNTER_TOPDOWN2    0x4
#define COUNTER_TOPDOWN_FE  0x8
#define COUNTER_TOPDOWN_BE  0x10
#define COUNTER_TOPDOWN_OP  0x20
#define COUNTER_BRANCH      0x40
#define COUNTER_DCACHE      0x80
#define COUNTER_ICACHE      0x100
#define COUNTER_L1CACHE     0x200
#define COUNTER_L2CACHE     0x400
#define COUNTER_L3CACHE     0x800
#define COUNTER_MEMORY      0x1000
#define COUNTER_TLB         0x2000
#define COUNTER_OPCACHE     0x4000
#define COUNTER_SOFTWARE    0x8000
#define COUNTER_FLOAT       0x10000
/* AMD IBS (ibs-basic/ibs-memory-deep collection profiles, see ibs.h). Not
 * folded into COUNTER_ALL: --capabilities already discovers IBS support via
 * its own dedicated ibs_probe()/print_ibs_capability_report() path, so a
 * generic "probe everything" run has no need to also open real IBS counting
 * events (which requires an explicit profile choice, see ibs_collection_profile). */
#define COUNTER_IBS         0x20000

/* every counter type wspy knows how to request; used by --capabilities to
 * probe the full set regardless of what counter flags were also given */
#define COUNTER_ALL (COUNTER_IPC|COUNTER_TOPDOWN|COUNTER_TOPDOWN2|COUNTER_TOPDOWN_FE| \
		     COUNTER_TOPDOWN_BE|COUNTER_TOPDOWN_OP|COUNTER_BRANCH|COUNTER_DCACHE| \
		     COUNTER_ICACHE|COUNTER_L1CACHE|COUNTER_L2CACHE|COUNTER_L3CACHE| \
		     COUNTER_MEMORY|COUNTER_TLB|COUNTER_OPCACHE|COUNTER_SOFTWARE|COUNTER_FLOAT)

extern unsigned int counter_mask;

#define SYSTEM_LOADAVG      0x1
#define SYSTEM_CPU          0x2
#if AMDGPU
#define SYSTEM_GPU          0x4
#endif
#define SYSTEM_NETWORK      0x8

extern unsigned int system_mask;

extern pid_t child_pid;
extern int child_pipe[2];
extern volatile int is_still_running;

/* Set by check_nmi_watchdog() (topdown.c); read directly (rather than by
 * re-invoking check_nmi_watchdog(), which would re-print its warning) by
 * anything that needs the already-determined NMI-watchdog state, e.g.
 * preflight.c's slot-budget estimate. */
extern int nmi_running;

/* Root child's exit status; set in wspy.c (non-tree, via wait4()) or
 * topdown.c's ptrace_loop() (--tree mode). See topdown.c for details. */
extern int child_exit_known;
extern int child_exited;
extern int child_exit_code;
extern int child_signaled;
extern int child_term_signal;

enum output_format { PRINT_NORMAL, PRINT_CSV, PRINT_CSV_HEADER };

int check_nmi_watchdog(void);
int setup_raw_events(void);
extern struct raw_event amd_raw_events[];
int amd_raw_events_count(void);
void setup_counter_groups(struct counter_group **counter_group_list);
struct counter_group *software_counter_group(char *name);
void setup_counters(struct counter_group *counter_group_list);
void start_counters(struct counter_group *counter_group_list);
void read_counters(struct counter_group *counter_group_list,int stop_counters);
void close_counters(struct counter_group *counter_group_list);
void print_usage(struct rusage *rusage,enum output_format oformat);
void print_metrics(struct counter_group *counter_group_list,enum output_format oformat);
struct counter_info *find_ci_label(struct counter_group *cgroup,char *label);
int launch_child(int argc,char *const argv[],char *const envp[]);
void timer_callback(int signum);
void ptrace_setup(pid_t child_pid);
void ptrace_loop(void);
// system.c
void read_system(void);
void print_system(enum output_format oformat);
