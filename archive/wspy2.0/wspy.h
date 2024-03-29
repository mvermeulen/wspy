/* wspy.h - header definitions for wspy program */
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>

/* wspy.c */
extern FILE *outfile;
extern int num_procs;
extern pthread_mutex_t event_lock;

/* procinfo.c */
#define NUM_COUNTERS_PER_PROCESS 6 // for now just instructions & cycles

struct process_counter_info {
  int perf_fd[NUM_COUNTERS_PER_PROCESS];
  unsigned long perf_counter[NUM_COUNTERS_PER_PROCESS];
};

struct open_file_info {
  int result;
  char *filename;
  struct open_file_info *next;
  struct open_file_info *prev;
};

struct process_info {
  pid_t pid;
  pid_t ppid;
  int cpu;
  unsigned int f_exited : 1; // ftrace
  unsigned int p_exited : 1; // ptrace
  unsigned int printed : 1;
  unsigned int cloned : 1;
  unsigned int sibling_order : 1;
  unsigned int counters_started : 1;
  char *comm;
  char *filename;
  int pcount;
  struct process_counter_info pci;
  //  int perf_fd[NUM_COUNTERS_PER_PROCESS];
  //  unsigned long perf_counter[NUM_COUNTERS_PER_PROCESS];
  unsigned long total_counter[NUM_COUNTERS_PER_PROCESS];
  unsigned long long starttime;
  unsigned long minflt,majflt;
  //  struct timeval time_fork;
  //  struct timeval time_exec;
  //  struct timeval time_exit;
  double time_start,time_finish;
  unsigned long utime,stime,total_utime,total_stime,vsize,rss;
  unsigned long cutime,cstime;
  struct open_file_info *syscall_open;
  struct process_info *parent;
  struct process_info *sibling;
  struct process_info *child;
};
typedef struct process_info procinfo;
procinfo *lookup_process_info(pid_t pid,int insert);
void add_process_syscall_open(procinfo *pinfo,char *filename,int result);
void print_process_tree(FILE *output,procinfo *pinfo,int level,double basetime);
void print_all_process_trees(FILE *output,double basetime,char *name);
int print_all_processes_csv(FILE *output);
void finalize_process_tree(void);

/* ftrace.c */
extern double first_ftrace_time;
extern int ftrace_cmd_pipe[2]; // command pipe
void *ftrace_start(void *arg);

/* tracecmd.c */
extern pid_t tracecmd_pid;
void *tracecmd_start(void *arg);

/* ptrace.c */
void ptrace_setup(pid_t child);
void ptrace_loop(void);
char *lookup_process_comm(pid_t pid);
pid_t lookup_process_ppid(pid_t pid);
// fields from /proc/stat
struct procstat_info {
  /*  1- 5 */ int pid; char comm[32]; char state; int ppid, pgrp;
  /*  6-10 */ int session, tty_nr, tpgid; unsigned int flags; unsigned long minflt;
  /* 11-15 */ unsigned long cminflt, majflt, cmajflt, utime, stime;
  /* 16-20 */ long cutime, cstime, priority, nice, num_threads;
  /* 21-25 */ long itrealvalue; unsigned long long starttime; unsigned long vsize;
              long rss; unsigned long rsslim;
  /* 26-30 */ unsigned long startcode, endcode, startstack, kstkesp, kstkeip;
  /* 31-35 */ unsigned long signal, blocked, sigignore, sigcatch, wchan;
  /* 36-40 */ unsigned long nswap, cnswap; int exit_signal, processor; unsigned rt_priority;
  /* 41-45 */ unsigned policy; unsigned long long delayacct_blkio_ticks;
              unsigned long guest_time, cguest_time, start_data;
  /* 46-50 */ unsigned long end_data, start_brk, arg_start, arg_end, end_start;
  /* 51-52 */ unsigned long env_end; int exit_code;
};
char *lookup_process_stat(pid_t pid);
char *lookup_process_task_stat(pid_t pid);
int parse_process_stat(char *line,struct procstat_info *pi);

/* ptrace2.c */
void ptrace2_setup(pid_t child);
void ptrace2_loop(void);
void ptrace2_finish(void);
extern int pid_max;

/* timer.c */
extern int timer_interval;
extern int timer_cmd_pipe[2]; // command pipe
void *timer_start(void *arg);
void read_uptime(double *td);

/* cpustatus.c */
void init_cpustats(void);
void read_cpustats(double time);
void print_cpustats(void);
void print_cpustats_files(void);

/* diskstats.c */
void init_diskstats(void);
void read_diskstats(double time);
void print_diskstats(void);
void print_diskstats_files(void);

/* memstats.c */
void init_memstats(void);
void read_memstats(double time);
void print_memstats(void);
void print_memstats_files(void);

/* netstats.c */
void init_netstats(void);

/* pcounter.c */
struct counterinfo {
  char *name;
  char *group;
  uint32_t type;
  uint64_t config;
  uint64_t config1;
  char *directory;
  unsigned int has_scale : 1;
  unsigned int has_unit  : 1;
  unsigned int is_multiple : 1;
  unsigned int scale;
};
extern struct counterinfo *countertable;
extern int num_countertable;
void init_global_perf_counters(void);
void read_global_perf_counters(double time);
void print_global_perf_counters(void);
void print_global_perf_counter_files(void);
void init_process_counterinfo(void);
struct counterinfo *counterinfo_lookup(char *name,char *group,int insert);
void inventory_counters(char *directory);
void print_counters(FILE *fp);
void sort_counters(void);
void start_process_perf_counters(pid_t pid,struct process_counter_info *pci,int root);
void stop_process_perf_counters(pid_t pid,struct process_counter_info *pci);

struct counterlist {
  char *name;
  unsigned long value;
  int fd;
  struct counterinfo *ci;
  struct counterlist *next;
};
#define MAXCPU 32
extern int all_counters_same;
extern struct counterlist *perf_counters_by_cpu[MAXCPU];
extern struct counterlist *perf_counters_same;
extern struct counterlist *perf_counters_by_process[NUM_COUNTERS_PER_PROCESS];

/* config.c */
extern int command_line_argc;
extern char **command_line_argv;
extern int version;
extern int flag_cmd;
extern int flag_cpustats;
extern int flag_debug;
extern int flag_version;
extern int flag_diskstats;
extern int flag_memstats;
extern int flag_netstats;
extern int flag_perfctr;
extern int flag_proctree;
extern int flag_syscall;
extern int flag_rusage;
enum perfcounter_model { PM_DEFAULT=0, PM_CORE=1,
  PM_PROCESS=2, PM_APPLICATION=3, };
extern enum perfcounter_model perfcounter_model;
extern int flag_showcounters;
// four possible types of processtree engines
extern int mask_processtree_engine_selected;
#define PROCESSTREE_FTRACE   0x1   // parse results of ftrace into in memory tree
#define PROCESSTREE_TRACECMD 0x2   // external invoke trace-cmd to create *.dat file
#define PROCESSTREE_PTRACE1  0x4   // get ptrace events into in memory tree
#define PROCESSTREE_PTRACE2  0x8   // get ptrace events into dump of processtree.csv file
extern int flag_require_ftrace;
extern int flag_require_ptrace;
extern int flag_require_ptrace2;
extern int flag_require_tracecmd;

extern int flag_require_timer;
extern int flag_require_counters;
extern int flag_require_perftimer;
extern int flag_require_perftree;
extern int flag_require_perfapp;
extern int flag_setcpumask;
extern int flag_set_uid;
extern int flag_zip;
extern int uid_value;
extern cpu_set_t cpumask;
extern char *zip_archive_name;
extern char *command_name;
void read_config_file(char *name);
int parse_options(int argc,char *const argv[]);
char *lookup_vendor(void);

/* vendor.c */
char *lookup_vendor(void);
