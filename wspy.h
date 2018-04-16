/* wspy.h - header definitions for wspy program */
#include <sys/types.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>

/* wspy.c */
FILE *outfile;
int num_procs;
pthread_mutex_t event_lock;

/* procinfo.c */
#define NUM_COUNTERS 2 // for now just instructions & cycles
struct process_info {
  pid_t pid;
  pid_t ppid;
  int cpu;
  unsigned int f_exited : 1; // ftrace
  unsigned int p_exited : 1; // ptrace
  unsigned int printed : 1;
  unsigned int cloned : 1;
  unsigned int sibling_order : 1;
  char *comm;
  char *filename;
  int pcount;
  int perf_fd[NUM_COUNTERS];
  unsigned long perf_counter[NUM_COUNTERS];
  unsigned long total_counter[NUM_COUNTERS];
  unsigned long long starttime;
  unsigned long minflt,majflt;
  struct timeval time_fork;
  struct timeval time_exec;
  struct timeval time_exit;
  unsigned long utime,stime,total_utime,total_stime,vsize,rss;
  struct process_info *parent;
  struct process_info *sibling;
  struct process_info *child;
};
typedef struct process_info procinfo;
procinfo *lookup_process_info(pid_t pid,int insert);
void print_process_tree(FILE *output,procinfo *pinfo,int level,double basetime);
void print_all_process_trees(FILE *output,double basetime,char *name);
int print_all_processes_csv(FILE *output);
void finalize_process_tree(void);
double find_first_process_time(char *name);

/* ftrace.c */
int ftrace_cmd_pipe[2]; // command pipe
void *ftrace_start(void *arg);

/* ptrace.c */
void ptrace_setup(pid_t child);
void ptrace_loop(void);

/* timer.c */
int timer_interval;
int timer_cmd_pipe[2]; // command pipe
void *timer_start(void *arg);
void read_uptime(struct timeval *tm);

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
};
struct counterinfo *countertable;
int num_countertable;
void init_global_perf_counters(void);
void read_global_perf_counters(double time);
void print_global_perf_counters(void);
void print_global_perf_counter_files(void);
struct counterinfo *counterinfo_lookup(char *name,char *group,int insert);
void inventory_counters(char *directory);
void print_counters(FILE *fp);
void sort_counters(void);
void start_process_perf_counters(procinfo *pinfo);
void stop_process_perf_counters(procinfo *pinfo);

struct counterlist {
  char *name;
  long value;
  int fd;
  struct counterinfo *ci;
  struct counterlist *next;
};
#define MAXCPU 16
struct counterlist *perf_counters_by_cpu[MAXCPU];

/* config.c */
int command_line_argc;
char **command_line_argv;
int flag_cmd;
int flag_cpustats;
int flag_debug;
int flag_diskstats;
int flag_memstats;
int flag_netstats;
int flag_perfctr;
int flag_proctree;
enum proctree_engine { PT_DEFAULT=0, PT_FTRACE=1, PT_PTRACE=2, PT_ALL=3 } proctree_engine;
enum perfcounter_model { PM_DEFAULT=0, PM_CORE=1, PM_PROCESS } perfcounter_model;
int flag_showcounters;
int flag_require_ftrace;
int flag_require_ptrace;
int flag_require_timer;
int flag_require_counters;
int flag_require_perftimer;
int flag_require_perftree;
int flag_setcpumask;
int flag_set_uid;
int flag_zip;
int uid_value;
cpu_set_t cpumask;
char *zip_archive_name;
char *command_name;
void read_config_file(void);
int parse_options(int argc,char *const argv[]);
