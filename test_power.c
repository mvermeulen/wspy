#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

/* power.c's report printer writes to outfile, normally defined in wspy.c --
 * this test doesn't include wspy.c, so provide it directly. */
FILE *outfile;

#include "power.c"

#define FAKE_SYSFS_BASE "/tmp/test_power_sysfs"

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

static void rmdir_recursive(const char *path){
  char cmd[600];
  snprintf(cmd,sizeof(cmd),"rm -rf '%s'",path);
  system(cmd);
}

/* Builds <FAKE_SYSFS_BASE>/<pmu_name>/{type,format/event,events/<event_name>[,.scale][,.unit]},
 * mirroring the real /sys/bus/event_source/devices/{power,power_core} layout.
 * Any of format_location/event_text/scale_text/unit_text may be NULL to
 * omit that file entirely (exercising the "field absent" degrade paths). */
static void make_fake_pmu(const char *pmu_name,int type,const char *format_location,
                           const char *event_name,const char *event_text,
                           const char *scale_text,const char *unit_text){
  char path[512];

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
  if (format_location){
    char file_path[600],content[64];
    snprintf(file_path,sizeof(file_path),"%s/event",path);
    snprintf(content,sizeof(content),"%s\n",format_location);
    write_file(file_path,content);
  }

  snprintf(path,sizeof(path),"%s/%s/events",FAKE_SYSFS_BASE,pmu_name);
  mkdir_or_die(path);
  if (event_text){
    char file_path[600],content[64];
    snprintf(file_path,sizeof(file_path),"%s/%s",path,event_name);
    snprintf(content,sizeof(content),"%s\n",event_text);
    write_file(file_path,content);
  }
  if (scale_text){
    char file_path[600],content[64];
    snprintf(file_path,sizeof(file_path),"%s/%s.scale",path,event_name);
    snprintf(content,sizeof(content),"%s\n",scale_text);
    write_file(file_path,content);
  }
  if (unit_text){
    char file_path[600],content[64];
    snprintf(file_path,sizeof(file_path),"%s/%s.unit",path,event_name);
    snprintf(content,sizeof(content),"%s\n",unit_text);
    write_file(file_path,content);
  }
}

static void test_power_not_present(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: no sysfs entries at all...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.supported == 0);
  assert(power.pkg.present == 0);
  assert(power.pkg.type == -1);
  assert(power.pkg.event_present == 0);
  assert(power.core.present == 0);
  assert(power.core.type == -1);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at not present\n");
}

static void test_power_fully_present(void){
  struct power_capabilities power;
  const struct power_format_field *f;

  printf("Testing power_probe_at: both power and power_core fully present...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",
                "2.3283064365386962890625e-10","Joules");
  make_fake_pmu("power_core",15,"config:0-7","energy-core","event=0x01",
                "2.3283064365386962890625e-10","Joules");

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.supported == 1);

  assert(power.pkg.present == 1);
  assert(power.pkg.type == 14);
  assert(power.pkg.event_present == 1);
  assert(power.pkg.event == 0x02);
  assert(power.pkg.scale > 2.3e-10 && power.pkg.scale < 2.4e-10);
  assert(!strcmp(power.pkg.unit,"Joules"));
  f = find_format(&power.pkg,"event");
  assert(f != NULL && !strcmp(f->location,"config:0-7"));
  assert(find_format(&power.pkg,"does_not_exist") == NULL);

  assert(power.core.present == 1);
  assert(power.core.type == 15);
  assert(power.core.event_present == 1);
  assert(power.core.event == 0x01);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at fully present\n");
}

static void test_power_pkg_only(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: power present, power_core absent -- still supported (pkg-only v1 scope)...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",
                "2.3283064365386962890625e-10","Joules");

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.pkg.present == 1);
  assert(power.core.present == 0);
  assert(power.supported == 1); /* only pkg is required */

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at pkg-only\n");
}

static void test_power_missing_event_file(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: power/ present but no events/energy-pkg file -- not supported...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7",NULL,NULL,NULL,NULL);

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.pkg.present == 1);
  assert(power.pkg.event_present == 0);
  assert(power.supported == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at missing event file\n");
}

static void test_power_scale_defaults_to_one(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: events/<name>.scale absent -- scale defaults to 1.0...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",NULL,NULL);

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.pkg.event_present == 1);
  assert(power.pkg.scale == 1.0);
  assert(power.pkg.unit[0] == '\0');

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at scale defaults to 1.0\n");
}

static void test_power_build_pkg_event(void){
  struct power_capabilities power;
  struct power_event ev;

  printf("Testing power_build_pkg_event: event value placed at the format field's bit location...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",
                "2.3283064365386962890625e-10","Joules");
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  ev = power_build_pkg_event(&power.pkg);
  assert(ev.valid == 1);
  assert(ev.type == 14);
  assert(ev.config == 0x02); /* lo==0, so no shift */

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_build_pkg_event bit placement\n");
}

static void test_power_build_pkg_event_shifted(void){
  struct power_capabilities power;
  struct power_event ev;

  printf("Testing power_build_pkg_event: nonzero-lo format location shifts the event value...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:8-15","energy-pkg","event=0x03",NULL,NULL);
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  ev = power_build_pkg_event(&power.pkg);
  assert(ev.valid == 1);
  assert(ev.config == (0x03UL << 8));

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_build_pkg_event shifted bit placement\n");
}

static void test_power_build_pkg_event_absent(void){
  struct power_capabilities power;
  struct power_event ev;

  printf("Testing power_build_pkg_event: absent PMU yields valid==0, not a crash...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  ev = power_build_pkg_event(&power.pkg);
  assert(ev.valid == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_build_pkg_event absent PMU\n");
}

static void test_power_counter_group_structural(void){
  struct counter_group *cgroup;

  /* power_counter_group() always probes the real /sys/bus/event_source path
   * (not the fake-sysfs power_probe_at() the other tests use, mirroring
   * ibs_counter_group()'s own test in test_ibs.c), so this can only assert
   * structural invariants that hold either way -- everything field/config-
   * specific is already covered above via power_build_pkg_event() against
   * fake fixtures. */
  printf("Testing power_counter_group: structural invariants regardless of host support...\n");

  cgroup = power_counter_group("power");
  if (cgroup == NULL){
    printf("  (power/energy-pkg not supported on this host -- NULL is correct)\n");
  } else {
    assert(cgroup->ncounters == 1);
    assert(!strcmp(cgroup->label,"power"));
    assert(cgroup->mask == COUNTER_POWER);
    assert(cgroup->type_id == PERF_TYPE_RAW);
    assert(!strcmp(cgroup->cinfo[0].label,"pkg_joules"));
    assert(cgroup->cinfo[0].is_group_leader == 1);
    assert(cgroup->cinfo[0].scale > 0.0);
    // Real perf-backed path only (fallback hwmon reads bypass perf_event_open()
    // entirely, device_type==9999 -- see power_counter_group()'s own comment):
    // regression test for INVESTIGATION.md's "RAPL/energy-pkg opened with the
    // wrong scope" item -- setup_counters() must route this through pid=-1,
    // not the generic per-process branch, since RAPL's "power" PMU driver
    // rejects a process-scoped open outright.
    if (cgroup->cinfo[0].device_type != 9999){
      assert(cgroup->cinfo[0].requires_system_wide == 1);
    }
  }

  printf("PASS: power_counter_group structural invariants\n");
}

/* INVESTIGATION.md's 4.2 Tier 1 "Per-core energy support" item:
 * power_core's own cpumask (comma/range list) tells us which logical CPUs
 * a real per-core event can actually be opened on. */
static void test_power_core_cpumask_parsing(void){
  struct power_capabilities power;
  char path[512];

  printf("Testing power_probe_at: power_core/cpumask parsed into core_cpus[]...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",
                "2.3283064365386962890625e-10","Joules");
  make_fake_pmu("power_core",15,"config:0-7","energy-core","event=0x01",
                "2.3283064365386962890625e-10","Joules");
  snprintf(path,sizeof(path),"%s/power_core/cpumask",FAKE_SYSFS_BASE);
  write_file(path,"0,2,4-6\n"); /* mixes plain commas and a range, like mark_cpus_for_pmu()'s grammar */

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.ncore_cpus == 5); /* 0,2,4,5,6 */
  assert(power_core_cpu_is_representative(&power,0));
  assert(power_core_cpu_is_representative(&power,2));
  assert(power_core_cpu_is_representative(&power,4));
  assert(power_core_cpu_is_representative(&power,5));
  assert(power_core_cpu_is_representative(&power,6));
  assert(!power_core_cpu_is_representative(&power,1));
  assert(!power_core_cpu_is_representative(&power,3));
  assert(!power_core_cpu_is_representative(&power,7));

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at power_core/cpumask parsing\n");
}

static void test_power_core_cpumask_absent_when_power_core_absent(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: no power_core -- ncore_cpus stays 0, no cpumask read attempted...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power",14,"config:0-7","energy-pkg","event=0x02",
                "2.3283064365386962890625e-10","Joules");

  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");
  assert(power.core.present == 0);
  assert(power.ncore_cpus == 0);
  assert(!power_core_cpu_is_representative(&power,0));

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_probe_at power_core absent -- no cpumask\n");
}

static void test_power_build_core_event(void){
  struct power_capabilities power;
  struct power_event ev;

  printf("Testing power_build_core_event: event value placed at the format field's bit location...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power_core",15,"config:0-7","energy-core","event=0x01",
                "2.3283064365386962890625e-10","Joules");
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  ev = power_build_core_event(&power.core);
  assert(ev.valid == 1);
  assert(ev.type == 15);
  assert(ev.config == 0x01);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_build_core_event bit placement\n");
}

static void test_power_core_counter_group_representative(void){
  struct power_capabilities power;
  struct counter_group *cgroup;
  char path[512];

  printf("Testing power_core_counter_group: representative CPU gets a real event...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power_core",15,"config:0-7","energy-core","event=0x01",
                "2.3283064365386962890625e-10","Joules");
  snprintf(path,sizeof(path),"%s/power_core/cpumask",FAKE_SYSFS_BASE);
  write_file(path,"0,2\n");
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  cgroup = power_core_counter_group("power-core",&power,2);
  assert(cgroup != NULL);
  assert(cgroup->ncounters == 1);
  assert(!strcmp(cgroup->label,"power-core"));
  assert(cgroup->mask == COUNTER_POWER_CORE);
  assert(cgroup->target_cpu == 2);
  assert(!strcmp(cgroup->cinfo[0].label,"core_joules"));
  assert(cgroup->cinfo[0].is_group_leader == 1);
  assert(cgroup->cinfo[0].device_type == 15); /* real dynamic type, not the skip sentinel */
  assert(cgroup->cinfo[0].config == 0x01);
  assert(cgroup->cinfo[0].scale > 2.3e-10 && cgroup->cinfo[0].scale < 2.4e-10);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_core_counter_group representative CPU\n");
}

static void test_power_core_counter_group_not_representative(void){
  struct power_capabilities power;
  struct counter_group *cgroup;
  char path[512];

  printf("Testing power_core_counter_group: non-representative CPU is skip-marked, not attempted...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_pmu("power_core",15,"config:0-7","energy-core","event=0x01",
                "2.3283064365386962890625e-10","Joules");
  snprintf(path,sizeof(path),"%s/power_core/cpumask",FAKE_SYSFS_BASE);
  write_file(path,"0,2\n");
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  /* CPU 1 is an SMT sibling of 0/2 in this fixture -- not in the cpumask. */
  cgroup = power_core_counter_group("power-core",&power,1);
  assert(cgroup != NULL); /* group still exists -- same column set every row needs, see power.h */
  assert(cgroup->ncounters == 1);
  assert(cgroup->target_cpu == 1);
  assert(!strcmp(cgroup->cinfo[0].label,"core_joules")); /* identical column name/position either way */
  assert(cgroup->cinfo[0].device_type == POWER_CORE_NOT_APPLICABLE_DEVICE_TYPE);
  assert(cgroup->cinfo[0].fd == -1); /* pre-set so read_counters()'s existing fd!=-1 guard skips it */

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_core_counter_group non-representative CPU\n");
}

static void test_power_core_counter_group_unsupported(void){
  struct power_capabilities power;
  struct counter_group *cgroup;

  printf("Testing power_core_counter_group: power_core itself absent -- NULL, not a crash...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);
  power = power_probe_at(FAKE_SYSFS_BASE, "/tmp/nonexistent");

  cgroup = power_core_counter_group("power-core",&power,0);
  assert(cgroup == NULL);

  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("PASS: power_core_counter_group unsupported\n");
}

static void test_power_hwmon_fallback(void){
  struct power_capabilities power;

  printf("Testing power_probe_at: hwmon fallback...\n");

  rmdir_recursive(FAKE_SYSFS_BASE);
  mkdir_or_die(FAKE_SYSFS_BASE);
  
  #define FAKE_HWMON_BASE "/tmp/test_power_hwmon"
  rmdir_recursive(FAKE_HWMON_BASE);
  mkdir_or_die(FAKE_HWMON_BASE);
  mkdir_or_die(FAKE_HWMON_BASE "/hwmon0");
  
  write_file(FAKE_HWMON_BASE "/hwmon0/energy1_input", "15000000\n");
  write_file(FAKE_HWMON_BASE "/hwmon0/name", "scmi_sensors\n");

  power = power_probe_at(FAKE_SYSFS_BASE, FAKE_HWMON_BASE);
  assert(power.supported == 1);
  assert(power.is_fallback == 1);
  assert(strcmp(power.fallback_path, FAKE_HWMON_BASE "/hwmon0/energy1_input") == 0);

  rmdir_recursive(FAKE_SYSFS_BASE);
  rmdir_recursive(FAKE_HWMON_BASE);
  printf("PASS: power_probe_at hwmon fallback\n");
}

int main(void){
  test_power_not_present();
  test_power_fully_present();
  test_power_pkg_only();
  test_power_missing_event_file();
  test_power_scale_defaults_to_one();
  test_power_build_pkg_event();
  test_power_build_pkg_event_shifted();
  test_power_build_pkg_event_absent();
  test_power_counter_group_structural();
  test_power_core_cpumask_parsing();
  test_power_core_cpumask_absent_when_power_core_absent();
  test_power_build_core_event();
  test_power_core_counter_group_representative();
  test_power_core_counter_group_not_representative();
  test_power_core_counter_group_unsupported();
  test_power_hwmon_fallback();

  printf("\nAll test_power tests passed.\n");
  return 0;
}
