/*
 * system.c - system-wide status
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>
#include <dirent.h>
#include "wspy.h"
#include "error.h"
#if AMDGPU
#include "amd_sysfs.h"
#endif

unsigned int system_mask = SYSTEM_LOADAVG|SYSTEM_CPU|SYSTEM_NETWORK|SYSTEM_FREQ|SYSTEM_TEMP|SYSTEM_DISK|SYSTEM_MEM;

struct netinfo {
  char *name;
  unsigned long bytes, last_bytes, prev_bytes;
};

// one whole block device (archive/wspy2.0/diskstats.c did this once, dropped
// in the 2.0->3.0 rewrite -- this reuses its /proc/partitions enumeration
// approach but reads deltas straight into ordinary SYSTEM_* CSV/header
// columns instead of that version's own per-device disk-<dev>.csv file)
struct diskinfo {
  char *name;
  char statfile[300];
  // sectors are always counted in 512-byte units by the kernel regardless of
  // the device's own logical block size, so no blocksize lookup is needed
  // (unlike archive/wspy2.0/diskstats.c's BLKSSZGET ioctl)
  unsigned long read_sectors, last_read_sectors, prev_read_sectors;
  unsigned long write_sectors, last_write_sectors, prev_write_sectors;
  unsigned long io_ms, last_io_ms, prev_io_ms;
};

// system state
struct system_state {
  double load; // 1 minute load average
  int runnable; // # runnable processes
  struct cpustat {
    unsigned long usertime, systemtime, idletime, iowaittime, irqtime;
    unsigned long last_usertime, last_systemtime, last_idletime, last_iowaittime, last_irqtime;
    unsigned long prev_usertime, prev_systemtime, prev_idletime, prev_iowaittime, prev_irqtime;
  } cpu;
  int num_net;
  struct netinfo *netinfo;
  int num_disk;
  struct diskinfo *diskinfo;
  // host-wide memory pressure (archive/wspy2.0/memstats.c did this once
  // against a fixed 18-label table; this keeps the same /proc/meminfo
  // source but narrows to the 6 fields INVESTIGATION.md's item calls out --
  // absolute point-in-time gauges, not deltas, unlike net/disk's counters)
  struct meminfo {
    unsigned long free_kb,cached_kb,dirty_kb,writeback_kb,swap_free_kb,committed_as_kb;
  } mem;
  double freq_mhz; // average current frequency across online cpus with cpufreq
  double cpu_temp_c; // average CPU package/die temperature across discovered hwmon sensors
  struct gpu {
    int busy_percent;
    int last_busy_percent;
    int prev_busy_percent;
  } gpu;
} system_state = { 0 };

// read /proc/net/dev and initialize the system_state structure for networks...
void setup_net_info(void){
  FILE *fp;
  char buffer[1024];
  char *p,*p2;
  int capacity = 0;
  if ((fp = fopen("/proc/net/dev","r")) == NULL) return;
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    p = strchr(buffer,':');
    if (p == NULL) continue; // skip header lines
    *p = 0;
    p2 = buffer;
    while (isspace(*p2)) p2++;
    if (system_state.num_net >= capacity){
      int newcap = capacity ? capacity * 2 : 4;
      struct netinfo *newlist = realloc(system_state.netinfo,newcap * sizeof(struct netinfo));
      if (newlist == NULL){
        fclose(fp);
        return;
      }
      // zero initialize new slots
      if (newcap > capacity){
        memset(newlist + capacity,0,(newcap - capacity)*sizeof(struct netinfo));
      }
      system_state.netinfo = newlist;
      capacity = newcap;
    }
    system_state.netinfo[system_state.num_net].name = strdup(p2);
    if (system_state.netinfo[system_state.num_net].name == NULL || system_state.netinfo[system_state.num_net].name[0] == '\0'){
      char tmp[32];
      snprintf(tmp,sizeof(tmp),"net%d",system_state.num_net);
      if (system_state.netinfo[system_state.num_net].name) free(system_state.netinfo[system_state.num_net].name);
      system_state.netinfo[system_state.num_net].name = strdup(tmp);
    }
    debug("parsed network interface: %s\n",system_state.netinfo[system_state.num_net].name);
    system_state.num_net++;
  }
  fclose(fp);
  if (system_state.num_net == 0){
    free(system_state.netinfo);
    system_state.netinfo = NULL;
  }
}

// device-name prefixes worth excluding from disk I/O collection by default:
// loop (snap/squashfs mounts -- confirmed live on a real dev host to number
// in the dozens, each permanently read=0/write=0/time=0 since the loop
// driver's own /sys/block/loopN/stat never reflects the backing file's real
// I/O), ram/zram (RAM-backed, not physical disk activity at all). Not a
// user-toggleable filter -- these names are never what "disk I/O stats"
// means, the same "measured vs unavailable" curation judgment amd_sysfs.c's
// vendor-id scan already makes for GPU enumeration. Filtering these out also
// keeps real-world CSV width away from plot.c's MAX_CSV_FIELDS cap -- a
// snap-heavy desktop's 30+ loop devices previously pushed a --system
// --power --counters=topdown CSV past 128 columns, silently truncating
// wspy-plot's header parsing and dropping the topdown-detail plot entirely.
static int is_virtual_disk_device(const char *name){
  return !strncmp(name,"loop",4) || !strncmp(name,"ram",3) || !strncmp(name,"zram",4);
}

// read /proc/partitions and initialize the system_state structure for whole
// block devices (not partitions -- /sys/block/<name> only has a top-level
// entry for a whole disk, a partition like sda1 lives at
// /sys/block/sda/sda1 instead, so the access() check below naturally
// filters partitions out without needing to parse the name itself)
void setup_disk_info(void){
  FILE *fp;
  char buffer[1024];
  char path[300];
  int major,minor;
  long blocks;
  char name[32];
  int capacity = 0;
  if ((fp = fopen("/proc/partitions","r")) == NULL) return;
  while (fgets(buffer,sizeof(buffer),fp) != NULL){
    if (sscanf(buffer,"%d %d %ld %31s",&major,&minor,&blocks,name) != 4) continue;
    if (is_virtual_disk_device(name)) continue;
    snprintf(path,sizeof(path),"/sys/block/%s",name);
    if (access(path,F_OK) != 0) continue; // not a whole disk (e.g. a partition), skip
    if (system_state.num_disk >= capacity){
      int newcap = capacity ? capacity * 2 : 4;
      struct diskinfo *newlist = realloc(system_state.diskinfo,newcap * sizeof(struct diskinfo));
      if (newlist == NULL){
        fclose(fp);
        return;
      }
      if (newcap > capacity){
        memset(newlist + capacity,0,(newcap - capacity)*sizeof(struct diskinfo));
      }
      system_state.diskinfo = newlist;
      capacity = newcap;
    }
    system_state.diskinfo[system_state.num_disk].name = strdup(name);
    snprintf(system_state.diskinfo[system_state.num_disk].statfile,
             sizeof(system_state.diskinfo[system_state.num_disk].statfile),
             "/sys/block/%s/stat",name);
    debug("parsed block device: %s\n",name);
    system_state.num_disk++;
  }
  fclose(fp);
  if (system_state.num_disk == 0){
    free(system_state.diskinfo);
    system_state.diskinfo = NULL;
  }
}

// cpu ids (from /sys/devices/system/cpu/cpu<N>) that expose cpufreq's
// scaling_cur_freq, cached once since the set doesn't change mid-run
static int *freq_cpu_ids = NULL;
static int num_freq_cpus = 0;
static int freq_setup_done = 0;

// scan /sys/devices/system/cpu for cpu<N> entries with a readable
// cpufreq/scaling_cur_freq file -- readdir-driven rather than assuming
// 0..num_procs-1 are all present/online, same idiom as amd_sysfs.c's
// device scan
static void setup_freq_info(void){
  DIR *dir;
  struct dirent *entry;
  int capacity = 0;
  char path[256];
  char *endptr;
  long id;

  freq_setup_done = 1;
  if ((dir = opendir("/sys/devices/system/cpu")) == NULL) return;
  while ((entry = readdir(dir)) != NULL){
    if (strncmp(entry->d_name,"cpu",3) != 0) continue;
    id = strtol(entry->d_name+3,&endptr,10);
    if (endptr == entry->d_name+3 || *endptr != '\0') continue; // not "cpu<digits>"
    snprintf(path,sizeof(path),"/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq",id);
    if (access(path,R_OK) != 0) continue;
    if (num_freq_cpus >= capacity){
      int newcap = capacity ? capacity * 2 : 8;
      int *newlist = realloc(freq_cpu_ids,newcap * sizeof(int));
      if (newlist == NULL) break;
      freq_cpu_ids = newlist;
      capacity = newcap;
    }
    freq_cpu_ids[num_freq_cpus++] = (int) id;
  }
  closedir(dir);
}

// CPU package/die temperature via hwmon (k10temp on AMD Zen, coretemp on
// Intel, cpu_thermal on some ARM SoCs) -- a single plain sysfs read, same
// cost class and no-privileges-needed idiom as setup_freq_info() above, not
// a perf counter or RAPL PMU. readdir-driven (the hwmonN index is
// enumeration-order dependent and varies host to host, same reasoning as
// amd_sysfs.c's device scan), not a hardcoded path.
static char **temp_paths = NULL;
static int num_temp_paths = 0;
static int temp_setup_done = 0;

// hwmon driver names known to expose a CPU package/die temperature. Not
// exhaustive -- an unmatched host just yields zero CPU temp sensors, same
// "measured vs unavailable" degrade as cpufreq being absent in a VM.
static const char *TEMP_HWMON_DRIVERS[] = { "k10temp", "coretemp", "cpu_thermal", NULL };
// Preferred hwmon temp*_label text identifying the overall package/die
// sensor rather than a per-core/per-chiplet one -- AMD's Tctl is the
// control temperature (what `sensors`/HWiNFO call "CPU"), Tdie the actual
// die temp on some parts; Intel's coretemp labels the package sensor
// "Package id N". A hwmon device with none of these labeled falls back to
// its own first temp*_input rather than being skipped outright.
static const char *TEMP_PREFERRED_LABELS[] = { "Tctl", "Tdie", "Package id 0", "Package id 1", NULL };

static int temp_driver_matches(const char *name){
  int i;
  for (i = 0; TEMP_HWMON_DRIVERS[i]; i++) if (!strcmp(name,TEMP_HWMON_DRIVERS[i])) return 1;
  return 0;
}

static void add_temp_path(const char *path){
  char **newlist = realloc(temp_paths,(num_temp_paths + 1) * sizeof(char *));
  if (newlist == NULL) return;
  temp_paths = newlist;
  temp_paths[num_temp_paths++] = strdup(path);
}

static void setup_temp_info(void){
  DIR *dir,*hdir;
  struct dirent *entry,*hentry;
  char namebuf[128],labelbuf[128];
  char hwmon_dir[300],namepath[350];
  char chosen[350],fallback[350];
  FILE *fp;

  temp_setup_done = 1;
  if ((dir = opendir("/sys/class/hwmon")) == NULL) return;
  while ((entry = readdir(dir)) != NULL){
    if (entry->d_name[0] == '.') continue;
    snprintf(namepath,sizeof(namepath),"/sys/class/hwmon/%s/name",entry->d_name);
    if ((fp = fopen(namepath,"r")) == NULL) continue;
    if (fgets(namebuf,sizeof(namebuf),fp) == NULL){ fclose(fp); continue; }
    fclose(fp);
    namebuf[strcspn(namebuf,"\n")] = '\0';
    if (!temp_driver_matches(namebuf)) continue;

    snprintf(hwmon_dir,sizeof(hwmon_dir),"/sys/class/hwmon/%s",entry->d_name);
    chosen[0] = '\0';
    fallback[0] = '\0';
    if ((hdir = opendir(hwmon_dir)) == NULL) continue;
    while ((hentry = readdir(hdir)) != NULL){
      size_t len = strlen(hentry->d_name);
      char base[64];
      char *suffix;
      char inputpath[350],labelpath[350];
      if (strncmp(hentry->d_name,"temp",4) != 0) continue;
      if (len < 6 || strcmp(hentry->d_name + len - 6,"_input") != 0) continue;
      snprintf(inputpath,sizeof(inputpath),"%s/%s",hwmon_dir,hentry->d_name);
      if (fallback[0] == '\0') snprintf(fallback,sizeof(fallback),"%s",inputpath);
      snprintf(base,sizeof(base),"%s",hentry->d_name);
      suffix = strstr(base,"_input");
      if (suffix) *suffix = '\0';
      snprintf(labelpath,sizeof(labelpath),"%s/%s_label",hwmon_dir,base);
      if ((fp = fopen(labelpath,"r")) != NULL){
        if (fgets(labelbuf,sizeof(labelbuf),fp) != NULL){
          int i;
          labelbuf[strcspn(labelbuf,"\n")] = '\0';
          for (i = 0; TEMP_PREFERRED_LABELS[i]; i++){
            if (!strcmp(labelbuf,TEMP_PREFERRED_LABELS[i])){
              snprintf(chosen,sizeof(chosen),"%s",inputpath);
              break;
            }
          }
        }
        fclose(fp);
      }
      if (chosen[0]) break;
    }
    closedir(hdir);
    if (chosen[0]) add_temp_path(chosen);
    else if (fallback[0]) add_temp_path(fallback);
  }
  closedir(dir);
}

static int read_gpu_busy_percent(void) {
#if AMDGPU
  int val = amd_sysfs_gpu_busy_percent();
  if (val >= 0) return val;
#endif

  static char igpu_load_path[450] = "";
  static int igpu_searched = 0;
  if (!igpu_searched) {
    igpu_searched = 1;
    DIR *dir = opendir("/sys/class/devfreq");
    if (dir) {
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (strstr(de->d_name, "gpu") || strstr(de->d_name, "mali") ||
            strstr(de->d_name, "panfrost") || strstr(de->d_name, "kgsl") ||
            strstr(de->d_name, "agx")) {
          char filepath[450];
          snprintf(filepath, sizeof(filepath), "/sys/class/devfreq/%s/load", de->d_name);
          if (access(filepath, F_OK) == 0) {
            snprintf(igpu_load_path, sizeof(igpu_load_path), "%s", filepath);
            debug("Found iGPU devfreq load sensor at %s\n", filepath);
            break;
          }
        }
      }
      closedir(dir);
    }
  }

  if (igpu_load_path[0] != '\0') {
    FILE *fp = fopen(igpu_load_path, "r");
    if (fp) {
      char buf[64];
      if (fgets(buf, sizeof(buf), fp)) {
        int load = 0;
        if (sscanf(buf, "%d", &load) == 1) {
          fclose(fp);
          if (load < 0) return 0;
          if (load > 100) return 100;
          return load;
        }
      }
      fclose(fp);
    }
  }

  return 0;
}

// read system-wide state
void read_system(void){
  FILE *fp;
  char buffer[1024];
  char *p,*p2;
  double value1,value5,value15;
  int runnable;
  unsigned long usertime,nicetime,systemtime,idletime,iowait,irqtime,softirqtime;
  int i;
  if (system_mask & SYSTEM_LOADAVG){
    if ((fp = fopen("/proc/loadavg","r"))){
      if (fscanf(fp,"%lf %lf %lf %d",&value1,&value5,&value15,&runnable) == 4){
        system_state.load = value1;
        system_state.runnable = runnable;
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_CPU){
    if ((fp = fopen("/proc/stat","r"))){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
        if (!strncmp(buffer,"cpu ",4)){
          if (sscanf(&buffer[4],"%lu %lu %lu %lu %lu %lu %lu",
                     &usertime,&nicetime,&systemtime,&idletime,&iowait,&irqtime,&softirqtime) == 7){
            system_state.cpu.prev_usertime = system_state.cpu.last_usertime;
            system_state.cpu.prev_systemtime = system_state.cpu.last_systemtime;
            system_state.cpu.prev_idletime = system_state.cpu.last_idletime;
            system_state.cpu.prev_iowaittime = system_state.cpu.last_iowaittime;
            system_state.cpu.prev_irqtime = system_state.cpu.last_irqtime;
            system_state.cpu.last_usertime = usertime;
            system_state.cpu.last_systemtime = systemtime;
            system_state.cpu.last_idletime = idletime;
            system_state.cpu.last_iowaittime = iowait;
            system_state.cpu.last_irqtime = irqtime + softirqtime;
            system_state.cpu.usertime = system_state.cpu.last_usertime - system_state.cpu.prev_usertime;
            system_state.cpu.systemtime = system_state.cpu.last_systemtime - system_state.cpu.prev_systemtime;
            system_state.cpu.idletime = system_state.cpu.last_idletime - system_state.cpu.prev_idletime;
            system_state.cpu.iowaittime = system_state.cpu.last_iowaittime - system_state.cpu.prev_iowaittime;
            system_state.cpu.irqtime = system_state.cpu.last_irqtime - system_state.cpu.prev_irqtime;
            break;
          }
        }
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_NETWORK){
    unsigned long int field1,field2,field3,field4,field5,field6,field7,field8,field9;
    if (system_state.netinfo == NULL) setup_net_info();
    if ((fp = fopen("/proc/net/dev","r"))){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
        p = strchr(buffer,':');
        if (p == NULL) continue;
        *p = 0;
        p++;
        p2 = buffer;
        while (isspace(*p2)) p2++;
        for (i=0;i<system_state.num_net;i++){
          if (system_state.netinfo[i].name && !strcmp(p2,system_state.netinfo[i].name)){
            if (sscanf(p,"%lu %lu %lu %lu %lu %lu %lu %lu %lu",
                       &field1,&field2,&field3,&field4,&field5,&field6,&field7,&field8,&field9) == 9){
              system_state.netinfo[i].prev_bytes = system_state.netinfo[i].last_bytes;
              system_state.netinfo[i].last_bytes = field1 + field9;
              system_state.netinfo[i].bytes = system_state.netinfo[i].last_bytes - system_state.netinfo[i].prev_bytes;
              continue;
            }
          }
        }
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_DISK){
    unsigned long int reads_completed,reads_merged,sectors_read,ms_reading;
    unsigned long int writes_completed,writes_merged,sectors_written,ms_writing;
    unsigned long int ios_in_progress,ms_doing_io,weighted_ms_doing_io;
    if (system_state.diskinfo == NULL) setup_disk_info();
    for (i=0;i<system_state.num_disk;i++){
      if ((fp = fopen(system_state.diskinfo[i].statfile,"r")) == NULL) continue;
      if (fgets(buffer,sizeof(buffer),fp) != NULL){
        if (sscanf(buffer,"%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu %lu",
                   &reads_completed,&reads_merged,&sectors_read,&ms_reading,
                   &writes_completed,&writes_merged,&sectors_written,&ms_writing,
                   &ios_in_progress,&ms_doing_io,&weighted_ms_doing_io) == 11){
          system_state.diskinfo[i].prev_read_sectors = system_state.diskinfo[i].last_read_sectors;
          system_state.diskinfo[i].prev_write_sectors = system_state.diskinfo[i].last_write_sectors;
          system_state.diskinfo[i].prev_io_ms = system_state.diskinfo[i].last_io_ms;
          system_state.diskinfo[i].last_read_sectors = sectors_read;
          system_state.diskinfo[i].last_write_sectors = sectors_written;
          system_state.diskinfo[i].last_io_ms = ms_doing_io;
          system_state.diskinfo[i].read_sectors =
            system_state.diskinfo[i].last_read_sectors - system_state.diskinfo[i].prev_read_sectors;
          system_state.diskinfo[i].write_sectors =
            system_state.diskinfo[i].last_write_sectors - system_state.diskinfo[i].prev_write_sectors;
          system_state.diskinfo[i].io_ms =
            system_state.diskinfo[i].last_io_ms - system_state.diskinfo[i].prev_io_ms;
        }
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_MEM){
    if ((fp = fopen("/proc/meminfo","r"))){
      while (fgets(buffer,sizeof(buffer),fp) != NULL){
        // strncmp anchored at line start -- "Cached:"/"Writeback:" can't
        // false-match "SwapCached:"/"WritebackTmp:" this way, since the
        // byte right after the matched prefix differs ('S'/'T' vs ':')
        if (!strncmp(buffer,"MemFree:",8)) sscanf(buffer+8,"%lu",&system_state.mem.free_kb);
        else if (!strncmp(buffer,"Cached:",7)) sscanf(buffer+7,"%lu",&system_state.mem.cached_kb);
        else if (!strncmp(buffer,"Dirty:",6)) sscanf(buffer+6,"%lu",&system_state.mem.dirty_kb);
        else if (!strncmp(buffer,"Writeback:",10)) sscanf(buffer+10,"%lu",&system_state.mem.writeback_kb);
        else if (!strncmp(buffer,"SwapFree:",9)) sscanf(buffer+9,"%lu",&system_state.mem.swap_free_kb);
        else if (!strncmp(buffer,"Committed_AS:",13)) sscanf(buffer+13,"%lu",&system_state.mem.committed_as_kb);
      }
      fclose(fp);
    }
  }
  if (system_mask & SYSTEM_GPU){
    system_state.gpu.prev_busy_percent = system_state.gpu.last_busy_percent;
    system_state.gpu.last_busy_percent = read_gpu_busy_percent();
    system_state.gpu.busy_percent = system_state.gpu.last_busy_percent;
  }
  if (system_mask & SYSTEM_FREQ){
    unsigned long khz;
    double sum = 0;
    int count = 0;
    if (!freq_setup_done) setup_freq_info();
    for (i=0;i<num_freq_cpus;i++){
      snprintf(buffer,sizeof(buffer),"/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq",freq_cpu_ids[i]);
      if ((fp = fopen(buffer,"r"))){
        if (fscanf(fp,"%lu",&khz) == 1){
          sum += khz;
          count++;
        }
        fclose(fp);
      }
    }
    // cpufreq unavailable (e.g. some VMs/containers) degrades to 0 rather
    // than failing, same "measured vs unavailable" idiom used throughout
    system_state.freq_mhz = count ? (sum / count) / 1000.0 : 0.0;
  }
  if (system_mask & SYSTEM_TEMP){
    long millideg;
    double sum = 0;
    int count = 0;
    if (!temp_setup_done) setup_temp_info();
    for (i=0;i<num_temp_paths;i++){
      if ((fp = fopen(temp_paths[i],"r"))){
        if (fscanf(fp,"%ld",&millideg) == 1){
          sum += millideg / 1000.0;
          count++;
        }
        fclose(fp);
      }
    }
    // no CPU temp sensor discovered (e.g. a VM/container, or an
    // unrecognized hwmon driver) degrades to 0, same idiom as cpufreq above
    system_state.cpu_temp_c = count ? sum / count : 0.0;
  }
}

void print_system(enum output_format oformat){
  double elapsed;
  int i;
  if (interval) elapsed = interval;
  else {
    clock_gettime(CLOCK_REALTIME,&finish_time);
    elapsed = finish_time.tv_sec + finish_time.tv_nsec / 1000000000.0 -
      start_time.tv_sec - start_time.tv_nsec / 1000000000.0;    
  }

  switch(oformat){
  case PRINT_CSV_HEADER:
    if (system_mask & SYSTEM_LOADAVG) fprintf(outfile,"load,runnable,");
    if (system_mask & SYSTEM_CPU) fprintf(outfile,"cpu,idle,iowait,irq,");
    if (system_mask & SYSTEM_FREQ) fprintf(outfile,"freq,");
    if (system_mask & SYSTEM_TEMP) fprintf(outfile,"cpu_temp,");
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++) fprintf(outfile,"net %s,",system_state.netinfo[i].name);
    }
    if (system_mask & SYSTEM_DISK){
      if (system_state.diskinfo == NULL) setup_disk_info();
      for (i=0;i<system_state.num_disk;i++)
        fprintf(outfile,"disk %s read,disk %s write,disk %s time,",
                system_state.diskinfo[i].name,system_state.diskinfo[i].name,system_state.diskinfo[i].name);
    }
    if (system_mask & SYSTEM_MEM)
      fprintf(outfile,"mem_free_mb,mem_cached_mb,mem_dirty_mb,mem_writeback_mb,swap_free_mb,committed_as_mb,");
    if (system_mask & SYSTEM_GPU) fprintf(outfile,"gpu_busy,");
    break;
  case PRINT_CSV:
    if (system_mask & SYSTEM_LOADAVG) fprintf(outfile,"%4.2f,%d,",system_state.load,system_state.runnable);
    if (system_mask & SYSTEM_CPU){
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.usertime+system_state.cpu.systemtime)/elapsed/num_procs);
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.idletime)/elapsed/num_procs);
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.iowaittime)/elapsed/num_procs);      
      fprintf(outfile,"%3.2f%%,",
	      (double) (system_state.cpu.irqtime)/elapsed/num_procs);
    }
    if (system_mask & SYSTEM_FREQ) fprintf(outfile,"%4.0f,",system_state.freq_mhz);
    if (system_mask & SYSTEM_TEMP) fprintf(outfile,"%4.1f,",system_state.cpu_temp_c);
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%lu,",system_state.netinfo[i].bytes);
      }
    }
    if (system_mask & SYSTEM_DISK){
      if (system_state.diskinfo == NULL) setup_disk_info();
      for (i=0;i<system_state.num_disk;i++){
        // sectors are always 512-byte units regardless of the device's own
        // logical block size (see linux/Documentation/ABI/.../sysfs-block)
        fprintf(outfile,"%lu,%lu,%lu,",
                system_state.diskinfo[i].read_sectors * 512,
                system_state.diskinfo[i].write_sectors * 512,
                system_state.diskinfo[i].io_ms);
      }
    }
    if (system_mask & SYSTEM_MEM){
      fprintf(outfile,"%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,",
              system_state.mem.free_kb / 1024.0,
              system_state.mem.cached_kb / 1024.0,
              system_state.mem.dirty_kb / 1024.0,
              system_state.mem.writeback_kb / 1024.0,
              system_state.mem.swap_free_kb / 1024.0,
              system_state.mem.committed_as_kb / 1024.0);
    }
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"%d,",system_state.gpu.busy_percent);
    }
    break;
  case PRINT_NORMAL:
    if (system_mask & SYSTEM_LOADAVG){
      fprintf(outfile,"load                 %4.2f\n",system_state.load);
      fprintf(outfile,"runnable             %d\n",system_state.runnable);      
    }
    if (system_mask & SYSTEM_CPU){
      fprintf(outfile,"cpu                  %3.2f%%\n",
	      (double) (system_state.cpu.usertime+system_state.cpu.systemtime)/elapsed/num_procs);
      fprintf(outfile,"idle                 %3.2f%%\n",
	      (double) (system_state.cpu.idletime/elapsed/num_procs));
      fprintf(outfile,"io                   %3.2f%%\n",
	      (double) (system_state.cpu.iowaittime/elapsed/num_procs));      
      fprintf(outfile,"irq                  %3.2f%%\n",
	      (double) (system_state.cpu.irqtime)/elapsed/num_procs);
    }
    if (system_mask & SYSTEM_FREQ){
      fprintf(outfile,"freq                 %4.0f MHz\n",system_state.freq_mhz);
    }
    if (system_mask & SYSTEM_TEMP){
      fprintf(outfile,"cpu temp             %4.1f C\n",system_state.cpu_temp_c);
    }
    if (system_mask & SYSTEM_NETWORK){
      if (system_state.netinfo == NULL) setup_net_info();
      for (i=0;i<system_state.num_net;i++){
	fprintf(outfile,"%-14s       %lu\n",system_state.netinfo[i].name,system_state.netinfo[i].bytes);
      }
    }
    if (system_mask & SYSTEM_DISK){
      if (system_state.diskinfo == NULL) setup_disk_info();
      for (i=0;i<system_state.num_disk;i++){
        fprintf(outfile,"disk %-10s      read %10lu  write %10lu  time %8lu ms\n",
                system_state.diskinfo[i].name,
                system_state.diskinfo[i].read_sectors * 512,
                system_state.diskinfo[i].write_sectors * 512,
                system_state.diskinfo[i].io_ms);
      }
    }
    if (system_mask & SYSTEM_MEM){
      fprintf(outfile,"mem free             %8.1f MB\n",system_state.mem.free_kb / 1024.0);
      fprintf(outfile,"mem cached           %8.1f MB\n",system_state.mem.cached_kb / 1024.0);
      fprintf(outfile,"mem dirty            %8.1f MB\n",system_state.mem.dirty_kb / 1024.0);
      fprintf(outfile,"mem writeback        %8.1f MB\n",system_state.mem.writeback_kb / 1024.0);
      fprintf(outfile,"swap free            %8.1f MB\n",system_state.mem.swap_free_kb / 1024.0);
      fprintf(outfile,"committed            %8.1f MB\n",system_state.mem.committed_as_kb / 1024.0);
    }
    if (system_mask & SYSTEM_GPU){
      fprintf(outfile,"gpu busy             %d%%\n",system_state.gpu.busy_percent);
    }
  }
}
