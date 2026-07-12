/*
 * provenance.c - best-effort environment/provenance capture, described in
 * provenance.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#ifdef __x86_64__
#include <cpuid.h>
#endif
#include <sys/sysinfo.h>
#ifdef __GLIBC__
#include <gnu/libc-version.h>
#endif
#include "provenance.h"

static void field_set_unavailable(struct provenance_field *f,const char *reason){
  f->available = 0;
  f->value[0] = '\0';
  snprintf(f->reason,sizeof(f->reason),"%s",reason);
}

static void field_set_str(struct provenance_field *f,const char *value){
  f->available = 1;
  f->reason[0] = '\0';
  snprintf(f->value,sizeof(f->value),"%s",value);
}

/* Reads one line from path (a sysfs/procfs attribute file), stripping the
 * trailing newline. Returns 0 and fills buf on success, -1 (errno set) on
 * failure. */
static int read_line_trimmed(const char *path,char *buf,size_t bufsize){
  FILE *fp;
  size_t len;

  fp = fopen(path,"r");
  if (!fp) return -1;
  if (!fgets(buf,bufsize,fp)){
    fclose(fp);
    errno = EIO;
    return -1;
  }
  fclose(fp);
  len = strlen(buf);
  while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
  return 0;
}

static void collect_sysfs_field(struct provenance_field *f,const char *path){
  char buf[PROVENANCE_VALUE_LEN];

  if (read_line_trimmed(path,buf,sizeof(buf)) == 0) field_set_str(f,buf);
  else field_set_unavailable(f,strerror(errno));
}

static void collect_virt_role(struct provenance_info *info){
#ifdef __x86_64__
  unsigned int eax,ebx,ecx,edx;

  __cpuid(1,eax,ebx,ecx,edx);
  if (ecx & (1u << 31)){
    field_set_str(&info->virt_role,"guest");
    __cpuid(0x40000000,eax,ebx,ecx,edx);
    {
      char vendor[13];
      memcpy(vendor,&ebx,4);
      memcpy(vendor+4,&ecx,4);
      memcpy(vendor+8,&edx,4);
      vendor[12] = '\0';
      if (vendor[0]) field_set_str(&info->hypervisor_vendor,vendor);
      else field_set_unavailable(&info->hypervisor_vendor,"hypervisor did not report a vendor id");
    }
  } else {
    field_set_str(&info->virt_role,"host");
    field_set_unavailable(&info->hypervisor_vendor,"not applicable (host, not a guest)");
  }
#else
  FILE *fp;
  char line[256];
  int is_guest = 0;

  fp = fopen("/proc/cpuinfo","r");
  if (fp){
    while (fgets(line,sizeof(line),fp)){
      if (strstr(line,"hypervisor")){
        is_guest = 1;
        break;
      }
    }
    fclose(fp);
  }

  if (is_guest) field_set_str(&info->virt_role,"guest");
  else field_set_str(&info->virt_role,"host");
  field_set_unavailable(&info->hypervisor_vendor,
                        "hypervisor vendor detection via cpuid is unavailable on non-x86");
#endif
}

static void collect_microcode(struct provenance_field *f){
  FILE *fp;
  char line[256];

  fp = fopen("/proc/cpuinfo","r");
  if (!fp){
    field_set_unavailable(f,strerror(errno));
    return;
  }
  while (fgets(line,sizeof(line),fp)){
    if (!strncmp(line,"microcode",9)){
      char *colon = strchr(line,':');
      char *value,*end;
      if (!colon) continue;
      value = colon + 1;
      while (*value == ' ' || *value == '\t') value++;
      end = value + strlen(value);
      while (end > value && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = '\0';
      field_set_str(f,value);
      fclose(fp);
      return;
    }
  }
  fclose(fp);
  field_set_unavailable(f,"no \"microcode\" line in /proc/cpuinfo");
}

static void collect_governor(struct provenance_info *info){
  char buf[PROVENANCE_VALUE_LEN];
  long ncpus,i;
  int uniform = 1;

  if (read_line_trimmed("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",buf,sizeof(buf)) != 0){
    field_set_unavailable(&info->cpu_governor,strerror(errno));
    field_set_unavailable(&info->cpu_scaling_driver,"scaling_governor unavailable");
    info->cpu_governor_uniform = 0;
    return;
  }
  field_set_str(&info->cpu_governor,buf);
  collect_sysfs_field(&info->cpu_scaling_driver,"/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");

  ncpus = sysconf(_SC_NPROCESSORS_ONLN);
  for (i = 1; i < ncpus; i++){
    char path[96];
    char other[PROVENANCE_VALUE_LEN];
    snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_governor",i);
    if (read_line_trimmed(path,other,sizeof(other)) != 0) continue; /* offline/missing core, skip */
    if (strcmp(other,buf) != 0){ uniform = 0; break; }
  }
  info->cpu_governor_uniform = uniform;
}

static void collect_mem_total(struct provenance_field *f){
  struct sysinfo si;

  if (sysinfo(&si) != 0){
    field_set_unavailable(f,strerror(errno));
    return;
  }
  {
    char buf[32];
    unsigned long long kb = (unsigned long long)si.totalram * si.mem_unit / 1024;
    snprintf(buf,sizeof(buf),"%llu",kb);
    field_set_str(f,buf);
  }
}

static void collect_tool_versions(struct provenance_info *info){
#ifdef __VERSION__
  field_set_str(&info->compiler_version,"GCC " __VERSION__);
#else
  field_set_unavailable(&info->compiler_version,"compiler did not define __VERSION__");
#endif
#ifdef __GLIBC__
  field_set_str(&info->libc_version,gnu_get_libc_version());
#else
  field_set_unavailable(&info->libc_version,"non-glibc C library");
#endif
}

void provenance_collect(struct provenance_info *info){
  memset(info,0,sizeof(*info));
  collect_virt_role(info);
  collect_microcode(&info->microcode_version);
  collect_sysfs_field(&info->bios_vendor,"/sys/class/dmi/id/bios_vendor");
  collect_sysfs_field(&info->bios_version,"/sys/class/dmi/id/bios_version");
  collect_sysfs_field(&info->bios_date,"/sys/class/dmi/id/bios_date");
  collect_governor(info);
  collect_mem_total(&info->mem_total_kb);
  collect_tool_versions(info);
}

int provenance_gaps(const struct provenance_info *info,struct provenance_gap *out){
  struct { const char *name; const struct provenance_field *field; } tracked[PROVENANCE_TRACKED_FIELD_COUNT];
  int n = 0;
  int i;

  tracked[0].name = "virt_role";           tracked[0].field = &info->virt_role;
  tracked[1].name = "microcode_version";   tracked[1].field = &info->microcode_version;
  tracked[2].name = "bios_vendor";         tracked[2].field = &info->bios_vendor;
  tracked[3].name = "bios_version";        tracked[3].field = &info->bios_version;
  tracked[4].name = "bios_date";           tracked[4].field = &info->bios_date;
  tracked[5].name = "cpu_governor";        tracked[5].field = &info->cpu_governor;
  tracked[6].name = "memory_total_kb";     tracked[6].field = &info->mem_total_kb;
  tracked[7].name = "compiler_version";    tracked[7].field = &info->compiler_version;
  tracked[8].name = "libc_version";        tracked[8].field = &info->libc_version;

  for (i = 0; i < PROVENANCE_TRACKED_FIELD_COUNT; i++){
    if (!tracked[i].field->available){
      out[n].field_name = tracked[i].name;
      out[n].reason = tracked[i].field->reason;
      n++;
    }
  }
  return n;
}

int provenance_count_available(const struct provenance_info *info){
  struct provenance_gap gaps[PROVENANCE_TRACKED_FIELD_COUNT];
  return PROVENANCE_TRACKED_FIELD_COUNT - provenance_gaps(info,gaps);
}
