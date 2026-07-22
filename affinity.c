/*
 * affinity.c - core/thread affinity control, described in affinity.h
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "affinity.h"
#include "wspy.h"
#include "error.h"

struct affinity_topology affinity_topology;

const char *affinity_mode_name(enum affinity_mode mode){
  switch (mode){
  case AFFINITY_ALL: return "all";
  case AFFINITY_THREAD: return "thread";
  case AFFINITY_NOSMT: return "nosmt";
  case AFFINITY_DOMAIN: return "domain";
  case AFFINITY_CPUSET: return "cpuset";
  case AFFINITY_CORETYPE: return "coretype";
  default: return "unknown";
  }
}

static int read_line_file(const char *path,char *buf,size_t bufsize){
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
  while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r' || buf[len-1] == ' ')){
    buf[--len] = '\0';
  }
  return 0;
}

static int read_int_file(const char *path,int *value){
  char buf[32];

  if (read_line_file(path,buf,sizeof(buf)) != 0) return -1;
  if (sscanf(buf,"%d",value) != 1) return -1;
  return 0;
}

/* Parses a sysfs-style cpu range list ("0,2-3,7") into set, ignoring/
 * clamping anything outside 0..ncpus-1 or CPU_SETSIZE. A NULL/empty/
 * unparsable list just leaves set with whatever it already had (usually
 * zeroed by the caller) -- callers degrade a fully-empty result themselves
 * (see discover_topology_at()/affinity_parse_spec()). */
static void parse_cpu_list_into_set(const char *list,cpu_set_t *set,int ncpus){
  const char *p = list;

  if (!p) return;
  while (*p){
    char *endptr;
    long low = strtol(p,&endptr,10);
    long high = low;

    if (endptr == p) break;
    p = endptr;
    if (*p == '-'){
      p++;
      high = strtol(p,&endptr,10);
      if (endptr == p) break;
      p = endptr;
    }
    if (low < 0) low = 0;
    if (high >= ncpus) high = ncpus - 1;
    while (low <= high){
      if (low < CPU_SETSIZE) CPU_SET((int)low,set);
      low++;
    }
    if (*p == ',') p++;
    while (*p == ' ' || *p == '\t') p++;
  }
}

/* Parses a sysfs cache "size" file value (e.g. "16384K", "16M") into bytes;
 * a bare number with no suffix is already bytes. Returns 0 if unparsable. */
static unsigned long parse_cache_size_bytes(const char *s){
  char *endptr;
  double val;

  val = strtod(s,&endptr);
  if (endptr == s) return 0;
  while (*endptr == ' ') endptr++;
  switch (*endptr){
  case 'K': case 'k': return (unsigned long)(val * 1024.0);
  case 'M': case 'm': return (unsigned long)(val * 1024.0 * 1024.0);
  case 'G': case 'g': return (unsigned long)(val * 1024.0 * 1024.0 * 1024.0);
  default: return (unsigned long)val;
  }
}

void affinity_topology_free(void){
  free(affinity_topology.cpu);
  free(affinity_topology.l3domains);
  free(affinity_topology.coretypes);
  memset(&affinity_topology,0,sizeof(affinity_topology));
}

/* Parameterized by sysfs_base so tests can point it at a fake directory
 * tree (mirrors ibs.c's ibs_probe_at()), rather than depending on real
 * /sys/devices/system/cpu contents. Never fails: a CPU whose topology/cache
 * files are missing or unreadable just gets degraded fields (core_id=-1,
 * package_id=-1, l3_domain=-1, treated as its own singleton SMT group). */
static void affinity_topology_discover_at(const char *sysfs_base,int ncpus){
  int i,k;

  affinity_topology_free();
  affinity_topology.ncpus = ncpus;
  affinity_topology.cpu = calloc(ncpus > 0 ? ncpus : 1,sizeof(struct affinity_cpu_info));

  for (i=0;i<ncpus;i++){
    affinity_topology.cpu[i].core_id = -1;
    affinity_topology.cpu[i].package_id = -1;
    affinity_topology.cpu[i].is_primary_thread = 1;
    affinity_topology.cpu[i].l3_domain = -1;
    affinity_topology.cpu[i].core_type = -1;
  }

  for (i=0;i<ncpus;i++){
    char path[512];
    char list[256];
    cpu_set_t sibset;
    int val,lowest;

    snprintf(path,sizeof(path),"%s/cpu%d/topology/core_id",sysfs_base,i);
    if (read_int_file(path,&val) == 0) affinity_topology.cpu[i].core_id = val;

    snprintf(path,sizeof(path),"%s/cpu%d/topology/physical_package_id",sysfs_base,i);
    if (read_int_file(path,&val) == 0) affinity_topology.cpu[i].package_id = val;

    snprintf(path,sizeof(path),"%s/cpu%d/topology/thread_siblings_list",sysfs_base,i);
    if (read_line_file(path,list,sizeof(list)) != 0){
      snprintf(path,sizeof(path),"%s/cpu%d/topology/core_cpus_list",sysfs_base,i);
      if (read_line_file(path,list,sizeof(list)) != 0) list[0] = '\0';
    }
    CPU_ZERO(&sibset);
    parse_cpu_list_into_set(list,&sibset,ncpus);
    if (CPU_COUNT(&sibset) == 0) CPU_SET(i,&sibset);

    lowest = i;
    for (k=0;k<ncpus;k++){
      if (CPU_ISSET(k,&sibset) && k < lowest) lowest = k;
    }
    affinity_topology.cpu[i].is_primary_thread = (i == lowest);
  }

  /* L3 domains: single ascending pass -- the first cpu encountered in each
   * domain reads shared_cpu_list/size and creates the domain entry, then
   * every other member (including lower array slots we haven't reached yet)
   * is stamped with that domain id so it's never rediscovered. */
  for (i=0;i<ncpus;i++){
    int idx;

    if (affinity_topology.cpu[i].l3_domain != -1) continue;

    for (idx=0;idx<10;idx++){
      char level_path[512];
      int level;

      snprintf(level_path,sizeof(level_path),"%s/cpu%d/cache/index%d/level",sysfs_base,i,idx);
      if (read_int_file(level_path,&level) != 0) continue;
      if (level != 3) continue;
      {
        char shared_path[512],shared_list[256];
        char size_path[512],size_str[32];
        cpu_set_t domset;
        unsigned long size_bytes = 0;
        int dom_id,m;

        snprintf(shared_path,sizeof(shared_path),"%s/cpu%d/cache/index%d/shared_cpu_list",sysfs_base,i,idx);
        if (read_line_file(shared_path,shared_list,sizeof(shared_list)) != 0) shared_list[0] = '\0';
        CPU_ZERO(&domset);
        parse_cpu_list_into_set(shared_list,&domset,ncpus);
        if (CPU_COUNT(&domset) == 0) CPU_SET(i,&domset);

        snprintf(size_path,sizeof(size_path),"%s/cpu%d/cache/index%d/size",sysfs_base,i,idx);
        if (read_line_file(size_path,size_str,sizeof(size_str)) == 0){
          size_bytes = parse_cache_size_bytes(size_str);
        }

        dom_id = affinity_topology.nl3domains;
        affinity_topology.l3domains = realloc(affinity_topology.l3domains,
          (dom_id+1)*sizeof(struct affinity_l3_domain));
        affinity_topology.l3domains[dom_id].cpus = domset;
        affinity_topology.l3domains[dom_id].size_bytes = size_bytes;
        affinity_topology.nl3domains = dom_id+1;

        for (m=0;m<ncpus;m++){
          if (CPU_ISSET(m,&domset)) affinity_topology.cpu[m].l3_domain = dom_id;
        }
      }
      break;
    }
  }

  /* Core-type grouping (coretype=<id>, ARM only -- see affinity.h's header
   * comment): unlike SMT siblings/L3 domains, there's no single sysfs file
   * listing "every other cpu of this same microarchitecture", so this
   * clusters by equality of (implementer,part) read from each cpu's own
   * MIDR_EL1 register instead of following a shared list. A host with no
   * midr_el1 at all (x86, or any cpu whose file can't be read) just leaves
   * that cpu's core_type at -1 and never creates a coretypes[] entry for
   * it -- ncoretypes stays 0 on such a host, so coretype=<id> degrades to a
   * clear "no such core type" resolve error rather than silently doing
   * nothing. variant/revision bits are deliberately masked off: those are
   * silicon steppings within one microarchitecture (e.g. r0p1 vs r0p2 of
   * the same Cortex-A720), not a different core type. */
  for (i=0;i<ncpus;i++){
    char path[512];
    char hexbuf[32];
    unsigned long midr;
    unsigned int implementer,part;
    int j,found;

    snprintf(path,sizeof(path),"%s/cpu%d/regs/identification/midr_el1",sysfs_base,i);
    if (read_line_file(path,hexbuf,sizeof(hexbuf)) != 0) continue;

    midr = strtoul(hexbuf,NULL,16);
    implementer = (midr >> 24) & 0xff;
    part = (midr >> 4) & 0xfff;

    found = -1;
    for (j=0;j<affinity_topology.ncoretypes;j++){
      if (affinity_topology.coretypes[j].implementer == implementer &&
	  affinity_topology.coretypes[j].part == part){
	found = j;
	break;
      }
    }
    if (found == -1){
      found = affinity_topology.ncoretypes;
      affinity_topology.coretypes = realloc(affinity_topology.coretypes,
	(found+1)*sizeof(struct affinity_core_type));
      affinity_topology.coretypes[found].implementer = implementer;
      affinity_topology.coretypes[found].part = part;
      CPU_ZERO(&affinity_topology.coretypes[found].cpus);
      affinity_topology.ncoretypes = found+1;
    }
    affinity_topology.coretypes[found].is_midr = 1;
    CPU_SET(i,&affinity_topology.coretypes[found].cpus);
    affinity_topology.cpu[i].core_type = found;
  }

  /* x86 hybrid fallback (Intel P-core/E-core, AMD Zen5/Zen5c dense cores):
   * the MIDR loop above finds nothing on x86 (no midr_el1 register), but
   * cpu_info.c's inventory_cpu() has *already* classified each logical
   * cpu's microarchitecture into coreinfo[i].vendor while building
   * cpu_info (Intel via /sys/devices/cpu_atom|cpu_core/cpus, AMD Zen5c via
   * resolve_amd_zen5_dense_cores()'s frequency heuristic) -- reuse that
   * classification instead of re-deriving it here. Only kept when the host
   * is genuinely heterogeneous (>1 distinct classified vendor present): a
   * uniform x86 host must still report ncoretypes == 0, the same "no such
   * thing" answer every homogeneous host already gave before this
   * fallback existed. */
  if (affinity_topology.ncoretypes == 0 && cpu_info && cpu_info->coreinfo){
    for (i=0;i<ncpus && i<(int)cpu_info->num_cores;i++){
      enum cpu_core_type v = cpu_info->coreinfo[i].vendor;
      const char *name;
      int j,found;

      switch (v){
      case CORE_INTEL_ATOM: name = "intel_atom"; break;
      case CORE_INTEL_CORE: name = "intel_core"; break;
      case CORE_AMD_ZEN5: name = "amd_zen5"; break;
      case CORE_AMD_ZEN5C: name = "amd_zen5c"; break;
      default: continue; /* CORE_UNKNOWN/CORE_*_UNKNOWN/ARM types: not a meaningful grouping key here */
      }

      found = -1;
      for (j=0;j<affinity_topology.ncoretypes;j++){
        if (!strcmp(affinity_topology.coretypes[j].vendor_name,name)){ found = j; break; }
      }
      if (found == -1){
        found = affinity_topology.ncoretypes;
        affinity_topology.coretypes = realloc(affinity_topology.coretypes,
          (found+1)*sizeof(struct affinity_core_type));
        memset(&affinity_topology.coretypes[found],0,sizeof(struct affinity_core_type));
        snprintf(affinity_topology.coretypes[found].vendor_name,
          sizeof(affinity_topology.coretypes[found].vendor_name),"%s",name);
        CPU_ZERO(&affinity_topology.coretypes[found].cpus);
        affinity_topology.ncoretypes = found+1;
      }
      CPU_SET(i,&affinity_topology.coretypes[found].cpus);
      affinity_topology.cpu[i].core_type = found;
    }
    if (affinity_topology.ncoretypes == 1){
      /* Only one distinct vendor actually present -- not heterogeneous,
       * so undo it rather than report a single meaningless "core type 0". */
      free(affinity_topology.coretypes);
      affinity_topology.coretypes = NULL;
      affinity_topology.ncoretypes = 0;
      for (i=0;i<ncpus;i++) affinity_topology.cpu[i].core_type = -1;
    }
  }
}

void affinity_topology_discover(void){
  affinity_topology_discover_at("/sys/devices/system/cpu",cpu_info->num_cores);
}

int affinity_parse_spec(const char *arg,struct affinity_spec *spec){
  struct affinity_spec tmp;

  memset(&tmp,0,sizeof(tmp));
  CPU_ZERO(&tmp.set);

  if (!arg || !*arg) return -1;

  if (!strcmp(arg,"all")){
    tmp.mode = AFFINITY_ALL;
  } else if (!strcmp(arg,"nosmt")){
    tmp.mode = AFFINITY_NOSMT;
  } else if (!strncmp(arg,"thread=",7)){
    char *endptr;
    long id = strtol(arg+7,&endptr,10);
    if (endptr == arg+7 || *endptr || id < 0) return -1;
    tmp.mode = AFFINITY_THREAD;
    tmp.id = (int)id;
  } else if (!strncmp(arg,"domain=",7)){
    char *endptr;
    long id = strtol(arg+7,&endptr,10);
    if (endptr == arg+7 || *endptr || id < 0) return -1;
    tmp.mode = AFFINITY_DOMAIN;
    tmp.id = (int)id;
  } else if (!strncmp(arg,"coretype=",9)){
    char *endptr;
    long id = strtol(arg+9,&endptr,10);
    if (endptr == arg+9 || *endptr || id < 0) return -1;
    tmp.mode = AFFINITY_CORETYPE;
    tmp.id = (int)id;
  } else if (!strncmp(arg,"cpuset=",7)){
    tmp.mode = AFFINITY_CPUSET;
    parse_cpu_list_into_set(arg+7,&tmp.set,CPU_SETSIZE);
    if (CPU_COUNT(&tmp.set) == 0) return -1;
  } else {
    return -1;
  }

  *spec = tmp;
  return 0;
}

int affinity_resolve(struct affinity_spec *spec){
  cpu_set_t available,requested,result;
  int i,ncpus = cpu_info->num_cores;

  CPU_ZERO(&available);
  for (i=0;i<ncpus;i++){
    if (cpu_info->coreinfo[i].is_available) CPU_SET(i,&available);
  }

  CPU_ZERO(&requested);
  switch (spec->mode){
  case AFFINITY_ALL:
    requested = available;
    break;
  case AFFINITY_NOSMT:
    for (i=0;i<ncpus;i++){
      if (i < affinity_topology.ncpus && affinity_topology.cpu[i].is_primary_thread) CPU_SET(i,&requested);
    }
    break;
  case AFFINITY_THREAD:
    if (spec->id < 0 || spec->id >= ncpus){
      error("--affinity=thread=%d: no such CPU (valid range 0..%d)\n",spec->id,ncpus-1);
      return -1;
    }
    CPU_SET(spec->id,&requested);
    break;
  case AFFINITY_DOMAIN:
    if (spec->id < 0 || spec->id >= affinity_topology.nl3domains){
      error("--affinity=domain=%d: no such L3 domain (valid range 0..%d) -- see --list-affinity\n",
	    spec->id,affinity_topology.nl3domains-1);
      return -1;
    }
    requested = affinity_topology.l3domains[spec->id].cpus;
    break;
  case AFFINITY_CORETYPE:
    if (spec->id < 0 || spec->id >= affinity_topology.ncoretypes){
      error("--affinity=coretype=%d: no such core type (valid range 0..%d) -- see --list-affinity\n",
	    spec->id,affinity_topology.ncoretypes-1);
      return -1;
    }
    requested = affinity_topology.coretypes[spec->id].cpus;
    break;
  case AFFINITY_CPUSET:
    requested = spec->set;
    break;
  default:
    return -1;
  }

  CPU_ZERO(&result);
  for (i=0;i<ncpus;i++){
    if (CPU_ISSET(i,&requested) && CPU_ISSET(i,&available)) CPU_SET(i,&result);
  }

  if (CPU_COUNT(&result) == 0){
    error("--affinity=%s: no requested CPU is available to this process\n",affinity_mode_name(spec->mode));
    return -1;
  }
  if (CPU_COUNT(&result) < CPU_COUNT(&requested)){
    char buf[256];
    affinity_format_cpu_set(&result,ncpus,buf,sizeof(buf));
    warning("--affinity=%s: some requested CPUs are not available to this process, using %s\n",
	    affinity_mode_name(spec->mode),buf);
  }

  spec->set = result;
  return 0;
}

void affinity_format_cpu_set(const cpu_set_t *set,int ncpus,char *buf,size_t bufsize){
  int i,first = 1;
  size_t used = 0;

  if (bufsize == 0) return;
  buf[0] = '\0';
  for (i=0;i<ncpus;i++){
    int run_start,run_end,n;

    if (!CPU_ISSET(i,set)) continue;
    run_start = i;
    run_end = i;
    while (run_end+1 < ncpus && CPU_ISSET(run_end+1,set)) run_end++;

    if (run_start == run_end){
      n = snprintf(buf+used,bufsize-used,"%s%d",first ? "" : ",",run_start);
    } else {
      n = snprintf(buf+used,bufsize-used,"%s%d-%d",first ? "" : ",",run_start,run_end);
    }
    if (n < 0 || (size_t)n >= bufsize-used){
      if (bufsize > used+4) snprintf(buf+used,bufsize-used,"...");
      return;
    }
    used += (size_t)n;
    first = 0;
    i = run_end;
  }
}

void affinity_print_report(FILE *fp){
  int i;

  fprintf(fp,"affinity topology: %d cpu(s), %d L3 domain(s), %d core type(s) "
	  "(pass domain=<id>/coretype=<id> to --affinity)\n",
	  affinity_topology.ncpus,affinity_topology.nl3domains,affinity_topology.ncoretypes);
  fprintf(fp,"cpu,core_id,package_id,primary_thread,l3_domain,core_type\n");
  for (i=0;i<affinity_topology.ncpus;i++){
    struct affinity_cpu_info *c = &affinity_topology.cpu[i];
    fprintf(fp,"%d,%d,%d,%d,%d,%d\n",i,c->core_id,c->package_id,c->is_primary_thread,c->l3_domain,c->core_type);
  }
  for (i=0;i<affinity_topology.nl3domains;i++){
    char buf[512];
    affinity_format_cpu_set(&affinity_topology.l3domains[i].cpus,affinity_topology.ncpus,buf,sizeof(buf));
    fprintf(fp,"l3domain %d: cpus %s (%.1f MiB)\n",i,buf,
	    affinity_topology.l3domains[i].size_bytes / (1024.0*1024.0));
  }
  /* implementer/part are MIDR_EL1's raw hex fields, not decoded into a
   * vendor/model name -- see scripts/map_cpu_hierarchy.py for that. x86
   * hybrid entries (is_midr==0) have no MIDR fields at all, so they print
   * a vendor_name string instead -- see affinity.h's struct comment. */
  for (i=0;i<affinity_topology.ncoretypes;i++){
    char buf[512];
    affinity_format_cpu_set(&affinity_topology.coretypes[i].cpus,affinity_topology.ncpus,buf,sizeof(buf));
    if (affinity_topology.coretypes[i].is_midr){
      fprintf(fp,"coretype %d: implementer=0x%02x part=0x%03x cpus %s\n",i,
	      affinity_topology.coretypes[i].implementer,affinity_topology.coretypes[i].part,buf);
    } else {
      fprintf(fp,"coretype %d: vendor=%s cpus %s\n",i,
	      affinity_topology.coretypes[i].vendor_name,buf);
    }
  }
}
