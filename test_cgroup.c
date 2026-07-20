#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

#include "cgroup.c"

#define FAKE_PROC_BASE "/tmp/test_cgroup_proc"
#define FAKE_SYSFS_BASE "/tmp/test_cgroup_sysfs"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void mkdir_or_die(const char *path){
  if (mkdir(path,0755) != 0 && errno != EEXIST){
    perror(path);
    abort();
  }
}

static void mkdir_p(const char *path){
  char buf[900];
  char *p;

  snprintf(buf,sizeof(buf),"%s",path);
  for (p = buf + 1; *p; p++){
    if (*p == '/'){
      *p = '\0';
      mkdir_or_die(buf);
      *p = '/';
    }
  }
  mkdir_or_die(buf);
}

static void rmdir_recursive(const char *path){
  char cmd[600];
  snprintf(cmd,sizeof(cmd),"rm -rf '%s'",path);
  system(cmd);
}

static void reset_fixtures(void){
  rmdir_recursive(FAKE_PROC_BASE);
  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_PROC_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);
}

/* --- read_cgroup_v2_path_at --- */

static void test_read_cgroup_v2_path_simple(void){
  char path[512];
  char proc_file[600];

  printf("Testing read_cgroup_v2_path_at: simple v2 unified line...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,"0::/user.slice/user-1000.slice/session-2.scope\n");

  assert(read_cgroup_v2_path_at(proc_file,path,sizeof(path)) == 0);
  assert(!strcmp(path,"/user.slice/user-1000.slice/session-2.scope"));
  printf("PASS: read_cgroup_v2_path_at simple v2 unified line\n");
}

static void test_read_cgroup_v2_path_hybrid(void){
  char path[512];
  char proc_file[600];

  printf("Testing read_cgroup_v2_path_at: hybrid v1+v2 mount (only \"0::\" counts)...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,
    "12:pids:/user.slice\n"
    "1:name=systemd:/user.slice/user-1000.slice\n"
    "0::/user.slice/user-1000.slice/session-2.scope\n");

  assert(read_cgroup_v2_path_at(proc_file,path,sizeof(path)) == 0);
  assert(!strcmp(path,"/user.slice/user-1000.slice/session-2.scope"));
  printf("PASS: read_cgroup_v2_path_at hybrid mount\n");
}

static void test_read_cgroup_v2_path_v1_only(void){
  char path[512];
  char proc_file[600];

  printf("Testing read_cgroup_v2_path_at: pure cgroup v1 host (no \"0::\" line)...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,
    "12:pids:/user.slice\n"
    "1:name=systemd:/user.slice/user-1000.slice\n");

  assert(read_cgroup_v2_path_at(proc_file,path,sizeof(path)) == -1);
  printf("PASS: read_cgroup_v2_path_at pure v1 host degrades\n");
}

static void test_read_cgroup_v2_path_missing_file(void){
  char path[512];

  printf("Testing read_cgroup_v2_path_at: missing /proc/self/cgroup...\n");
  reset_fixtures();
  assert(read_cgroup_v2_path_at(FAKE_PROC_BASE "/does-not-exist",path,sizeof(path)) == -1);
  printf("PASS: read_cgroup_v2_path_at missing file\n");
}

/* --- parse_cpu_max_line --- */

static void test_parse_cpu_max_line(void){
  long long quota,period;

  printf("Testing parse_cpu_max_line...\n");
  assert(parse_cpu_max_line("max 100000",&quota,&period) == 0);
  assert(quota == -1 && period == 100000);

  assert(parse_cpu_max_line("200000 100000",&quota,&period) == 0);
  assert(quota == 200000 && period == 100000);

  assert(parse_cpu_max_line("garbage",&quota,&period) == -1);
  printf("PASS: parse_cpu_max_line\n");
}

/* --- parse_max_or_number_at --- */

static void test_parse_max_or_number_at(void){
  char path[600];

  printf("Testing parse_max_or_number_at...\n");
  reset_fixtures();

  snprintf(path,sizeof(path),"%s/memory.max.unlimited",FAKE_SYSFS_BASE);
  write_file(path,"max\n");
  {
    long long v;
    assert(parse_max_or_number_at(path,&v) == 0);
    assert(v == -1);
  }

  snprintf(path,sizeof(path),"%s/memory.max.limited",FAKE_SYSFS_BASE);
  write_file(path,"1073741824\n");
  {
    long long v;
    assert(parse_max_or_number_at(path,&v) == 0);
    assert(v == 1073741824LL);
  }

  {
    long long v;
    assert(parse_max_or_number_at(FAKE_SYSFS_BASE "/does-not-exist",&v) == -1);
  }
  printf("PASS: parse_max_or_number_at\n");
}

/* --- parse_cpu_stat_at --- */

static void test_parse_cpu_stat_at_full(void){
  char path[600];
  struct cgroup_throttle t;

  printf("Testing parse_cpu_stat_at: full cpu.stat with throttling fields...\n");
  reset_fixtures();
  snprintf(path,sizeof(path),"%s/cpu.stat.full",FAKE_SYSFS_BASE);
  write_file(path,
    "usage_usec 3925853195\n"
    "user_usec 3264188731\n"
    "system_usec 661664463\n"
    "nr_periods 42\n"
    "nr_throttled 7\n"
    "throttled_usec 123456\n");

  assert(parse_cpu_stat_at(path,&t) == 0);
  assert(t.available);
  assert(t.nr_periods == 42);
  assert(t.nr_throttled == 7);
  assert(t.throttled_usec == 123456);
  printf("PASS: parse_cpu_stat_at full\n");
}

static void test_parse_cpu_stat_at_no_cpu_controller(void){
  char path[600];
  struct cgroup_throttle t;

  printf("Testing parse_cpu_stat_at: leaf cgroup with cpu controller not enabled "
         "(real, confirmed-live case -- no nr_periods/nr_throttled/throttled_usec keys)...\n");
  reset_fixtures();
  snprintf(path,sizeof(path),"%s/cpu.stat.nocpu",FAKE_SYSFS_BASE);
  write_file(path,
    "usage_usec 3925853195\n"
    "user_usec 3264188731\n"
    "system_usec 661664463\n"
    "nice_usec 0\n"
    "core_sched.force_idle_usec 0\n");

  assert(parse_cpu_stat_at(path,&t) == -1);
  assert(!t.available);
  printf("PASS: parse_cpu_stat_at degrades when cpu controller not enabled\n");
}

static void test_parse_cpu_stat_at_missing_file(void){
  struct cgroup_throttle t;

  printf("Testing parse_cpu_stat_at: missing cpu.stat file...\n");
  reset_fixtures();
  assert(parse_cpu_stat_at(FAKE_SYSFS_BASE "/does-not-exist",&t) == -1);
  assert(!t.available);
  printf("PASS: parse_cpu_stat_at missing file\n");
}

/* --- collect_identity_and_limits_at / read_throttle_at (integration) --- */

static void test_collect_identity_and_limits_full(void){
  char proc_file[600];
  char cg_dir[960];
  struct cgroup_info info;

  printf("Testing collect_identity_and_limits_at: fully-populated cgroup...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,"0::/test.slice/test.scope\n");

  snprintf(cg_dir,sizeof(cg_dir),"%s/test.slice/test.scope",FAKE_SYSFS_BASE);
  mkdir_p(cg_dir);
  {
    char path[1024];
    snprintf(path,sizeof(path),"%s/cpu.max",cg_dir); write_file(path,"200000 100000\n");
    snprintf(path,sizeof(path),"%s/cpu.weight",cg_dir); write_file(path,"100\n");
    snprintf(path,sizeof(path),"%s/memory.max",cg_dir); write_file(path,"max\n");
    snprintf(path,sizeof(path),"%s/memory.high",cg_dir); write_file(path,"1073741824\n");
  }

  collect_identity_and_limits_at(proc_file,FAKE_SYSFS_BASE,&info);
  assert(info.available);
  assert(!strcmp(info.path,"/test.slice/test.scope"));
  assert(info.cpu_max_available);
  assert(info.cpu_quota_us == 200000);
  assert(info.cpu_period_us == 100000);
  assert(info.cpu_weight_available);
  assert(info.cpu_weight == 100);
  assert(info.memory_max_available);
  assert(info.memory_max_bytes == -1);
  assert(info.memory_high_available);
  assert(info.memory_high_bytes == 1073741824LL);
  printf("PASS: collect_identity_and_limits_at fully-populated\n");
}

static void test_collect_identity_and_limits_no_cpu_controller(void){
  /* Regression fixture for the real host behavior confirmed live during
   * development: a leaf cgroup (desktop terminal-emulator scope) had
   * memory.max/memory.high but no cpu.max/cpu.weight at all, since the cpu
   * controller wasn't enabled on it. Each field must degrade
   * independently -- available must still be 1 (a path was found). */
  char proc_file[600];
  char cg_dir[960];
  struct cgroup_info info;

  printf("Testing collect_identity_and_limits_at: cpu controller not enabled on this cgroup...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,"0::/user.slice/vte-spawn.scope\n");

  snprintf(cg_dir,sizeof(cg_dir),"%s/user.slice/vte-spawn.scope",FAKE_SYSFS_BASE);
  mkdir_p(cg_dir);
  {
    char path[1024];
    snprintf(path,sizeof(path),"%s/memory.max",cg_dir); write_file(path,"max\n");
    snprintf(path,sizeof(path),"%s/memory.high",cg_dir); write_file(path,"max\n");
    /* deliberately no cpu.max/cpu.weight/cpu.stat files at all */
  }

  collect_identity_and_limits_at(proc_file,FAKE_SYSFS_BASE,&info);
  assert(info.available);
  assert(!strcmp(info.path,"/user.slice/vte-spawn.scope"));
  assert(!info.cpu_max_available);
  assert(!info.cpu_weight_available);
  assert(info.memory_max_available && info.memory_max_bytes == -1);
  assert(info.memory_high_available && info.memory_high_bytes == -1);
  printf("PASS: collect_identity_and_limits_at degrades cpu.max/cpu.weight independently\n");
}

static void test_collect_identity_and_limits_v1_only_host(void){
  char proc_file[600];
  struct cgroup_info info;

  printf("Testing collect_identity_and_limits_at: pure cgroup v1 host...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,"1:name=systemd:/user.slice\n");

  collect_identity_and_limits_at(proc_file,FAKE_SYSFS_BASE,&info);
  assert(!info.available);
  assert(info.path[0] == '\0');
  assert(!info.cpu_max_available);
  assert(!info.memory_max_available);
  printf("PASS: collect_identity_and_limits_at pure v1 host\n");
}

static void test_read_throttle_at_full(void){
  char proc_file[600];
  char cg_dir[960];
  struct cgroup_info info;
  struct cgroup_throttle t;

  printf("Testing read_throttle_at: cpu.stat with throttling fields present...\n");
  reset_fixtures();
  snprintf(proc_file,sizeof(proc_file),"%s/cgroup",FAKE_PROC_BASE);
  write_file(proc_file,"0::/test.slice/test.scope\n");
  snprintf(cg_dir,sizeof(cg_dir),"%s/test.slice/test.scope",FAKE_SYSFS_BASE);
  mkdir_p(cg_dir);
  {
    char path[1024];
    snprintf(path,sizeof(path),"%s/cpu.stat",cg_dir);
    write_file(path,"usage_usec 100\nnr_periods 5\nnr_throttled 1\nthrottled_usec 999\n");
  }

  collect_identity_and_limits_at(proc_file,FAKE_SYSFS_BASE,&info);
  read_throttle_at(FAKE_SYSFS_BASE,&info,&t);
  assert(t.available);
  assert(t.nr_periods == 5 && t.nr_throttled == 1 && t.throttled_usec == 999);
  printf("PASS: read_throttle_at full\n");
}

static void test_read_throttle_at_no_path_found(void){
  struct cgroup_info info;
  struct cgroup_throttle t;

  printf("Testing read_throttle_at: no cgroup path found (skips filesystem access)...\n");
  memset(&info,0,sizeof(info));
  info.available = 0;

  read_throttle_at(FAKE_SYSFS_BASE,&info,&t);
  assert(!t.available);
  printf("PASS: read_throttle_at no path found\n");
}

/* --- cgroup_throttle_delta (pure) --- */

static void test_cgroup_throttle_delta_normal(void){
  struct cgroup_throttle start,end,delta;

  printf("Testing cgroup_throttle_delta: normal case...\n");
  memset(&start,0,sizeof(start));
  memset(&end,0,sizeof(end));
  start.available = 1; start.nr_periods = 10; start.nr_throttled = 2; start.throttled_usec = 500;
  end.available = 1;   end.nr_periods = 15;   end.nr_throttled = 5;   end.throttled_usec = 2000;

  cgroup_throttle_delta(&start,&end,&delta);
  assert(delta.available);
  assert(delta.nr_periods == 5);
  assert(delta.nr_throttled == 3);
  assert(delta.throttled_usec == 1500);
  printf("PASS: cgroup_throttle_delta normal\n");
}

static void test_cgroup_throttle_delta_unavailable(void){
  struct cgroup_throttle start,end,delta;

  printf("Testing cgroup_throttle_delta: either snapshot unavailable...\n");
  memset(&start,0,sizeof(start));
  memset(&end,0,sizeof(end));
  start.available = 0;
  end.available = 1;

  cgroup_throttle_delta(&start,&end,&delta);
  assert(!delta.available);
  assert(delta.nr_periods == 0 && delta.nr_throttled == 0 && delta.throttled_usec == 0);
  printf("PASS: cgroup_throttle_delta unavailable\n");
}

/* --- public API wrappers (real host, best-effort sanity only) --- */

static void test_public_api_does_not_crash_on_real_host(void){
  struct cgroup_info info;
  struct cgroup_throttle t1,t2,delta;

  printf("Testing cgroup_collect_identity_and_limits/cgroup_read_throttle on the real host...\n");
  cgroup_collect_identity_and_limits(&info);
  cgroup_read_throttle(&info,&t1);
  cgroup_read_throttle(&info,&t2);
  cgroup_throttle_delta(&t1,&t2,&delta);
  /* No assertions on values (host-dependent) -- this just confirms the
   * real /proc/self/cgroup + /sys/fs/cgroup path doesn't crash and
   * produces internally consistent available flags. */
  if (!info.available){
    assert(!t1.available && !t2.available && !delta.available);
  }
  printf("PASS: public API real-host sanity check\n");
}

int main(void){
  test_read_cgroup_v2_path_simple();
  test_read_cgroup_v2_path_hybrid();
  test_read_cgroup_v2_path_v1_only();
  test_read_cgroup_v2_path_missing_file();
  test_parse_cpu_max_line();
  test_parse_max_or_number_at();
  test_parse_cpu_stat_at_full();
  test_parse_cpu_stat_at_no_cpu_controller();
  test_parse_cpu_stat_at_missing_file();
  test_collect_identity_and_limits_full();
  test_collect_identity_and_limits_no_cpu_controller();
  test_collect_identity_and_limits_v1_only_host();
  test_read_throttle_at_full();
  test_read_throttle_at_no_path_found();
  test_cgroup_throttle_delta_normal();
  test_cgroup_throttle_delta_unavailable();
  test_public_api_does_not_crash_on_real_host();

  rmdir_recursive(FAKE_PROC_BASE);
  rmdir_recursive(FAKE_SYSFS_BASE);

  printf("\nAll test_cgroup tests passed.\n");
  return 0;
}
