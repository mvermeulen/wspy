/*
 * power.c - CPU energy/power via the Linux power/power_core dynamic PMUs,
 * described in power.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "wspy.h"
#include "power.h"
#include "error.h"

/* Reads the first line of path (trailing newline/CR stripped) into buf.
 * Returns 0 on success, -1 if the file couldn't be opened/read. */
static int read_sysfs_line(const char *path,char *buf,size_t bufsize){
  FILE *fp;
  size_t len;

  fp = fopen(path,"r");
  if (!fp) return -1;
  if (!fgets(buf,bufsize,fp)){
    fclose(fp);
    return -1;
  }
  fclose(fp);
  len = strlen(buf);
  while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
  return 0;
}

/* Parses events/<name>'s own content ("event=0x02") into a plain numeric
 * value. Returns 0 on success, -1 if the text doesn't look like that. */
static int parse_event_value(const char *text,unsigned long *value){
  const char *eq = strchr(text,'=');
  if (!eq) return -1;
  *value = strtoul(eq+1,NULL,0);
  return 0;
}

/* Parses a format field's sysfs location string ("config:0-7" or
 * "config:3") into a [lo,hi] bit range within perf_event_attr.config --
 * power/power_core only ever place their one format field in "config"
 * itself (no config1/config2 word the way IBS's filters use), so this is
 * deliberately narrower than ibs.c's parse_format_location(). Returns -1
 * for anything else, so the caller can degrade rather than guess. */
static int parse_config_location(const char *location,int *lo,int *hi){
  const char *colon = strchr(location,':');
  if (!colon) return -1;
  if (strncmp(location,"config",6) || colon != location+6) return -1;
  if (sscanf(colon+1,"%d-%d",lo,hi) == 2) return 0;
  if (sscanf(colon+1,"%d",lo) == 1){ *hi = *lo; return 0; }
  return -1;
}

static void scan_format_fields(const char *pmu_path,struct power_pmu *pmu){
  char dir_path[350];
  DIR *dir;
  struct dirent *de;

  snprintf(dir_path,sizeof(dir_path),"%s/format",pmu_path);
  dir = opendir(dir_path);
  if (!dir) return;

  while ((de = readdir(dir)) != NULL && pmu->format_count < POWER_MAX_FORMAT_FIELDS){
    char file_path[700];
    char line[64];
    struct power_format_field *f;

    if (de->d_name[0] == '.') continue;
    snprintf(file_path,sizeof(file_path),"%s/%s",dir_path,de->d_name);
    if (read_sysfs_line(file_path,line,sizeof(line)) != 0) continue;

    f = &pmu->format[pmu->format_count++];
    snprintf(f->name,sizeof(f->name),"%.*s",(int)sizeof(f->name)-1,de->d_name);
    snprintf(f->location,sizeof(f->location),"%.*s",(int)sizeof(f->location)-1,line);
  }
  closedir(dir);
}

static const struct power_format_field *find_format(const struct power_pmu *pmu,const char *name){
  int i;
  for (i = 0; i < pmu->format_count; i++)
    if (!strcmp(pmu->format[i].name,name)) return &pmu->format[i];
  return NULL;
}

/* Parameterized by sysfs_base so tests can point it at a fake directory
 * tree instead of the real /sys/bus/event_source/devices, mirroring
 * ibs.c's ibs_probe_at()/probe_ibs_pmu_at(). */
static void probe_power_pmu_at(const char *sysfs_base,const char *pmu_name,
                                const char *event_name,struct power_pmu *pmu){
  char pmu_path[300];
  char path[400];
  char line[64];

  memset(pmu,0,sizeof(*pmu));
  pmu->type = -1;
  pmu->scale = 1.0; /* sane fallback if events/<name>.scale is itself absent */

  snprintf(pmu_path,sizeof(pmu_path),"%s/%s",sysfs_base,pmu_name);
  if (access(pmu_path,F_OK) != 0) return; /* not present -- fine, e.g. no RAPL-equivalent support */
  pmu->present = 1;

  snprintf(path,sizeof(path),"%s/type",pmu_path);
  if (read_sysfs_line(path,line,sizeof(line)) == 0) pmu->type = atoi(line);

  scan_format_fields(pmu_path,pmu);

  snprintf(path,sizeof(path),"%s/events/%s",pmu_path,event_name);
  if (read_sysfs_line(path,line,sizeof(line)) == 0 && parse_event_value(line,&pmu->event) == 0)
    pmu->event_present = 1;

  snprintf(path,sizeof(path),"%s/events/%s.scale",pmu_path,event_name);
  if (read_sysfs_line(path,line,sizeof(line)) == 0){
    char *end;
    double s = strtod(line,&end);
    if (end != line) pmu->scale = s;
  }

  snprintf(path,sizeof(path),"%s/events/%s.unit",pmu_path,event_name);
  read_sysfs_line(path,pmu->unit,sizeof(pmu->unit)); /* leaves unit[0]==0 on failure */
}

static struct power_capabilities power_probe_at(const char *sysfs_base){
  struct power_capabilities power;

  memset(&power,0,sizeof(power));
  probe_power_pmu_at(sysfs_base,"power","energy-pkg",&power.pkg);
  probe_power_pmu_at(sysfs_base,"power_core","energy-core",&power.core);
  power.supported = power.pkg.present && power.pkg.event_present;
  return power;
}

struct power_capabilities power_probe(void){
  return power_probe_at("/sys/bus/event_source/devices");
}

void print_power_capability_report(const struct power_capabilities *power){
  fprintf(outfile,"CPU power/energy capability report: %s\n",
          power->supported ? "supported (package)" : "not supported");
  if (power->pkg.present)
    fprintf(outfile,"  %-10s available (type=%d), event=0x%lx, scale=%g %s\n",
            "power",power->pkg.type,power->pkg.event,power->pkg.scale,
            power->pkg.unit[0] ? power->pkg.unit : "(unit unknown)");
  else
    fprintf(outfile,"  %-10s not present\n","power");
  if (power->core.present)
    fprintf(outfile,"  %-10s available (type=%d), event=0x%lx, scale=%g %s -- per-core"
            " breakdown not yet collected (v1 is package-level only)\n",
            "power_core",power->core.type,power->core.event,power->core.scale,
            power->core.unit[0] ? power->core.unit : "(unit unknown)");
  else
    fprintf(outfile,"  %-10s not present\n","power_core");
}

struct power_event power_build_pkg_event(const struct power_pmu *pkg){
  struct power_event ev;
  const struct power_format_field *f;
  int lo,hi;

  memset(&ev,0,sizeof(ev));
  if (!pkg->present || !pkg->event_present) return ev;
  ev.valid = 1;
  ev.type = pkg->type;

  f = find_format(pkg,"event");
  if (f && parse_config_location(f->location,&lo,&hi) == 0){
    unsigned long mask = (hi >= 63) ? (~0UL << lo) : (((1UL << (hi-lo+1)) - 1) << lo);
    ev.config = (pkg->event << lo) & mask;
  } else {
    /* format field missing/unparseable -- degrade to placing the event
     * value at bit 0 rather than dropping the counter entirely, same
     * graceful-degradation spirit as ibs.c's apply_format_field() warning
     * (though that one leaves the bit range untouched since it's one of
     * several optional filters; here it's the only field this event has). */
    warning("power PMU's 'event' format field has unrecognized location -- placing raw"
            " event value at bit 0\n");
    ev.config = pkg->event;
  }
  return ev;
}

struct counter_group *power_counter_group(char *name){
  struct power_capabilities caps;
  struct power_event ev;
  struct counter_group *cgroup;

  caps = power_probe();
  if (!caps.supported){
    warning("CPU power/energy (power/energy-pkg) not supported on this host/kernel -- "
            "--power produces no counters\n");
    return NULL;
  }

  ev = power_build_pkg_event(&caps.pkg);
  if (!ev.valid) return NULL; /* shouldn't happen given caps.supported, but guard anyway */

  cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  /* Per-counter device_type carries the real dynamic type -- the same
   * escape hatch raw_counter_group()/ibs_counter_group() use for L3/IBS
   * events, since setup_counters() only honors cgroup->type_id itself for
   * the single-type groups (PERF_TYPE_SOFTWARE etc). */
  cgroup->type_id = PERF_TYPE_RAW;
  cgroup->mask = COUNTER_POWER;
  cgroup->ncounters = 1;
  cgroup->cinfo = calloc(1,sizeof(struct counter_info));
  cgroup->cinfo[0].label = strdup("pkg_joules");
  cgroup->cinfo[0].device_type = ev.type;
  cgroup->cinfo[0].config = ev.config;
  cgroup->cinfo[0].is_group_leader = 1;
  cgroup->cinfo[0].scale = caps.pkg.scale;
  return cgroup;
}
