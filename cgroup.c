/*
 * cgroup.c - best-effort cgroup v2 identity/limits/throttling capture,
 * described in cgroup.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "cgroup.h"

struct cgroup_info cgroup_state;
struct cgroup_throttle cgroup_throttle_baseline;

/* Reads one line from path, trimmed of a trailing newline/CR. Returns 0 on
 * success, -1 if the file couldn't be opened/read. */
static int read_line_trimmed(const char *path,char *buf,size_t bufsize){
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

/* Reads proc_self_cgroup_path (real path "/proc/self/cgroup", parameterized
 * for test_cgroup.c) looking for the cgroup v2 unified-hierarchy line
 * ("0::<path>") -- always hierarchy id 0, regardless of whether other
 * (cgroup v1) controller lines are also present in a hybrid mount. Returns
 * 0 and fills path (trimmed) on success, -1 if no such line was found or
 * the file couldn't be read at all. */
static int read_cgroup_v2_path_at(const char *proc_self_cgroup_path,char *path,size_t pathsize){
  FILE *fp;
  char line[600];
  int found = 0;

  fp = fopen(proc_self_cgroup_path,"r");
  if (!fp) return -1;
  while (fgets(line,sizeof(line),fp)){
    if (!strncmp(line,"0::",3)){
      size_t len;
      snprintf(path,pathsize,"%s",line + 3);
      len = strlen(path);
      while (len > 0 && (path[len-1] == '\n' || path[len-1] == '\r')) path[--len] = '\0';
      found = 1;
      break;
    }
  }
  fclose(fp);
  return found ? 0 : -1;
}

/* Parses cpu.max's "<quota> <period>" line -- <quota> is either "max"
 * (unlimited) or a microsecond count, <period> is always a microsecond
 * count. Returns 0 on success, -1 if the line doesn't match that shape. */
static int parse_cpu_max_line(const char *line,long long *quota_us,long long *period_us){
  char quota_tok[32];
  long long period;

  if (sscanf(line,"%31s %lld",quota_tok,&period) != 2) return -1;
  *period_us = period;
  if (!strcmp(quota_tok,"max")) *quota_us = -1;
  else *quota_us = strtoll(quota_tok,NULL,10);
  return 0;
}

/* Parses a bare "max"-or-number value file (memory.max/memory.high).
 * Returns 0 and fills *out (-1 for "max") on success, -1 on read/parse
 * failure. */
static int parse_max_or_number_at(const char *path,long long *out){
  char buf[64];
  char *end;

  if (read_line_trimmed(path,buf,sizeof(buf)) != 0) return -1;
  if (!strcmp(buf,"max")){ *out = -1; return 0; }
  *out = strtoll(buf,&end,10);
  if (end == buf) return -1;
  return 0;
}

/* Parses cpu.stat's "<key> <value>" lines into *out. Returns 0 and sets
 * out->available=1 only if all three tracked keys were found; -1 (and
 * out left zeroed) otherwise -- a cgroup whose cpu.stat is present but
 * missing these keys (shouldn't happen on a real cgroup v2 host, but a
 * test fixture might) is treated the same as "couldn't read it at all". */
static int parse_cpu_stat_at(const char *path,struct cgroup_throttle *out){
  FILE *fp;
  char line[128];
  int found_periods = 0,found_throttled = 0,found_usec = 0;

  memset(out,0,sizeof(*out));
  fp = fopen(path,"r");
  if (!fp) return -1;
  while (fgets(line,sizeof(line),fp)){
    unsigned long long value;
    if (sscanf(line,"nr_periods %llu",&value) == 1){ out->nr_periods = value; found_periods = 1; }
    else if (sscanf(line,"nr_throttled %llu",&value) == 1){ out->nr_throttled = value; found_throttled = 1; }
    else if (sscanf(line,"throttled_usec %llu",&value) == 1){ out->throttled_usec = value; found_usec = 1; }
  }
  fclose(fp);
  if (found_periods && found_throttled && found_usec){
    out->available = 1;
    return 0;
  }
  return -1;
}

static void collect_identity_and_limits_at(const char *proc_self_cgroup_path,const char *cgroup_root,
                                            struct cgroup_info *info){
  char cg_path[512];
  char full_path[900];

  memset(info,0,sizeof(*info));

  if (read_cgroup_v2_path_at(proc_self_cgroup_path,cg_path,sizeof(cg_path)) != 0){
    return; /* info->available stays 0: no cgroup v2 unified hierarchy found */
  }
  info->available = 1;
  snprintf(info->path,sizeof(info->path),"%s",cg_path);

  snprintf(full_path,sizeof(full_path),"%s%s/cpu.max",cgroup_root,cg_path);
  {
    char line[64];
    if (read_line_trimmed(full_path,line,sizeof(line)) == 0 &&
        parse_cpu_max_line(line,&info->cpu_quota_us,&info->cpu_period_us) == 0){
      info->cpu_max_available = 1;
    }
  }

  snprintf(full_path,sizeof(full_path),"%s%s/cpu.weight",cgroup_root,cg_path);
  {
    char line[32];
    char *end;
    if (read_line_trimmed(full_path,line,sizeof(line)) == 0){
      long weight = strtol(line,&end,10);
      if (end != line){
        info->cpu_weight = (int)weight;
        info->cpu_weight_available = 1;
      }
    }
  }

  snprintf(full_path,sizeof(full_path),"%s%s/memory.max",cgroup_root,cg_path);
  if (parse_max_or_number_at(full_path,&info->memory_max_bytes) == 0){
    info->memory_max_available = 1;
  }

  snprintf(full_path,sizeof(full_path),"%s%s/memory.high",cgroup_root,cg_path);
  if (parse_max_or_number_at(full_path,&info->memory_high_bytes) == 0){
    info->memory_high_available = 1;
  }
}

static void read_throttle_at(const char *cgroup_root,const struct cgroup_info *info,
                              struct cgroup_throttle *out){
  char full_path[900];

  memset(out,0,sizeof(*out));
  if (!info->available) return;
  snprintf(full_path,sizeof(full_path),"%s%s/cpu.stat",cgroup_root,info->path);
  parse_cpu_stat_at(full_path,out);
}

void cgroup_collect_identity_and_limits(struct cgroup_info *info){
  collect_identity_and_limits_at("/proc/self/cgroup","/sys/fs/cgroup",info);
}

void cgroup_read_throttle(const struct cgroup_info *info,struct cgroup_throttle *out){
  read_throttle_at("/sys/fs/cgroup",info,out);
}

void cgroup_throttle_delta(const struct cgroup_throttle *start,const struct cgroup_throttle *end,
                           struct cgroup_throttle *delta){
  memset(delta,0,sizeof(*delta));
  if (!start->available || !end->available) return;
  delta->available = 1;
  delta->nr_periods = end->nr_periods - start->nr_periods;
  delta->nr_throttled = end->nr_throttled - start->nr_throttled;
  delta->throttled_usec = end->throttled_usec - start->throttled_usec;
}
