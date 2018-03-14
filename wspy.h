/* wspy.h - header definitions for wspy program */
#include <sys/types.h>

/* wspy.c */
FILE *outfile;

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
  struct timeval time_fork;
  struct timeval time_exec;
  struct timeval time_exit;  
  struct process_info *parent;
  struct process_info *sibling;
  struct process_info *child;
};
typedef struct process_info procinfo;
procinfo *lookup_process_info(pid_t pid,int insert);
void print_process_tree(procinfo *pinfo,int level,double basetime);
void print_all_process_trees(double basetime,char *name);
void finalize_process_tree(void);
double find_first_process_time(char *name);

/* ktrace.c */
int ktrace_cmd_pipe[2]; // command pipe
void *ktrace_start(void *arg);

/* timer.c */
int timer_cmd_pipe[2]; // command pipe
void *timer_start(void *arg);

/* cpustatus.c */
void init_cpustatus(void);
void read_cpustatus(double time);
void print_cpustatus(double basetime);
