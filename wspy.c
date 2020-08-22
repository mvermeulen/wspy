/*
 * wspy.c - workload spy
 *
 * This file is the main driver for wspy program, responsible for parsing
 * arguments, starting processes and overall control.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/ptrace.h>
#include <sys/time.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <libgen.h>
#include "wspy.h"
#include "error.h"

pid_t child_pid = 0;
procinfo *child_procinfo = NULL;
pthread_t ftrace_thread;
pthread_t ptrace_thread;
pthread_t timer_thread;
pthread_t tracecmd_thread;
char *default_command[] = { "sleep", "30", NULL };
char original_dir[1024];
int num_procs = 0;

#define PATHLEN 1024
int child_pipe[2];
int setup_child_process(int argc,char **argv,char *const envp[]){
  pid_t child;
  char pathbuf[PATHLEN];
  char *path,*p;
  int len;
  if (pipe(child_pipe) == -1) fatal("pipe creation failed\n");
  switch(child = fork()){
  case 0:
    if (flag_set_uid) setuid(uid_value);
    if (flag_setcpumask) sched_setaffinity(0,sizeof(cpu_set_t),&cpumask);
    if (flag_require_ptrace||flag_require_ptrace2){
      debug("ptrace(PTRACE_TRACEME,0)\n");
      ptrace(PTRACE_TRACEME,0,NULL,NULL);
    } 
    close(child_pipe[1]); // close writing end
    read(child_pipe[0],pathbuf,PATHLEN); // wait until parent has written
    execve(argv[0],argv,envp);
    // if argv[0] fails, try looking in the path
    path = strdup(getenv("PATH"));
    len = strlen(argv[0]);
    p = strtok(path,":\n");
    if (p){
      do {
	strncpy(pathbuf,p,PATHLEN-len-2);
	strcat(pathbuf,"/");
	strcat(pathbuf,argv[0]);
	execve(pathbuf,argv,envp);
	p = strtok(NULL,":\n");
      } while (p);
      // exec failed
      return 1;
    }
    break;
  default:
    close(child_pipe[0]); // close reading end
    child_pid = child;
  }
  return 0;
}

// dump rusage information
void print_rusage(FILE *outfile){
  int status;
  struct rusage usage;
  status = getrusage(RUSAGE_CHILDREN,&usage);
  fprintf(outfile,"utime:    %ld.%6.6ld\n",usage.ru_utime.tv_sec,usage.ru_utime.tv_usec);
  fprintf(outfile,"stime:    %ld.%6.6ld\n",usage.ru_stime.tv_sec,usage.ru_stime.tv_usec);
  fprintf(outfile,"maxrss:   %luK\n",usage.ru_maxrss/1024);
  fprintf(outfile,"minflt:   %lu\n",usage.ru_minflt);
  fprintf(outfile,"majflt:   %lu\n",usage.ru_majflt);
  fprintf(outfile,"nswap:    %lu\n",usage.ru_nswap);
  fprintf(outfile,"inblock:  %lu\n",usage.ru_inblock);
  fprintf(outfile,"oublock:  %lu\n",usage.ru_oublock);
  fprintf(outfile,"msgsnd:   %lu\n",usage.ru_msgsnd);
  fprintf(outfile,"msgrcv:   %lu\n",usage.ru_msgrcv);
  fprintf(outfile,"nsignals: %lu\n",usage.ru_nsignals);
  fprintf(outfile,"nvcsw:    %lu\n",usage.ru_nvcsw);
  fprintf(outfile,"nivcsw:   %lu\n",usage.ru_nivcsw);
}

int main(int argc,char *const argv[],char *const envp[]){
  int status;
  int i;
  pid_t child;

  getcwd(original_dir,sizeof(original_dir));
  num_procs = get_nprocs();
  if (pthread_mutex_init(&event_lock,NULL) != 0)
    error("mutex lock creation failed\n");

  outfile = stdout;
  initialize_error_subsystem(argv[0],"-");

  read_config_file(NULL);

  if (parse_options(argc,argv)){
    fatal("usage: %s [options] <cmd><args>...\n"
	  "\t--cpustats, --no-cpustats      \tCPU usage tracing from /proc/stat\n"
	  "\t--diskstats,--no-diskstats     \tDisk usage tracing from /proc/diskstats\n"
	  "\t--memstats, --no-memstats      \tMemory usage tracing from /proc/meminfo\n"
	  "\t--netstats, --no-netstats      \tNetwork usage tracing from /proc/net/dev\n"
	  "\t--processtree, --no-processtree\tGenerate process tree\n"
	  "\t--processtree-engine <engine>  \tSet engine to either ftrace or ptrace\n"
	  "\t--perfcounters, --no-perfcounters\tCollect basic perf counters\n"
	  "\t--perfcounter-model            \tSet perfcounters to either core or process\n"
	  "\t--counterinfo                  \tRead directory of counter definitions\n"
	  "\t--show-counters,--no-show-counters\tShow available counters\n"
	  "\t--set-counters <cpulist>:<counterlist>\tSet list of counters to measure\n"
	  "\t--uid <uid>, -u <uid>          \trun as user\n"
	  "\t--set-cpumask <cpulist>        \tbind child to list of cores\n"
	  "\t--show-rusage                  \tdump output of get_rusage(2)\n"
	  "\t--interval <value>             \tset timer in ms (default %d)\n"
	  "\t--zip <archive-name>           \tcreate zip archive of results\n"
	  "\t--root <proc>, -r <proc>       \tset name for process tree root\n"
	  "\t--config <file>                \tread configuration file\n"
	  "\t--output <file>                \tredirect output to <file>\n"
	  "\t--error-output <file>          \tredirect errors and debug to <file>\n"
	  "\t--debug, -d                    \tinternal debugging flag\n",
	  argv[0],timer_interval);
  }

  if (command_line_argv == NULL){
    command_line_argv = default_command;
    command_line_argc = sizeof(default_command)/sizeof(default_command[0]) - 1;
    warning("missing command line\n");
    notice_noprogram("\tdefault:");
    for (i=0;i<command_line_argc;i++){
      notice_noprogram(" %s",command_line_argv[i]);
    }
    notice_noprogram("\n");
  }
  if (flag_version){
    notice("wspy version %2.1f\n",version/10.0);
  }
  
  // check to see if we've selected --perfcounters without an engine
  if (flag_require_perftree && (mask_processtree_engine_selected == 0)){
    // default to ptrace
    flag_require_ptrace = 1;
  }

  sort_counters();

  if (setup_child_process(command_line_argc,command_line_argv,envp)){
    fatal("unable to launch %s\n",command_line_argv[0]);
  }

  // let ^C go to children
  signal(SIGINT,SIG_IGN);

  if (flag_cpustats){ init_cpustats(); }
  if (flag_diskstats){ init_diskstats(); }
  if (flag_memstats){ init_memstats(); }
  if (flag_netstats){ init_netstats(); }
  if (flag_require_perftimer){ init_global_perf_counters(); }
  if ((flag_require_ptrace || flag_require_ptrace2) && flag_require_perftree){
    inventory_counters(0);
    init_process_counterinfo();
  }

  // These two are mutually exclusive since both use kernel tracing facility
  if (flag_require_ftrace){
    pthread_create(&ftrace_thread,NULL,ftrace_start,&child_pid);
  } else if (flag_require_tracecmd){
    pthread_create(&tracecmd_thread,NULL,tracecmd_start,(void *) envp);
  }

  if (flag_require_timer){
    double start_time = -5.0;
    // periodic timer
    pthread_create(&timer_thread,NULL,timer_start,&start_time);
  }

  // wait 5 seconds to allow subsystems to start
  sleep(5);
  child_procinfo = lookup_process_info(child_pid,1);
  if (flag_require_ptrace){
    start_process_perf_counters(child_pid,&child_procinfo->pci,1);
    child_procinfo->counters_started = 1;
  }
  
  // let the child proceed
  write(child_pipe[1],"start\n",6);
  
  if (flag_require_ptrace){
    ptrace_setup(child_pid);
    read_uptime(&child_procinfo->time_start);
  } else if (flag_require_ptrace2){
    ptrace2_setup(child_pid);    
  }

  notice("running until %s completes\n",command_line_argv[0]);
  if (flag_require_ptrace){
    ptrace_loop();
  } else if (flag_require_ptrace2){
    ptrace2_loop();    
  } else {
    // without ptrace, this process waits for the child to complete
    child = waitpid(child_pid,&status,0);
    if (WIFEXITED(status)){
      if (WEXITSTATUS(status) != 0){
	notice("child exited with status %d\n",WEXITSTATUS(status));
      }
    } else if (WIFSIGNALED(status)){
      notice("child signaled %d\n",WTERMSIG(status));
    }
  }

  if (flag_require_ftrace){
    write(ftrace_cmd_pipe[1],"quit\n",5);
    pthread_join(ftrace_thread,NULL);
  } else if (flag_require_tracecmd){
    if (tracecmd_pid != 0){
      kill(tracecmd_pid,SIGINT);
    }      
  }

  if (flag_require_timer){
    write(timer_cmd_pipe[1],"quit\n",5);
    pthread_join(timer_thread,NULL);
  }

  // run 5 seconds to let things stop
  sleep(5);

  if (flag_showcounters){
    print_counters(outfile);
  }
  if (flag_require_ptrace2){
    ptrace2_finish();
  }

  pthread_mutex_lock(&event_lock);
  finalize_process_tree();
  pthread_mutex_unlock(&event_lock);
  if (flag_require_ptrace && !flag_zip)
    print_all_process_trees(outfile,
			    flag_require_ptrace?child_procinfo->time_start:first_ftrace_time,
			    command_name);

  if (flag_cpustats && !flag_zip)
    print_cpustats();

  if (flag_memstats && !flag_zip)
    print_memstats();

  if (flag_diskstats && !flag_zip)
    print_diskstats();

  if (flag_require_perftimer && !flag_zip)
    print_global_perf_counters();

  if (flag_rusage && !flag_zip)
    print_rusage(stdout);

  if (flag_zip){
    FILE *fp;
    char buffer[1024],cmd[1024+128];
    char basezvalue[1024];
    char tmpdir[] = "/tmp/wspy.XXXXXX";
    char *newdir = mkdtemp(tmpdir);
    char *basez;
    int count = 0;
    status = chdir(newdir);
    if (flag_require_ptrace2){
      snprintf(cmd,sizeof(cmd),"cp %s/processtree2.csv .",original_dir);
      system(cmd);
    } else if (flag_proctree && (flag_require_ptrace || flag_require_ftrace)){
      fp = fopen("processtree.txt","w");
      if (fp){
	print_all_process_trees(fp,child_procinfo->time_start,command_name);
	fclose(fp);
      }
      fp = fopen("processtree.csv","w");
      if (fp){
	count = print_all_processes_csv(fp);
	fclose(fp);
      }
      fp = fopen("processtree.num","w");
      if (fp){
	fprintf(fp,"%d\n",count);
	fclose(fp);
      }
    }
    if (flag_cpustats) print_cpustats_files();
    if (flag_memstats) print_memstats_files();
    if (flag_diskstats) print_diskstats_files();
    if (flag_require_perftimer) print_global_perf_counter_files();
    if (flag_rusage){
      fp = fopen("resource.txt","w");
      if (fp){
	print_rusage(fp);
	fclose(fp);
      }
    }
    if (flag_require_tracecmd){
      snprintf(cmd,sizeof(cmd),"cp %s/processtree.dat .",original_dir);
      system(cmd);
    }

    strcpy(basezvalue,zip_archive_name);
    basez = basename(basezvalue);
    
    snprintf(cmd,sizeof(cmd),"zip -m %s *",basez);
    system(cmd);
    chdir(original_dir);
    snprintf(cmd,sizeof(cmd),"mv %s/%s* %s",tmpdir,basez,zip_archive_name);
    system(cmd);    
    rmdir(tmpdir);
  }
  return 0;
}

