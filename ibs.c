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
