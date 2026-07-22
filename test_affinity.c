/*
 * test_affinity.c - unit tests for affinity.c's core/thread affinity
 * topology discovery + --affinity=<spec> parsing/resolution.
 *
 * _GNU_SOURCE must come before the first system header in this translation
 * unit (glibc's feature-test decisions -- here, cpu_set_t/CPU_SET() and
 * friends -- are locked in at that point), so it's defined here rather than
 * left to affinity.c's own #define, which by this point would be too late.
 *
 * Follows test_ibs.c's pattern: #include the module directly (affinity.c
 * has no main() to stub) and build a fake sysfs directory tree so
 * affinity_topology_discover_at() (the testable, sysfs_base-parameterized
 * half of affinity_topology_discover(), mirroring ibs.c's ibs_probe_at())
 * can be exercised without depending on real hardware topology. cpu_info is
 * a real global (declared extern by cpu_info.h, pulled in via affinity.c's
 * own "wspy.h" include) that each test populates by hand, mirroring how a
 * real run's inventory_cpu() would.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>

#include "affinity.c"

struct cpu_info *cpu_info;

#define FAKE_SYSFS_BASE "/tmp/test_affinity_sysfs"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void mkdir_p(const char *path){
  char tmp[512];
  char *p;

  snprintf(tmp,sizeof(tmp),"%s",path);
  for (p = tmp+1; *p; p++){
    if (*p == '/'){
      *p = '\0';
      mkdir(tmp,0755);
      *p = '/';
    }
  }
  mkdir(tmp,0755);
}

static void rmdir_recursive(const char *path){
  char cmd[600];
  snprintf(cmd,sizeof(cmd),"rm -rf '%s'",path);
  system(cmd);
}

/* Writes cpu<n>'s topology/{core_id,physical_package_id,thread_siblings_list}
 * and, when level3_shared_list is non-NULL, cache/index3/{level,shared_cpu_list,size}. */
static void make_fake_cpu(int n,int core_id,int package_id,const char *siblings_list,
			   const char *level3_shared_list,const char *level3_size){
  char path[512],dir[512];

  snprintf(dir,sizeof(dir),"%s/cpu%d/topology",FAKE_SYSFS_BASE,n);
  mkdir_p(dir);

  snprintf(path,sizeof(path),"%s/core_id",dir);
  { char buf[16]; snprintf(buf,sizeof(buf),"%d\n",core_id); write_file(path,buf); }
  snprintf(path,sizeof(path),"%s/physical_package_id",dir);
  { char buf[16]; snprintf(buf,sizeof(buf),"%d\n",package_id); write_file(path,buf); }
  snprintf(path,sizeof(path),"%s/thread_siblings_list",dir);
  { char buf[64]; snprintf(buf,sizeof(buf),"%s\n",siblings_list); write_file(path,buf); }

  if (level3_shared_list){
    snprintf(dir,sizeof(dir),"%s/cpu%d/cache/index3",FAKE_SYSFS_BASE,n);
    mkdir_p(dir);
    snprintf(path,sizeof(path),"%s/level",dir);
    write_file(path,"3\n");
    snprintf(path,sizeof(path),"%s/shared_cpu_list",dir);
    { char buf[64]; snprintf(buf,sizeof(buf),"%s\n",level3_shared_list); write_file(path,buf); }
    snprintf(path,sizeof(path),"%s/size",dir);
    { char buf[32]; snprintf(buf,sizeof(buf),"%s\n",level3_size); write_file(path,buf); }
  }
}

/* Builds an 8-cpu fake topology: 4 cores (SMT pairs {0,4},{1,5},{2,6},{3,7}),
 * two L3 domains -- A = {0,1,4,5} (16384K), B = {2,3,6,7} (8192K) -- mirroring
 * this codebase's own real Zen5/Zen5c CCD split (16 MiB vs 8 MiB L3) at a
 * small, readable scale. */
static void build_fake_topology(void){
  rmdir_recursive(FAKE_SYSFS_BASE);
  make_fake_cpu(0,0,0,"0,4","0,1,4,5","16384K");
  make_fake_cpu(1,1,0,"1,5","0,1,4,5","16384K");
  make_fake_cpu(2,2,0,"2,6","2,3,6,7","8192K");
  make_fake_cpu(3,3,0,"3,7","2,3,6,7","8192K");
  make_fake_cpu(4,0,0,"0,4","0,1,4,5","16384K");
  make_fake_cpu(5,1,0,"1,5","0,1,4,5","16384K");
  make_fake_cpu(6,2,0,"2,6","2,3,6,7","8192K");
  make_fake_cpu(7,3,0,"3,7","2,3,6,7","8192K");
}

/* Populates the real cpu_info global with num_cores cpus, all (or
 * selectively) marked is_available -- affinity_resolve() reads this to
 * decide what's actually schedulable from this process, mirroring
 * inventory_cpu()'s own sched_getaffinity()-derived is_available. */
static void build_fake_cpu_info(int num_cores){
  int i;

  free(cpu_info);
  cpu_info = calloc(1,sizeof(struct cpu_info));
  cpu_info->num_cores = num_cores;
  cpu_info->coreinfo = calloc(num_cores,sizeof(struct cpu_core_info));
  for (i = 0; i < num_cores; i++) cpu_info->coreinfo[i].is_available = 1;
}

/* Writes cpu<n>/regs/identification/midr_el1 (ARM's per-cpu microarchitecture
 * identification register) -- used to exercise coretype=<id> grouping
 * against a fake big.LITTLE part (e.g. Cortex-A720 "big" + Cortex-A520
 * "little" cores sharing one combined L3, confirmed against a real such
 * host: no separate L3 domain can tell the two apart, only MIDR can). */
static void make_fake_cpu_midr(int n,const char *midr_hex){
  char dir[512],path[512],buf[32];

  snprintf(dir,sizeof(dir),"%s/cpu%d/regs/identification",FAKE_SYSFS_BASE,n);
  mkdir_p(dir);
  snprintf(path,sizeof(path),"%s/midr_el1",dir);
  snprintf(buf,sizeof(buf),"%s\n",midr_hex);
  write_file(path,buf);
}

static void test_affinity_mode_name(void){
  printf("Testing affinity_mode_name...\n");
  assert(!strcmp(affinity_mode_name(AFFINITY_ALL),"all"));
  assert(!strcmp(affinity_mode_name(AFFINITY_THREAD),"thread"));
  assert(!strcmp(affinity_mode_name(AFFINITY_NOSMT),"nosmt"));
  assert(!strcmp(affinity_mode_name(AFFINITY_DOMAIN),"domain"));
  assert(!strcmp(affinity_mode_name(AFFINITY_CPUSET),"cpuset"));
  assert(!strcmp(affinity_mode_name(AFFINITY_CORETYPE),"coretype"));
  assert(!strcmp(affinity_mode_name((enum affinity_mode)99),"unknown"));
  printf("PASS: affinity_mode_name\n");
}

static void test_parse_spec(void){
  struct affinity_spec spec;

  printf("Testing affinity_parse_spec...\n");

  assert(affinity_parse_spec("all",&spec) == 0);
  assert(spec.mode == AFFINITY_ALL);

  assert(affinity_parse_spec("nosmt",&spec) == 0);
  assert(spec.mode == AFFINITY_NOSMT);

  assert(affinity_parse_spec("thread=5",&spec) == 0);
  assert(spec.mode == AFFINITY_THREAD);
  assert(spec.id == 5);

  assert(affinity_parse_spec("domain=2",&spec) == 0);
  assert(spec.mode == AFFINITY_DOMAIN);
  assert(spec.id == 2);

  assert(affinity_parse_spec("coretype=1",&spec) == 0);
  assert(spec.mode == AFFINITY_CORETYPE);
  assert(spec.id == 1);

  assert(affinity_parse_spec("cpuset=0,2-3",&spec) == 0);
  assert(spec.mode == AFFINITY_CPUSET);
  assert(CPU_ISSET(0,&spec.set));
  assert(!CPU_ISSET(1,&spec.set));
  assert(CPU_ISSET(2,&spec.set));
  assert(CPU_ISSET(3,&spec.set));
  assert(CPU_COUNT(&spec.set) == 3);

  assert(affinity_parse_spec("thread=-1",&spec) == -1);
  assert(affinity_parse_spec("thread=abc",&spec) == -1);
  assert(affinity_parse_spec("domain=",&spec) == -1);
  assert(affinity_parse_spec("coretype=",&spec) == -1);
  assert(affinity_parse_spec("coretype=-1",&spec) == -1);
  assert(affinity_parse_spec("cpuset=",&spec) == -1);
  assert(affinity_parse_spec("bogus",&spec) == -1);
  assert(affinity_parse_spec("",&spec) == -1);
  assert(affinity_parse_spec(NULL,&spec) == -1);

  printf("PASS: affinity_parse_spec\n");
}

static void test_format_cpu_set(void){
  cpu_set_t set;
  char buf[128];

  printf("Testing affinity_format_cpu_set...\n");

  CPU_ZERO(&set);
  affinity_format_cpu_set(&set,8,buf,sizeof(buf));
  assert(!strcmp(buf,""));

  CPU_ZERO(&set);
  CPU_SET(0,&set); CPU_SET(1,&set); CPU_SET(2,&set);
  affinity_format_cpu_set(&set,8,buf,sizeof(buf));
  assert(!strcmp(buf,"0-2"));

  CPU_ZERO(&set);
  CPU_SET(0,&set); CPU_SET(2,&set); CPU_SET(4,&set);
  affinity_format_cpu_set(&set,8,buf,sizeof(buf));
  assert(!strcmp(buf,"0,2,4"));

  CPU_ZERO(&set);
  CPU_SET(0,&set); CPU_SET(1,&set); CPU_SET(4,&set); CPU_SET(5,&set);
  affinity_format_cpu_set(&set,8,buf,sizeof(buf));
  assert(!strcmp(buf,"0-1,4-5"));

  printf("PASS: affinity_format_cpu_set\n");
}

static void test_topology_discover(void){
  int i;

  printf("Testing affinity_topology_discover_at...\n");

  build_fake_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);

  assert(affinity_topology.ncpus == 8);
  assert(affinity_topology.nl3domains == 2);

  /* SMT pairs {0,4},{1,5},{2,6},{3,7} -- lowest-numbered sibling is primary */
  assert(affinity_topology.cpu[0].is_primary_thread == 1);
  assert(affinity_topology.cpu[4].is_primary_thread == 0);
  assert(affinity_topology.cpu[1].is_primary_thread == 1);
  assert(affinity_topology.cpu[5].is_primary_thread == 0);

  /* domain A = {0,1,4,5} (16 MiB), domain B = {2,3,6,7} (8 MiB); domain A is
   * discovered first (ascending cpu scan starts at cpu 0) so it gets id 0. */
  assert(affinity_topology.cpu[0].l3_domain == 0);
  assert(affinity_topology.cpu[1].l3_domain == 0);
  assert(affinity_topology.cpu[4].l3_domain == 0);
  assert(affinity_topology.cpu[5].l3_domain == 0);
  assert(affinity_topology.cpu[2].l3_domain == 1);
  assert(affinity_topology.cpu[3].l3_domain == 1);
  assert(affinity_topology.cpu[6].l3_domain == 1);
  assert(affinity_topology.cpu[7].l3_domain == 1);

  assert(affinity_topology.l3domains[0].size_bytes == 16384UL*1024UL);
  assert(affinity_topology.l3domains[1].size_bytes == 8192UL*1024UL);
  for (i = 0; i < 8; i++){
    int want_domain = (i == 0 || i == 1 || i == 4 || i == 5) ? 0 : 1;
    assert(CPU_ISSET(i,&affinity_topology.l3domains[want_domain].cpus));
  }

  /* build_fake_topology() writes no midr_el1 files at all (this fixture
   * models an AMD-style host, no ARM core-type distinction) -- every cpu's
   * core_type must degrade to -1 and ncoretypes must stay 0, exactly the
   * real behavior confirmed on this codebase's own AMD dev host. */
  assert(affinity_topology.ncoretypes == 0);
  for (i = 0; i < 8; i++) assert(affinity_topology.cpu[i].core_type == -1);

  affinity_topology_free();
  assert(affinity_topology.ncpus == 0);
  assert(affinity_topology.cpu == NULL);
  assert(affinity_topology.nl3domains == 0);
  assert(affinity_topology.ncoretypes == 0);
  assert(affinity_topology.coretypes == NULL);

  printf("PASS: affinity_topology_discover_at\n");
}

static const int biglittle_big_cpus[] = {0,1,6,7,8,9,10,11};
static const int biglittle_little_cpus[] = {2,3,4,5};

/* Models a real heterogeneous ARM part reported by a user (12 cores: 8x
 * Cortex-A720 "big" + 4x Cortex-A520 "little", one combined L3 -- so
 * domain=<id> can't tell the two core types apart, only MIDR can).
 * implementer=0x41 (ARM); part 0xd81 for Cortex-A720, 0xd80 for Cortex-A520
 * (variant/revision bits deliberately differ per cpu to confirm they're
 * masked off, not treated as a distinct core type). */
static void build_fake_biglittle_topology(void){
  int i;
  static const char *big_midr[] = {
    "0x411fd810","0x412fd811","0x413fd810","0x410fd810",
    "0x411fd810","0x412fd811","0x413fd810","0x410fd810",
  };

  rmdir_recursive(FAKE_SYSFS_BASE);
  for (i = 0; i < 12; i++){
    /* topology/cache files aren't needed for this fixture -- only midr_el1
     * -- but make_fake_cpu() needs a topology dir to exist for its own
     * bookkeeping, so give every cpu a trivial singleton core/no L3. */
    char siblings[8];
    snprintf(siblings,sizeof(siblings),"%d",i);
    make_fake_cpu(i,i,0,siblings,NULL,NULL);
  }
  for (i = 0; i < 8; i++) make_fake_cpu_midr(biglittle_big_cpus[i],big_midr[i]);
  for (i = 0; i < 4; i++) make_fake_cpu_midr(biglittle_little_cpus[i],"0x412fd800");
}

static void test_topology_discover_core_types(void){
  int i;

  printf("Testing affinity_topology_discover_at: ARM big.LITTLE core types...\n");

  build_fake_biglittle_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,12);

  assert(affinity_topology.ncoretypes == 2);
  /* Discovery order is ascending cpu id: cpu0 (big) is seen first -> type 0;
   * cpu2 (little) is the first not-yet-assigned cpu after that -> type 1. */
  for (i = 0; i < 8; i++) assert(affinity_topology.cpu[biglittle_big_cpus[i]].core_type == 0);
  for (i = 0; i < 4; i++) assert(affinity_topology.cpu[biglittle_little_cpus[i]].core_type == 1);
  assert(affinity_topology.coretypes[0].implementer == 0x41);
  assert(affinity_topology.coretypes[0].part == 0xd81);
  assert(affinity_topology.coretypes[1].implementer == 0x41);
  assert(affinity_topology.coretypes[1].part == 0xd80);
  for (i = 0; i < 8; i++) assert(CPU_ISSET(biglittle_big_cpus[i],&affinity_topology.coretypes[0].cpus));
  for (i = 0; i < 4; i++) assert(CPU_ISSET(biglittle_little_cpus[i],&affinity_topology.coretypes[1].cpus));
  assert(CPU_COUNT(&affinity_topology.coretypes[0].cpus) == 8);
  assert(CPU_COUNT(&affinity_topology.coretypes[1].cpus) == 4);

  affinity_topology_free();
  printf("PASS: affinity_topology_discover_at ARM big.LITTLE core types\n");
}

/* Models a real Intel P-core/E-core hybrid part (Raptor/Alder Lake): 4
 * P-cores (cpu 0-3) + 4 E-cores (cpu 4-7), no midr_el1 files at all (x86
 * has no such register) -- exercises the coretype fallback that reuses
 * cpu_info.c's own CORE_INTEL_ATOM/CORE_INTEL_CORE per-core classification
 * (already computed by inventory_cpu() before affinity_topology_discover()
 * runs, see wspy.c's call sites) instead of re-deriving it from sysfs. */
static void test_topology_discover_x86_hybrid(void){
  int i;

  printf("Testing affinity_topology_discover_at: x86 P-core/E-core fallback...\n");

  build_fake_topology();
  build_fake_cpu_info(8);
  for (i = 0; i < 4; i++) cpu_info->coreinfo[i].vendor = CORE_INTEL_CORE;
  for (i = 4; i < 8; i++) cpu_info->coreinfo[i].vendor = CORE_INTEL_ATOM;

  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);

  assert(affinity_topology.ncoretypes == 2);
  /* Discovery order is ascending cpu id: cpu0 (intel_core) is seen first ->
   * type 0; cpu4 (intel_atom) is the first not-yet-assigned type -> type 1. */
  assert(!affinity_topology.coretypes[0].is_midr);
  assert(!strcmp(affinity_topology.coretypes[0].vendor_name,"intel_core"));
  assert(!affinity_topology.coretypes[1].is_midr);
  assert(!strcmp(affinity_topology.coretypes[1].vendor_name,"intel_atom"));
  for (i = 0; i < 4; i++) assert(affinity_topology.cpu[i].core_type == 0);
  for (i = 4; i < 8; i++) assert(affinity_topology.cpu[i].core_type == 1);
  for (i = 0; i < 4; i++) assert(CPU_ISSET(i,&affinity_topology.coretypes[0].cpus));
  for (i = 4; i < 8; i++) assert(CPU_ISSET(i,&affinity_topology.coretypes[1].cpus));
  assert(CPU_COUNT(&affinity_topology.coretypes[0].cpus) == 4);
  assert(CPU_COUNT(&affinity_topology.coretypes[1].cpus) == 4);

  affinity_topology_free();
  printf("PASS: affinity_topology_discover_at x86 P-core/E-core fallback\n");
}

/* A uniform (non-hybrid) x86 host must still report ncoretypes == 0 -- a
 * single classified vendor isn't "heterogeneous", so the fallback must
 * undo its own tentative single-entry grouping rather than report a
 * meaningless "core type 0" covering every cpu. */
static void test_topology_discover_x86_uniform(void){
  int i;

  printf("Testing affinity_topology_discover_at: x86 uniform (no hybrid)...\n");

  build_fake_topology();
  build_fake_cpu_info(8);
  for (i = 0; i < 8; i++) cpu_info->coreinfo[i].vendor = CORE_INTEL_CORE;

  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);

  assert(affinity_topology.ncoretypes == 0);
  assert(affinity_topology.coretypes == NULL);
  for (i = 0; i < 8; i++) assert(affinity_topology.cpu[i].core_type == -1);

  affinity_topology_free();
  printf("PASS: affinity_topology_discover_at x86 uniform\n");
}

/* A cpu with no sysfs files at all (never created by build_fake_topology())
 * degrades to core_id/package_id/l3_domain -1, treated as its own singleton
 * SMT group -- rather than crashing or misattributing it to cpu 0's group. */
static void test_topology_discover_missing_cpu(void){
  printf("Testing affinity_topology_discover_at: missing sysfs entries...\n");

  build_fake_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,9); /* cpu8 has no fake files */

  assert(affinity_topology.cpu[8].core_id == -1);
  assert(affinity_topology.cpu[8].package_id == -1);
  assert(affinity_topology.cpu[8].is_primary_thread == 1);
  assert(affinity_topology.cpu[8].l3_domain == -1);

  affinity_topology_free();
  printf("PASS: affinity_topology_discover_at missing cpu\n");
}

static void test_resolve(void){
  struct affinity_spec spec;

  printf("Testing affinity_resolve...\n");

  build_fake_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);
  build_fake_cpu_info(8);

  /* all -> every available cpu */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_ALL;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 8);

  /* nosmt -> one primary thread per core: {0,1,2,3} */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_NOSMT;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 4);
  assert(CPU_ISSET(0,&spec.set) && CPU_ISSET(1,&spec.set));
  assert(CPU_ISSET(2,&spec.set) && CPU_ISSET(3,&spec.set));
  assert(!CPU_ISSET(4,&spec.set) && !CPU_ISSET(5,&spec.set));

  /* domain=0 -> {0,1,4,5} */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_DOMAIN;
  spec.id = 0;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 4);
  assert(CPU_ISSET(0,&spec.set) && CPU_ISSET(1,&spec.set) && CPU_ISSET(4,&spec.set) && CPU_ISSET(5,&spec.set));

  /* thread=6 -> {6} */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_THREAD;
  spec.id = 6;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 1);
  assert(CPU_ISSET(6,&spec.set));

  /* thread=99 (out of range) -> -1 */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_THREAD;
  spec.id = 99;
  assert(affinity_resolve(&spec) == -1);

  /* domain=7 (out of range) -> -1 */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_DOMAIN;
  spec.id = 7;
  assert(affinity_resolve(&spec) == -1);

  /* cpuset={0,2} -> exact match when both available */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_CPUSET;
  CPU_ZERO(&spec.set);
  CPU_SET(0,&spec.set); CPU_SET(2,&spec.set);
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 2);

  affinity_topology_free();
  printf("PASS: affinity_resolve\n");
}

static void test_resolve_coretype(void){
  struct affinity_spec spec;
  int i;

  printf("Testing affinity_resolve: coretype= (ARM big.LITTLE)...\n");

  build_fake_biglittle_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,12);
  build_fake_cpu_info(12);

  /* coretype=0 (the "big" Cortex-A720 cluster) -> exactly biglittle_big_cpus */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_CORETYPE;
  spec.id = 0;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 8);
  for (i = 0; i < 8; i++) assert(CPU_ISSET(biglittle_big_cpus[i],&spec.set));

  /* coretype=1 (the "little" Cortex-A520 cluster) -> exactly biglittle_little_cpus */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_CORETYPE;
  spec.id = 1;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 4);
  for (i = 0; i < 4; i++) assert(CPU_ISSET(biglittle_little_cpus[i],&spec.set));

  /* coretype=2 (out of range -- only 2 core types exist) -> -1 */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_CORETYPE;
  spec.id = 2;
  assert(affinity_resolve(&spec) == -1);

  affinity_topology_free();
  printf("PASS: affinity_resolve coretype=\n");
}

/* On a host with no MIDR at all (this codebase's own AMD dev host, or any
 * non-ARM machine), ncoretypes stays 0 -- coretype=<id> must fail cleanly
 * (there's nothing to pin to) rather than resolve to an empty/meaningless
 * set, same as domain=<id> already does when nl3domains == 0. */
static void test_resolve_coretype_unavailable(void){
  struct affinity_spec spec;

  printf("Testing affinity_resolve: coretype= unavailable (no MIDR on this host)...\n");

  build_fake_topology(); /* the AMD-style 8-cpu fixture, no midr_el1 files */
  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);
  build_fake_cpu_info(8);

  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_CORETYPE;
  spec.id = 0;
  assert(affinity_resolve(&spec) == -1);

  affinity_topology_free();
  printf("PASS: affinity_resolve coretype= unavailable\n");
}

/* This process's own available-CPU mask (cpu_info->coreinfo[i].is_available)
 * only partially overlapping a request degrades to the overlap (with a
 * warning, not asserted here) rather than failing; no overlap at all fails. */
static void test_resolve_partial_and_no_availability(void){
  struct affinity_spec spec;

  printf("Testing affinity_resolve: availability intersection...\n");

  build_fake_topology();
  affinity_topology_discover_at(FAKE_SYSFS_BASE,8);
  build_fake_cpu_info(8);
  cpu_info->coreinfo[4].is_available = 0; /* simulate an outer taskset restriction */
  cpu_info->coreinfo[5].is_available = 0;

  /* domain=0 requests {0,1,4,5}; 4,5 unavailable -> degrades to {0,1} */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_DOMAIN;
  spec.id = 0;
  assert(affinity_resolve(&spec) == 0);
  assert(CPU_COUNT(&spec.set) == 2);
  assert(CPU_ISSET(0,&spec.set) && CPU_ISSET(1,&spec.set));
  assert(!CPU_ISSET(4,&spec.set) && !CPU_ISSET(5,&spec.set));

  /* thread=4 requests a cpu that's entirely unavailable -> -1 */
  memset(&spec,0,sizeof(spec));
  spec.mode = AFFINITY_THREAD;
  spec.id = 4;
  assert(affinity_resolve(&spec) == -1);

  affinity_topology_free();
  printf("PASS: affinity_resolve availability intersection\n");
}

int main(void){
  test_affinity_mode_name();
  test_parse_spec();
  test_format_cpu_set();
  test_topology_discover();
  test_topology_discover_missing_cpu();
  test_topology_discover_core_types();
  test_topology_discover_x86_hybrid();
  test_topology_discover_x86_uniform();
  test_resolve();
  test_resolve_partial_and_no_availability();
  test_resolve_coretype();
  test_resolve_coretype_unavailable();
  rmdir_recursive(FAKE_SYSFS_BASE);
  printf("All affinity tests passed.\n");
  return 0;
}
