/*
 * wspy.h - common definitions
 */

#include <stdio.h>
#include <time.h>
#include <sys/resource.h>
#include "cpu_info.h"

#define WSPY_VERSION_MAJOR 4
#define WSPY_VERSION_MINOR 1
// Point release, bumped for a correctness-only fix that doesn't warrant a
// MINOR bump (no new feature/flag) but is significant enough that the
// binary's own --version output should distinguish it from plain 4.1 --
// see the "raw event .config left unparsed for non-default --passes
// groups" fix. 0 for a release with no patch-level fix yet.
#define WSPY_VERSION_PATCH 1

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
extern int tree_futex;
extern int tree_io;
extern int tree_io_wait;
extern int tree_schedstat;
extern int tree_vmsize;
extern int tree_connect;
extern int tree_nanosleep;
extern int tree_wait;
extern int tree_poll;
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
#define COUNTER_ARM_DCACHE_MEM 0x40000
#define COUNTER_ARM_ICACHE_TLB 0x80000
#define COUNTER_ARM_MEM_ALIGN_TLB 0x100000
/* CPU energy/power via the Linux power/energy-pkg dynamic PMU (RAPL-
 * equivalent, see power.h). Not folded into COUNTER_ALL, same reasoning as
 * COUNTER_IBS above: --capabilities gets its own dedicated
 * power_probe()/print_power_capability_report() discovery path, so a
 * generic "probe everything" run doesn't need to also open a real energy
 * counter. */
#define COUNTER_POWER       0x200000
/* Per-core energy (power_core/energy-core, see power.h), INVESTIGATION.md's
 * 4.2 Tier 1 "Per-core energy support" item. Internal-only: never a
 * user-facing flag, never folded into COUNTER_ALL -- set automatically
 * (wspy.c's main(), --per-core setup) only when --power and --per-core are
 * both already in effect. Needs its own bit (not reused from COUNTER_POWER)
 * because it's a structurally different group -- print_metrics() (topdown.c)
 * dispatches print_power() vs print_power_core() by mask, same as every
 * other pair of related-but-differently-shaped groups (e.g. COUNTER_TOPDOWN
 * vs COUNTER_TOPDOWN_BE). */
#define COUNTER_POWER_CORE  0x400000

/* every counter type wspy knows how to request; used by --capabilities to
 * probe the full set regardless of what counter flags were also given */
#define COUNTER_ALL (COUNTER_IPC|COUNTER_TOPDOWN|COUNTER_TOPDOWN2|COUNTER_TOPDOWN_FE| \
		     COUNTER_TOPDOWN_BE|COUNTER_TOPDOWN_OP|COUNTER_BRANCH|COUNTER_DCACHE| \
		     COUNTER_ICACHE|COUNTER_L1CACHE|COUNTER_L2CACHE|COUNTER_L3CACHE| \
		     COUNTER_MEMORY|COUNTER_TLB|COUNTER_OPCACHE|COUNTER_SOFTWARE|COUNTER_FLOAT| \
		     COUNTER_ARM_DCACHE_MEM|COUNTER_ARM_ICACHE_TLB|COUNTER_ARM_MEM_ALIGN_TLB)

/* Version tag for topdown.c's print_topdown() percentage-decomposition
 * formulas -- which raw events feed each L1/L2 node, how SMT contention is
 * computed/subtracted, which denominator (slots_no_contention) drives
 * classification. Bump when that formula changes, independent of
 * MANIFEST_SCHEMA_VERSION, so two runs' topdown numbers can be checked for
 * "were these computed the same way" without diffing source. Recorded in
 * manifest.h's struct manifest_info (NULL when a run collects no topdown
 * counters at all). */
#define TOPDOWN_FORMULA_VERSION "1.0"

extern unsigned int counter_mask;

#define SYSTEM_LOADAVG      0x1
#define SYSTEM_CPU          0x2
#define SYSTEM_GPU          0x4
#define SYSTEM_NETWORK      0x8
#define SYSTEM_FREQ         0x10
#define SYSTEM_TEMP         0x20

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
void check_schedstat_enabled(void);
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
