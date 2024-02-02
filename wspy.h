/*
 * wspy.h - common definitions
 */

#include <stdio.h>
#include <time.h>
#include <sys/resource.h>
#include "cpu_info.h"
extern FILE *outfile;

extern int aflag;
extern int oflag;
extern int sflag;
extern int vflag;
extern int xflag;
extern int csvflag;
extern int interval;
extern int treeflag;
extern int tree_cmdline;
extern int tree_open;
extern int trace_syscall;
extern FILE *treefile;

extern int num_procs;
extern int clocks_per_second;
extern struct timespec start_time,finish_time;

#define COUNTER_IPC         0x1
#define COUNTER_TOPDOWN     0x2
#define COUNTER_TOPDOWN2    0x4
#define COUNTER_TOPDOWN_FE  0x8
#define COUNTER_TOPDOWN_OP  0x10
#define COUNTER_BRANCH      0x20
#define COUNTER_DCACHE      0x40
#define COUNTER_ICACHE      0x80
#define COUNTER_L1CACHE     0x100
#define COUNTER_L2CACHE     0x200
#define COUNTER_L3CACHE     0x400
#define COUNTER_MEMORY      0x800
#define COUNTER_TLB         0x1000
#define COUNTER_OPCACHE     0x2000
#define COUNTER_SOFTWARE    0x4000
#define COUNTER_FLOAT       0x8000

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

enum output_format { PRINT_NORMAL, PRINT_CSV, PRINT_CSV_HEADER };

int check_nmi_watchdog(void);
int setup_raw_events(void);
void setup_counter_groups(struct counter_group **counter_group_list);
struct counter_group *software_counter_group(char *name);
void setup_counters(struct counter_group *counter_group_list);
void start_counters(struct counter_group *counter_group_list);
void read_counters(struct counter_group *counter_group_list,int stop_counters);
void print_usage(struct rusage *rusage,enum output_format oformat);
void print_metrics(struct counter_group *counter_group_list,enum output_format oformat);
int launch_child(int argc,char *const argv[],char *const envp[]);
void timer_callback(int signum);
void ptrace_setup(pid_t child_pid);
void ptrace_loop(void);
// system.c
void read_system(void);
void print_system(enum output_format oformat);
