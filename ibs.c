/*
 * ibs.c - AMD IBS capability discovery, described in ibs.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include "wspy.h"
#include "ibs.h"
#include "error.h"

enum ibs_profile ibs_collection_profile = IBS_PROFILE_NONE;
struct ibs_profile_params ibs_params = {0,0,0};

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

static int cmp_format_field(const void *a,const void *b){
  return strcmp(((const struct ibs_format_field *)a)->name,((const struct ibs_format_field *)b)->name);
}

static int cmp_cap(const void *a,const void *b){
  return strcmp(((const struct ibs_cap *)a)->name,((const struct ibs_cap *)b)->name);
}

static void scan_format_fields(const char *pmu_path,struct ibs_pmu *pmu){
  char dir_path[350];
  DIR *dir;
  struct dirent *de;

  snprintf(dir_path,sizeof(dir_path),"%s/format",pmu_path);
  dir = opendir(dir_path);
  if (!dir) return;

  while ((de = readdir(dir)) != NULL && pmu->format_count < IBS_MAX_FORMAT_FIELDS){
    char file_path[700];
    char line[64];
    struct ibs_format_field *f;

    if (de->d_name[0] == '.') continue;
    snprintf(file_path,sizeof(file_path),"%s/%s",dir_path,de->d_name);
    if (read_sysfs_line(file_path,line,sizeof(line)) != 0) continue;

    f = &pmu->format[pmu->format_count++];
    snprintf(f->name,sizeof(f->name),"%.*s",(int)sizeof(f->name)-1,de->d_name);
    snprintf(f->location,sizeof(f->location),"%.*s",(int)sizeof(f->location)-1,line);
  }
  closedir(dir);

  /* readdir() order isn't guaranteed stable across kernel versions/runs --
   * sort so report output (and test assertions) don't depend on it. */
  qsort(pmu->format,pmu->format_count,sizeof(pmu->format[0]),cmp_format_field);
}

static void scan_caps(const char *pmu_path,struct ibs_pmu *pmu){
  char dir_path[350];
  DIR *dir;
  struct dirent *de;

  snprintf(dir_path,sizeof(dir_path),"%s/caps",pmu_path);
  dir = opendir(dir_path);
  if (!dir) return;

  while ((de = readdir(dir)) != NULL && pmu->caps_count < IBS_MAX_CAPS){
    char file_path[700];
    char line[64];
    struct ibs_cap *c;

    if (de->d_name[0] == '.') continue;
    snprintf(file_path,sizeof(file_path),"%s/%s",dir_path,de->d_name);
    if (read_sysfs_line(file_path,line,sizeof(line)) != 0) continue;

    c = &pmu->caps[pmu->caps_count++];
    snprintf(c->name,sizeof(c->name),"%.*s",(int)sizeof(c->name)-1,de->d_name);
    c->enabled = atoi(line);
  }
  closedir(dir);

  qsort(pmu->caps,pmu->caps_count,sizeof(pmu->caps[0]),cmp_cap);
}

/* Parameterized by sysfs_base so tests can point it at a fake directory
 * tree instead of the real /sys/bus/event_source/devices. */
static void probe_ibs_pmu_at(const char *sysfs_base,const char *pmu_name,struct ibs_pmu *pmu){
  char pmu_path[300];
  char type_path[350];
  char line[64];

  memset(pmu,0,sizeof(*pmu));
  pmu->type = -1;

  snprintf(pmu_path,sizeof(pmu_path),"%s/%s",sysfs_base,pmu_name);
  if (access(pmu_path,F_OK) != 0) return; /* not present -- fine, e.g. non-AMD host */
  pmu->present = 1;

  snprintf(type_path,sizeof(type_path),"%s/type",pmu_path);
  if (read_sysfs_line(type_path,line,sizeof(line)) == 0) pmu->type = atoi(line);

  scan_format_fields(pmu_path,pmu);
  scan_caps(pmu_path,pmu);
}

static struct ibs_capabilities ibs_probe_at(const char *sysfs_base){
  struct ibs_capabilities ibs;

  memset(&ibs,0,sizeof(ibs));
  probe_ibs_pmu_at(sysfs_base,"ibs_fetch",&ibs.fetch);
  probe_ibs_pmu_at(sysfs_base,"ibs_op",&ibs.op);
  ibs.supported = ibs.fetch.present && ibs.op.present;
  return ibs;
}

struct ibs_capabilities ibs_probe(void){
  return ibs_probe_at("/sys/bus/event_source/devices");
}

const struct ibs_format_field *ibs_pmu_format(const struct ibs_pmu *pmu,const char *name){
  int i;
  for (i = 0; i < pmu->format_count; i++)
    if (!strcmp(pmu->format[i].name,name)) return &pmu->format[i];
  return NULL;
}

const struct ibs_cap *ibs_pmu_cap(const struct ibs_pmu *pmu,const char *name){
  int i;
  for (i = 0; i < pmu->caps_count; i++)
    if (!strcmp(pmu->caps[i].name,name)) return &pmu->caps[i];
  return NULL;
}

/* Parses a format field's sysfs location string ("config:59" or
 * "config1:0-11") into which perf_event_attr config word it belongs to
 * (0/1/2 for config/config1/config2) and its bit range [lo,hi]. Returns -1
 * for a location this code doesn't know how to place (e.g. some future
 * "configN" word), so the caller can skip it rather than guess. */
static int parse_format_location(const char *location,int *word,int *lo,int *hi){
  const char *colon = strchr(location,':');
  int prefix_len;

  if (!colon) return -1;
  prefix_len = (int)(colon - location);
  if (prefix_len == 6 && !strncmp(location,"config",6)) *word = 0;
  else if (prefix_len == 7 && !strncmp(location,"config1",7)) *word = 1;
  else if (prefix_len == 7 && !strncmp(location,"config2",7)) *word = 2;
  else return -1;

  if (sscanf(colon+1,"%d-%d",lo,hi) == 2) return 0;
  if (sscanf(colon+1,"%d",lo) == 1){ *hi = *lo; return 0; }
  return -1;
}

/* ORs value, shifted into the bit range named by f->location, into the
 * matching config word of ev. Leaves ev unmodified (with a warning) if the
 * location can't be parsed -- graceful degradation, same as an absent field
 * entirely, rather than corrupting an unrelated bit range. */
static void apply_format_field(struct ibs_event *ev,const struct ibs_format_field *f,unsigned long value){
  int word,lo,hi;
  unsigned long mask,shifted,*target;

  if (parse_format_location(f->location,&word,&lo,&hi) != 0){
    warning("IBS format field %s has unrecognized location '%s', not applied\n",f->name,f->location);
    return;
  }
  mask = (hi >= 63) ? (~0UL << lo) : (((1UL << (hi-lo+1)) - 1) << lo);
  shifted = (value << lo) & mask;
  target = (word == 0) ? &ev->config : (word == 1) ? &ev->config1 : &ev->config2;
  *target = (*target & ~mask) | shifted;
}

struct ibs_event ibs_build_fetch_event(const struct ibs_pmu *fetch,enum ibs_profile profile,const struct ibs_profile_params *params){
  struct ibs_event ev;
  const struct ibs_format_field *f;
  static const struct ibs_profile_params defaults = {0,0,0};

  memset(&ev,0,sizeof(ev));
  if (!params) params = &defaults;
  if (!fetch->present) return ev;
  ev.valid = 1;
  ev.type = fetch->type;
  ev.sample_period = params->maxcnt ? params->maxcnt : IBS_DEFAULT_MAXCNT;

  if (profile == IBS_PROFILE_MEMORY_DEEP){
    ev.l3missonly_requested = 1;
    if ((f = ibs_pmu_format(fetch,"l3missonly"))){
      apply_format_field(&ev,f,1);
      ev.l3missonly_applied = 1;
    }
    ev.fetchlat_requested = 1;
    if ((f = ibs_pmu_format(fetch,"fetchlat"))){
      ev.fetchlat_threshold = params->fetchlat_threshold ? params->fetchlat_threshold : IBS_DEFAULT_FETCHLAT_THRESHOLD;
      apply_format_field(&ev,f,ev.fetchlat_threshold);
      ev.fetchlat_applied = 1;
    }
  }
  return ev;
}

struct ibs_event ibs_build_op_event(const struct ibs_pmu *op,enum ibs_profile profile,const struct ibs_profile_params *params){
  struct ibs_event ev;
  const struct ibs_format_field *f;
  static const struct ibs_profile_params defaults = {0,0,0};

  memset(&ev,0,sizeof(ev));
  if (!params) params = &defaults;
  if (!op->present) return ev;
  ev.valid = 1;
  ev.type = op->type;
  ev.sample_period = params->maxcnt ? params->maxcnt : IBS_DEFAULT_MAXCNT;

  if (profile == IBS_PROFILE_MEMORY_DEEP){
    ev.l3missonly_requested = 1;
    if ((f = ibs_pmu_format(op,"l3missonly"))){
      apply_format_field(&ev,f,1);
      ev.l3missonly_applied = 1;
    }
    ev.ldlat_requested = 1;
    if ((f = ibs_pmu_format(op,"ldlat"))){
      ev.ldlat_threshold = params->ldlat_threshold ? params->ldlat_threshold : IBS_DEFAULT_LDLAT_THRESHOLD;
      apply_format_field(&ev,f,ev.ldlat_threshold);
      ev.ldlat_applied = 1;
    }
  }
  return ev;
}

struct ibs_event ibs_build_op_unfiltered_event(const struct ibs_pmu *op,const struct ibs_profile_params *params){
  struct ibs_event ev;
  static const struct ibs_profile_params defaults = {0,0,0};

  memset(&ev,0,sizeof(ev));
  if (!params) params = &defaults;
  if (!op->present) return ev;
  ev.valid = 1;
  ev.type = op->type;
  ev.sample_period = params->maxcnt ? params->maxcnt : IBS_DEFAULT_MAXCNT;
  return ev;
}

struct counter_group *ibs_counter_group(char *name,enum ibs_profile profile,const struct ibs_profile_params *params){
  struct ibs_capabilities caps;
  struct ibs_event fetch_ev,op_ev,op_unfiltered_ev;
  struct counter_group *cgroup;
  int n,idx;

  memset(&op_unfiltered_ev,0,sizeof(op_unfiltered_ev));
  if (profile == IBS_PROFILE_NONE) return NULL;

  caps = ibs_probe();
  if (!caps.supported){
    warning("AMD IBS not supported on this host/kernel -- %s profile produces no counters\n",
	    profile == IBS_PROFILE_MEMORY_DEEP ? "ibs-memory-deep" : "ibs-basic");
    return NULL;
  }

  fetch_ev = ibs_build_fetch_event(&caps.fetch,profile,params);
  op_ev = ibs_build_op_event(&caps.op,profile,params);
  if (profile == IBS_PROFILE_MEMORY_DEEP)
    op_unfiltered_ev = ibs_build_op_unfiltered_event(&caps.op,params);

  n = fetch_ev.valid + op_ev.valid + op_unfiltered_ev.valid;
  if (n == 0) return NULL;

  cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  /* Per-counter device_type carries each PMU's real dynamic type -- the
   * same escape hatch raw_counter_group() uses for L3 cache events, since
   * setup_counters() only honors cgroup->type_id itself for the single-type
   * groups (PERF_TYPE_SOFTWARE etc). */
  cgroup->type_id = PERF_TYPE_RAW;
  cgroup->mask = COUNTER_IBS;
  cgroup->ncounters = n;
  cgroup->cinfo = calloc(n,sizeof(struct counter_info));

  // IBS is system-wide only (CLAUDE.md's ibs.c entry) -- see struct
  // counter_info's requires_system_wide comment.
  idx = 0;
  if (fetch_ev.valid){
    cgroup->cinfo[idx].label = strdup("ibs_fetch");
    cgroup->cinfo[idx].device_type = fetch_ev.type;
    cgroup->cinfo[idx].config = fetch_ev.config;
    cgroup->cinfo[idx].config1 = fetch_ev.config1;
    cgroup->cinfo[idx].config2 = fetch_ev.config2;
    cgroup->cinfo[idx].sample_period = fetch_ev.sample_period;
    cgroup->cinfo[idx].is_group_leader = 1;
    cgroup->cinfo[idx].requires_system_wide = 1;
    idx++;
  }
  if (op_ev.valid){
    cgroup->cinfo[idx].label = strdup("ibs_op");
    cgroup->cinfo[idx].device_type = op_ev.type;
    cgroup->cinfo[idx].config = op_ev.config;
    cgroup->cinfo[idx].config1 = op_ev.config1;
    cgroup->cinfo[idx].config2 = op_ev.config2;
    cgroup->cinfo[idx].sample_period = op_ev.sample_period;
    cgroup->cinfo[idx].is_group_leader = 1;
    cgroup->cinfo[idx].requires_system_wide = 1;
    idx++;
  }
  if (op_unfiltered_ev.valid){
    cgroup->cinfo[idx].label = strdup("ibs_op_unfiltered");
    cgroup->cinfo[idx].device_type = op_unfiltered_ev.type;
    cgroup->cinfo[idx].config = op_unfiltered_ev.config;
    cgroup->cinfo[idx].config1 = op_unfiltered_ev.config1;
    cgroup->cinfo[idx].config2 = op_unfiltered_ev.config2;
    cgroup->cinfo[idx].sample_period = op_unfiltered_ev.sample_period;
    cgroup->cinfo[idx].is_group_leader = 1;
    cgroup->cinfo[idx].requires_system_wide = 1;
    idx++;
  }
  return cgroup;
}

static void print_ibs_pmu_report(const char *name,const struct ibs_pmu *pmu){
  int i;

  if (!pmu->present){
    fprintf(outfile,"  %-10s not present\n",name);
    return;
  }
  fprintf(outfile,"  %-10s available (type=%d), %d format field(s), %d cap(s)\n",
          name,pmu->type,pmu->format_count,pmu->caps_count);
  for (i = 0; i < pmu->format_count; i++)
    fprintf(outfile,"    format          %-16s %s\n",pmu->format[i].name,pmu->format[i].location);
  for (i = 0; i < pmu->caps_count; i++)
    fprintf(outfile,"    cap             %-16s %s\n",pmu->caps[i].name,pmu->caps[i].enabled ? "yes" : "no");
}

void print_ibs_capability_report(const struct ibs_capabilities *ibs){
  fprintf(outfile,"IBS capability report: %s\n",ibs->supported ? "supported" : "not supported");
  print_ibs_pmu_report("ibs_fetch",&ibs->fetch);
  print_ibs_pmu_report("ibs_op",&ibs->op);
}
