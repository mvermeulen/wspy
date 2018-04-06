/* wspy.h - header definitions for wspy program */
#include <sys/types.h>
#include <inttypes.h>
#include <sched.h>

/* wspy.c */
FILE *outfile;
int num_procs;

/* procinfo.c */
struct process_info {
  pid_t pid;
  pid_t ppid;
  int cpu;
  unsigned int exited  : 1;
  unsigned int printed : 1;
  unsigned int sibling_order : 1;
  char *comm;
  char *filename;
  int pcount;
  struct timeval time_fork;
  struct timeval time_exec;
  struct timeval time_exit;  
  struct process_info *parent;
  struct process_info *sibling;
  struct process_info *child;
};
typedef struct process_info procinfo;
procinfo *lookup_process_info(pid_t pid,int insert);
void print_process_tree(FILE *output,procinfo *pinfo,int level,double basetime);
void print_all_process_trees(FILE *output,double basetime,char *name);
void finalize_process_tree(void);
double find_first_process_time(char *name);

/* ktrace.c */
int ktrace_cmd_pipe[2]; // command pipe
void *ktrace_start(void *arg);

/* timer.c */
int timer_cmd_pipe[2]; // command pipe
void *timer_start(void *arg);

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
  int has_scale : 1;
  int has_unit  : 1;
  int is_multiple : 1;
};
struct counterinfo *countertable;
int num_countertable;
void init_perf_counters(void);
void read_perf_counters(double time);
void print_perf_counters(void);
void print_perf_counter_files(void);
void inventory_counters(void);

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
int flag_showcounters;
int flag_require_ftrace;
int flag_require_timer;
int flag_require_counters;
int flag_setcpumask;
int flag_set_uid;
int flag_zip;
int uid_value;
cpu_set_t cpumask;
char *zip_archive_name;
char *command_name;
void read_config_file(void);
int parse_options(int argc,char *const argv[]);
