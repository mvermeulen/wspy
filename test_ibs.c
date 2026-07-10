#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

/* ibs.c's report printer writes to outfile, normally defined in wspy.c --
 * this test doesn't include wspy.c, so provide it directly. */
FILE *outfile;

#include "ibs.c"

#define FAKE_SYSFS_BASE "/tmp/test_ibs_sysfs"

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

/* Builds <FAKE_SYSFS_BASE>/<pmu_name>/{type,format/*,caps/*}, mirroring the
 * real /sys/bus/event_source/devices/{ibs_fetch,ibs_op} layout. */
static void make_fake_pmu(const char *pmu_name,int type,
                          const char *const format_names[],const char *const format_values[],int nformat,
                          const char *const cap_names[],const int cap_values[],int ncaps){
  char path[512];
  int i;

  snprintf(path,sizeof(path),"%s",FAKE_SYSFS_BASE);
  mkdir_or_die(path);
  snprintf(path,sizeof(path),"%s/%s",FAKE_SYSFS_BASE,pmu_name);
  mkdir_or_die(path);

  snprintf(path,sizeof(path),"%s/%s/type",FAKE_SYSFS_BASE,pmu_name);
  {
    char buf[16];
    snprintf(buf,sizeof(buf),"%d\n",type);
    write_file(path,buf);
  }

  snprintf(path,sizeof(path),"%s/%s/format",FAKE_SYSFS_BASE,pmu_name);
  mkdir_or_die(path);
  for (i = 0; i < nformat; i++){
    char file_path[600];
    char content[64];
    snprintf(file_path,sizeof(file_path),"%s/%s",path,format_names[i]);
    snprintf(content,sizeof(content),"%s\n",format_values[i]);
    write_file(file_path,content);
  }

  snprintf(path,sizeof(path),"%s/%s/caps",FAKE_SYSFS_BASE,pmu_name);
  mkdir_or_die(path);
  for (i = 0; i < ncaps; i++){
    char file_path[600];
    char content[16];
    snprintf(file_path,sizeof(file_path),"%s/%s",path,cap_names[i]);
    snprintf(content,sizeof(content),"%d\n",cap_values[i]);
    write_file(file_path,content);
  }
}

static void rmdir_recursive(const char *path){
  char cmd[600];
  snprintf(cmd,sizeof(cmd),"rm -rf '%s'",path);
  system(cmd);
}

static void test_ibs_not_present(void){
  struct ibs_capabilities ibs;

  printf("Testing ibs_probe_at: no sysfs entries at all...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);

  ibs = ibs_probe_at(FAKE_SYSFS_BASE);
  assert(ibs.supported == 0);
  assert(ibs.fetch.present == 0);
  assert(ibs.fetch.type == -1);
  assert(ibs.op.present == 0);
  assert(ibs.op.type == -1);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_probe_at not present\n");
}

static void test_ibs_fully_present(void){
  struct ibs_capabilities ibs;
  const struct ibs_format_field *f;
  const struct ibs_cap *c;

  printf("Testing ibs_probe_at: both PMUs fully present...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);

  {
    const char *fmt_names[] = { "l3missonly","rand_en","swfilt" };
    const char *fmt_values[] = { "config:59","config:57","config2:0" };
    const char *cap_names[] = { "zen4_ibs_extensions" };
    const int cap_values[] = { 1 };
    make_fake_pmu("ibs_fetch",10,fmt_names,fmt_values,3,cap_names,cap_values,1);
  }
  {
    const char *fmt_names[] = { "cnt_ctl","l3missonly","ldlat","swfilt" };
    const char *fmt_values[] = { "config:19","config:16","config1:0-11","config2:0" };
    const char *cap_names[] = { "dtlb_pgsize","ldlat","zen4_ibs_extensions" };
    const int cap_values[] = { 1,1,1 };
    make_fake_pmu("ibs_op",11,fmt_names,fmt_values,4,cap_names,cap_values,3);
  }

  ibs = ibs_probe_at(FAKE_SYSFS_BASE);
  assert(ibs.supported == 1);

  assert(ibs.fetch.present == 1);
  assert(ibs.fetch.type == 10);
  assert(ibs.fetch.format_count == 3);
  assert(ibs.fetch.caps_count == 1);
  f = ibs_pmu_format(&ibs.fetch,"l3missonly");
  assert(f != NULL && !strcmp(f->location,"config:59"));
  assert(ibs_pmu_format(&ibs.fetch,"does_not_exist") == NULL);
  c = ibs_pmu_cap(&ibs.fetch,"zen4_ibs_extensions");
  assert(c != NULL && c->enabled == 1);

  assert(ibs.op.present == 1);
  assert(ibs.op.type == 11);
  assert(ibs.op.format_count == 4);
  assert(ibs.op.caps_count == 3);
  f = ibs_pmu_format(&ibs.op,"ldlat");
  assert(f != NULL && !strcmp(f->location,"config1:0-11"));
  c = ibs_pmu_cap(&ibs.op,"dtlb_pgsize");
  assert(c != NULL && c->enabled == 1);

  /* format/caps must come back sorted by name, regardless of readdir order */
  assert(strcmp(ibs.fetch.format[0].name,ibs.fetch.format[1].name) < 0);
  assert(strcmp(ibs.fetch.format[1].name,ibs.fetch.format[2].name) < 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_probe_at fully present\n");
}

static void test_ibs_partially_present(void){
  struct ibs_capabilities ibs;

  printf("Testing ibs_probe_at: only ibs_fetch present (not \"supported\")...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  {
    const char *fmt_names[] = { "rand_en" };
    const char *fmt_values[] = { "config:57" };
    make_fake_pmu("ibs_fetch",10,fmt_names,fmt_values,1,NULL,NULL,0);
  }

  ibs = ibs_probe_at(FAKE_SYSFS_BASE);
  assert(ibs.fetch.present == 1);
  assert(ibs.op.present == 0);
  assert(ibs.supported == 0); /* both PMUs are required */

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_probe_at partially present\n");
}

/* Sets up the same fake ibs_fetch/ibs_op fixture test_ibs_fully_present()
 * uses -- ibs_op exposes cnt_ctl/l3missonly/ldlat/swfilt but no "maxcnt" or
 * "fetchlat", ibs_fetch exposes l3missonly/rand_en/swfilt but no
 * "fetchlat" either -- so the build/apply tests below exercise both the
 * "field present, gets applied" and "field absent, requested-not-applied"
 * paths against one realistic-looking sysfs snapshot. */
static void make_fully_present_fixture(void){
  rmdir_recursive(FAKE_SYSFS_BASE);
  {
    const char *fmt_names[] = { "l3missonly","rand_en","swfilt" };
    const char *fmt_values[] = { "config:59","config:57","config2:0" };
    const char *cap_names[] = { "zen4_ibs_extensions" };
    const int cap_values[] = { 1 };
    make_fake_pmu("ibs_fetch",10,fmt_names,fmt_values,3,cap_names,cap_values,1);
  }
  {
    const char *fmt_names[] = { "cnt_ctl","l3missonly","ldlat","swfilt" };
    const char *fmt_values[] = { "config:19","config:16","config1:0-11","config2:0" };
    const char *cap_names[] = { "dtlb_pgsize","ldlat","zen4_ibs_extensions" };
    const int cap_values[] = { 1,1,1 };
    make_fake_pmu("ibs_op",11,fmt_names,fmt_values,4,cap_names,cap_values,3);
  }
}

static void test_ibs_build_event_basic_profile(void){
  struct ibs_capabilities ibs;
  struct ibs_event fetch_ev,op_ev;

  printf("Testing ibs_build_fetch_event/ibs_build_op_event: basic profile requests no filters...\n");

  make_fully_present_fixture();
  ibs = ibs_probe_at(FAKE_SYSFS_BASE);

  fetch_ev = ibs_build_fetch_event(&ibs.fetch,IBS_PROFILE_BASIC,NULL);
  assert(fetch_ev.valid == 1);
  assert(fetch_ev.type == 10);
  assert(fetch_ev.l3missonly_requested == 0);
  assert(fetch_ev.fetchlat_requested == 0);
  assert(fetch_ev.config == 0); /* no maxcnt field in this fixture, nothing else requested */

  op_ev = ibs_build_op_event(&ibs.op,IBS_PROFILE_BASIC,NULL);
  assert(op_ev.valid == 1);
  assert(op_ev.type == 11);
  assert(op_ev.l3missonly_requested == 0);
  assert(op_ev.ldlat_requested == 0);
  assert(op_ev.config == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_build_*_event basic profile\n");
}

static void test_ibs_build_event_memory_deep_profile(void){
  struct ibs_capabilities ibs;
  struct ibs_event fetch_ev,op_ev,op_unf_ev;
  struct ibs_profile_params params = {0,250,0}; /* custom ldlat threshold, default maxcnt/fetchlat */

  printf("Testing ibs_build_fetch_event/ibs_build_op_event: memory-deep profile applies supported filters, flags unsupported ones...\n");

  make_fully_present_fixture();
  ibs = ibs_probe_at(FAKE_SYSFS_BASE);

  /* ibs_op: l3missonly is present (config:16) and gets applied; ldlat is
   * present (config1:0-11) and gets applied with the overridden threshold. */
  op_ev = ibs_build_op_event(&ibs.op,IBS_PROFILE_MEMORY_DEEP,&params);
  assert(op_ev.valid == 1);
  assert(op_ev.l3missonly_requested == 1 && op_ev.l3missonly_applied == 1);
  assert((op_ev.config & (1UL<<16)) != 0);
  assert(op_ev.ldlat_requested == 1 && op_ev.ldlat_applied == 1);
  assert(op_ev.ldlat_threshold == 250);
  assert(op_ev.config1 == 250);

  /* ibs_fetch: l3missonly is present (config:59) and gets applied; fetchlat
   * is requested by the profile but this fixture has no "fetchlat" format
   * field, so it must come back requested-but-not-applied rather than
   * silently corrupting some other bit range. */
  fetch_ev = ibs_build_fetch_event(&ibs.fetch,IBS_PROFILE_MEMORY_DEEP,&params);
  assert(fetch_ev.valid == 1);
  assert(fetch_ev.l3missonly_requested == 1 && fetch_ev.l3missonly_applied == 1);
  assert((fetch_ev.config & (1UL<<59)) != 0);
  assert(fetch_ev.fetchlat_requested == 1 && fetch_ev.fetchlat_applied == 0);

  /* the unfiltered baseline event never requests l3missonly/ldlat, so the
   * filtered/unfiltered pair differs only by those bits -- that difference
   * is exactly what the accepted-vs-filtered ratio in topdown.c's
   * print_ibs() is meant to surface. */
  op_unf_ev = ibs_build_op_unfiltered_event(&ibs.op,&params);
  assert(op_unf_ev.valid == 1);
  assert(op_unf_ev.l3missonly_requested == 0);
  assert(op_unf_ev.config == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_build_*_event memory-deep profile\n");
}

static void test_ibs_build_event_absent_pmu(void){
  struct ibs_capabilities ibs;
  struct ibs_event ev;

  printf("Testing ibs_build_*_event: absent PMU yields valid==0, not a crash...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);
  ibs = ibs_probe_at(FAKE_SYSFS_BASE);

  ev = ibs_build_fetch_event(&ibs.fetch,IBS_PROFILE_BASIC,NULL);
  assert(ev.valid == 0);
  ev = ibs_build_op_event(&ibs.op,IBS_PROFILE_MEMORY_DEEP,NULL);
  assert(ev.valid == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: ibs_build_*_event absent PMU\n");
}

static void test_ibs_counter_group(void){
  struct counter_group *cgroup;

  /* ibs_counter_group() always probes the real /sys/bus/event_source path
   * (not the fake-sysfs ibs_probe_at() the other tests use), so the only
   * behavior this test can assert independent of the host it runs on is
   * IBS_PROFILE_NONE's short-circuit -- everything field/config-specific is
   * already covered above via ibs_build_*_event() against the fixture. */
  printf("Testing ibs_counter_group: IBS_PROFILE_NONE always yields NULL...\n");

  cgroup = ibs_counter_group("ibs",IBS_PROFILE_NONE,NULL);
  assert(cgroup == NULL);

  printf("PASS: ibs_counter_group (NONE profile yields NULL)\n");
}

int main(void){
  test_ibs_not_present();
  test_ibs_fully_present();
  test_ibs_partially_present();
  test_ibs_build_event_basic_profile();
  test_ibs_build_event_memory_deep_profile();
  test_ibs_build_event_absent_pmu();
  test_ibs_counter_group();

  printf("\nAll test_ibs tests passed.\n");
  return 0;
}
