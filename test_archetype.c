#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

#define TEST_ARCHETYPE 1
#include "archetype.c"

/* Minimal fixture: only the runs/run_features columns archetype.c actually
 * reads, not store.c's full SCHEMA_DDL -- keeps these tests decoupled from
 * store.c's own schema evolution, same reasoning test_summary.c's own
 * open_memory_db() comment gives. */
static sqlite3 *open_memory_db(void){
  sqlite3 *db;
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);
  assert(sqlite3_exec(db,
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL, "
    "command TEXT NOT NULL);"
    "CREATE TABLE run_features (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL, "
    "feature_name TEXT NOT NULL, value REAL, coverage TEXT NOT NULL, feature_set_version TEXT NOT NULL);",
    NULL,NULL,NULL) == SQLITE_OK);
  return db;
}

static sqlite3_int64 insert_run(sqlite3 *db,const char *run_id_text,const char *hostname,const char *command){
  sqlite3_stmt *stmt;
  sqlite3_int64 id;
  assert(sqlite3_prepare_v2(db,"INSERT INTO runs (run_id,hostname,command) VALUES (?,?,?);",
                            -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt,1,run_id_text,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,command,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
  id = sqlite3_last_insert_rowid(db);
  return id;
}

static void insert_feature_measured(sqlite3 *db,sqlite3_int64 run_id,const char *feature_name,double value){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO run_features (run_id,feature_name,value,coverage,feature_set_version) "
    "VALUES (?,?,?,'measured','1.1');",-1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int64(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,feature_name,-1,SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt,3,value);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_feature_unavailable(sqlite3 *db,sqlite3_int64 run_id,const char *feature_name){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO run_features (run_id,feature_name,value,coverage,feature_set_version) "
    "VALUES (?,?,NULL,'unavailable','1.1');",-1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int64(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,feature_name,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

/* --- classify_simple_axis --- */

static void test_classify_simple_axis_thresholds(void){
  struct classified_axis out;

  printf("Testing classify_simple_axis: ascending threshold rules pick the first satisfied bucket...\n");

  classify_simple_axis(0.10,1,PARALLELISM_RULES,2,&out);
  assert(out.available && !strcmp(out.label,"balanced-parallel"));

  classify_simple_axis(0.15,1,PARALLELISM_RULES,2,&out);
  assert(out.available && !strcmp(out.label,"balanced-parallel")); /* boundary is inclusive (<=) */

  classify_simple_axis(0.16,1,PARALLELISM_RULES,2,&out);
  assert(out.available && !strcmp(out.label,"imbalanced"));

  classify_simple_axis(1.9,1,CONTROL_FLOW_RULES,2,&out);
  assert(out.available && !strcmp(out.label,"straight-line"));
  classify_simple_axis(2.1,1,CONTROL_FLOW_RULES,2,&out);
  assert(out.available && !strcmp(out.label,"branch-heavy"));

  classify_simple_axis(0.3,1,STABILITY_RULES,3,&out);
  assert(out.available && !strcmp(out.label,"erratic"));
  classify_simple_axis(0.6,1,STABILITY_RULES,3,&out);
  assert(out.available && !strcmp(out.label,"phased"));
  classify_simple_axis(0.95,1,STABILITY_RULES,3,&out);
  assert(out.available && !strcmp(out.label,"steady"));

  printf("PASS: classify_simple_axis thresholds\n");
}

static void test_classify_simple_axis_unavailable(void){
  struct classified_axis out;

  printf("Testing classify_simple_axis: unavailable input always yields unknown regardless of value...\n");
  classify_simple_axis(0.01,0,PARALLELISM_RULES,2,&out);
  assert(!out.available && !strcmp(out.label,"unknown"));
  printf("PASS: classify_simple_axis unavailable\n");
}

/* --- classify_resource_dominance --- */

static void test_classify_resource_dominance_basic(void){
  struct dominance_result out;

  printf("Testing classify_resource_dominance: ranks the 4 topdown L1 categories, reports top-2...\n");
  classify_resource_dominance(20.0,1, 15.0,1, 46.0,1, 19.0,1, &out);
  assert(out.available);
  assert(!strcmp(out.primary_label,"memory-bound"));
  assert(out.primary_pct > 45.9 && out.primary_pct < 46.1);
  assert(out.has_alternative);
  assert(!strcmp(out.alternative_label,"compute-bound"));
  assert(out.alternative_pct > 19.9 && out.alternative_pct < 20.1);
  printf("PASS: classify_resource_dominance basic\n");
}

static void test_classify_resource_dominance_all_unavailable(void){
  struct dominance_result out;

  printf("Testing classify_resource_dominance: all 4 unavailable -> not available (unknown)...\n");
  classify_resource_dominance(0,0, 0,0, 0,0, 0,0, &out);
  assert(!out.available);
  printf("PASS: classify_resource_dominance all-unavailable\n");
}

static void test_classify_resource_dominance_partial_availability(void){
  struct dominance_result out;

  printf("Testing classify_resource_dominance: only some categories measured still ranks correctly...\n");
  classify_resource_dominance(70.0,1, 0,0, 20.0,1, 0,0, &out);
  assert(out.available);
  assert(!strcmp(out.primary_label,"compute-bound"));
  assert(out.has_alternative && !strcmp(out.alternative_label,"memory-bound"));

  /* Only one candidate measured at all -- no alternative to report. */
  classify_resource_dominance(70.0,1, 0,0, 0,0, 0,0, &out);
  assert(out.available && !out.has_alternative);
  printf("PASS: classify_resource_dominance partial availability\n");
}

/* --- classify_memory_attribution --- */

static void test_classify_memory_attribution_corroborated(void){
  struct memory_attribution_result out;
  struct memory_signal signals[3] = {
    { "dcache_miss_pct", 3.0,  1, CACHE_MISS_ELEVATED_PCT }, /* below threshold */
    { "l3_miss_pct",     42.0, 1, CACHE_MISS_ELEVATED_PCT }, /* above -- fires */
    { "ibs_dram_pct",    38.0, 1, IBS_DRAM_ELEVATED_PCT },   /* above -- fires */
  };

  printf("Testing classify_memory_attribution: backend_pct significant + >=1 corroborating signal "
         "elevated -> corroborated, reasons lists only the signals that fired...\n");
  classify_memory_attribution(55.0,1,signals,3,&out);
  assert(out.available);
  assert(!strcmp(out.label,"corroborated"));
  assert(strstr(out.reasons,"l3_miss_pct=42") != NULL);
  assert(strstr(out.reasons,"ibs_dram_pct=38") != NULL);
  assert(strstr(out.reasons,"dcache_miss_pct") == NULL); /* didn't fire, not listed */
  printf("PASS: classify_memory_attribution corroborated\n");
}

static void test_classify_memory_attribution_uncorroborated(void){
  struct memory_attribution_result out;
  struct memory_signal signals[2] = {
    { "dcache_miss_pct", 2.0, 1, CACHE_MISS_ELEVATED_PCT },
    { "l2_miss_pct",     3.0, 1, CACHE_MISS_ELEVATED_PCT },
  };

  printf("Testing classify_memory_attribution: backend_pct significant but every measured "
         "corroborating signal is unremarkable -> uncorroborated, reasons lists what was checked...\n");
  classify_memory_attribution(55.0,1,signals,2,&out);
  assert(out.available);
  assert(!strcmp(out.label,"uncorroborated"));
  assert(strstr(out.reasons,"checked:dcache_miss_pct") != NULL);
  assert(strstr(out.reasons,"checked:l2_miss_pct") != NULL);
  printf("PASS: classify_memory_attribution uncorroborated\n");
}

static void test_classify_memory_attribution_not_memory_bound(void){
  struct memory_attribution_result out;
  struct memory_signal signals[1] = { { "l3_miss_pct", 90.0, 1, CACHE_MISS_ELEVATED_PCT } };

  printf("Testing classify_memory_attribution: backend_pct below the significance floor -> "
         "not-memory-bound regardless of how elevated other signals are...\n");
  classify_memory_attribution(5.0,1,signals,1,&out);
  assert(out.available);
  assert(!strcmp(out.label,"not-memory-bound"));
  assert(out.reasons[0] == '\0');
  printf("PASS: classify_memory_attribution not-memory-bound\n");
}

static void test_classify_memory_attribution_unknown_no_backend_data(void){
  struct memory_attribution_result out;
  struct memory_signal signals[1] = { { "l3_miss_pct", 90.0, 1, CACHE_MISS_ELEVATED_PCT } };

  printf("Testing classify_memory_attribution: backend_pct itself never measured -> unknown, "
         "available=0 (distinct from the 'measured but inconclusive' unknown case)...\n");
  classify_memory_attribution(0.0,0,signals,1,&out);
  assert(!out.available);
  assert(!strcmp(out.label,"unknown"));
  printf("PASS: classify_memory_attribution unknown (no backend data)\n");
}

static void test_classify_memory_attribution_unknown_no_corroborating_data(void){
  struct memory_attribution_result out;
  struct memory_signal signals[2] = {
    { "dcache_miss_pct", 0.0, 0, CACHE_MISS_ELEVATED_PCT }, /* not measured this run */
    { "l2_miss_pct",     0.0, 0, CACHE_MISS_ELEVATED_PCT },
  };

  printf("Testing classify_memory_attribution: backend_pct significant but zero corroborating "
         "signals were even collected -> unknown, available=1 (we do know backend_pct)...\n");
  classify_memory_attribution(55.0,1,signals,2,&out);
  assert(out.available);
  assert(!strcmp(out.label,"unknown"));
  assert(out.reasons[0] == '\0');
  printf("PASS: classify_memory_attribution unknown (no corroborating data collected)\n");
}

/* --- compute_overall_confidence --- */

static void test_confidence_insufficient_data(void){
  struct dominance_result dom;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result conf;
  int i;

  printf("Testing compute_overall_confidence: unavailable resource_dominance -> insufficient-data...\n");
  memset(&dom,0,sizeof(dom));
  for (i = 0; i < NUM_SIMPLE_AXES; i++){ simple[i].available = 0; strcpy(simple[i].label,"unknown"); }
  compute_overall_confidence(&dom,simple,NUM_SIMPLE_AXES,&conf);
  assert(!strcmp(conf.level,"insufficient-data"));
  assert(!strcmp(conf.reasons,"no-topdown-data"));
  printf("PASS: compute_overall_confidence insufficient-data\n");
}

static void test_confidence_high(void){
  struct dominance_result dom;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result conf;

  printf("Testing compute_overall_confidence: decisive margin + >=2 known axes -> high, no reasons...\n");
  memset(&dom,0,sizeof(dom));
  dom.available = 1; dom.primary_pct = 70.0;
  dom.has_alternative = 1; dom.alternative_pct = 20.0; /* margin 50 */
  simple[AXIS_PARALLELISM_SHAPE].available = 1;
  simple[AXIS_CONTROL_FLOW_STYLE].available = 1;
  simple[AXIS_RUNTIME_STABILITY].available = 0;

  compute_overall_confidence(&dom,simple,NUM_SIMPLE_AXES,&conf);
  assert(!strcmp(conf.level,"high"));
  assert(!strcmp(conf.reasons,"missing-runtime_stability-data"));
  printf("PASS: compute_overall_confidence high\n");
}

static void test_confidence_medium(void){
  struct dominance_result dom;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result conf;
  int i;

  printf("Testing compute_overall_confidence: narrow-ish margin, one known axis -> medium...\n");
  memset(&dom,0,sizeof(dom));
  dom.available = 1; dom.primary_pct = 40.0;
  dom.has_alternative = 1; dom.alternative_pct = 28.0; /* margin 12 */
  for (i = 0; i < NUM_SIMPLE_AXES; i++) simple[i].available = 0;
  simple[AXIS_PARALLELISM_SHAPE].available = 1;

  compute_overall_confidence(&dom,simple,NUM_SIMPLE_AXES,&conf);
  assert(!strcmp(conf.level,"medium"));
  assert(strstr(conf.reasons,"missing-control_flow_style-data") != NULL);
  assert(strstr(conf.reasons,"missing-runtime_stability-data") != NULL);
  printf("PASS: compute_overall_confidence medium\n");
}

static void test_confidence_low_narrow_margin(void){
  struct dominance_result dom;
  struct classified_axis simple[NUM_SIMPLE_AXES];
  struct confidence_result conf;
  int i;

  printf("Testing compute_overall_confidence: very narrow margin -> low, narrow-margin reason first...\n");
  memset(&dom,0,sizeof(dom));
  dom.available = 1; dom.primary_pct = 26.0;
  dom.has_alternative = 1; dom.alternative_pct = 25.0; /* margin 1 */
  for (i = 0; i < NUM_SIMPLE_AXES; i++) simple[i].available = 1;

  compute_overall_confidence(&dom,simple,NUM_SIMPLE_AXES,&conf);
  assert(!strcmp(conf.level,"low"));
  assert(!strcmp(conf.reasons,"narrow-margin")); /* every axis known, only the margin itself is the reason */
  printf("PASS: compute_overall_confidence low narrow-margin\n");
}

/* --- end-to-end: score_runs() (bulk mode) --- */

static void test_score_runs_end_to_end(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_bulk_XXXXXX";
  int fd;
  FILE *out;
  char line[4096];
  sqlite3_int64 run1;
  int rows;

  printf("Testing score_runs: end-to-end bulk scorecard from a fixture DB...\n");
  db = open_memory_db();
  run1 = insert_run(db,"run1","host1","/bin/workload");
  insert_feature_measured(db,run1,"retire_pct",20.0);
  insert_feature_measured(db,run1,"frontend_pct",15.0);
  insert_feature_measured(db,run1,"backend_pct",46.0);
  insert_feature_measured(db,run1,"speculate_pct",19.0);
  insert_feature_measured(db,run1,"parallelism_proxy",0.5);
  insert_feature_measured(db,run1,"branch_mispredict_pct",1.0);
  insert_feature_measured(db,run1,"phase_stability",0.9);
  insert_feature_unavailable(db,run1,"active_core_count");

  fd = mkstemp(tmpfile);
  assert(fd >= 0);
  out = fdopen(fd,"w+");
  assert(out != NULL);

  rows = score_runs(db,"","",1,out);
  assert(rows == 1);

  rewind(out);
  assert(fgets(line,sizeof(line),out) != NULL); /* header */
  assert(strstr(line,"resource_dominance") != NULL);
  assert(fgets(line,sizeof(line),out) != NULL); /* the one scored run */
  assert(strstr(line,"host1") != NULL);
  assert(strstr(line,"memory-bound") != NULL);
  assert(strstr(line,"compute-bound") != NULL); /* the top-2 alternative */
  assert(strstr(line,"straight-line") != NULL);
  assert(strstr(line,"steady") != NULL);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: score_runs end-to-end\n");
}

static void test_score_runs_skips_runs_with_no_features(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nofeatures_XXXXXX";
  int fd;
  FILE *out;
  int rows;

  printf("Testing score_runs: a run with zero run_features rows is excluded, not shown as all-unknown...\n");
  db = open_memory_db();
  insert_run(db,"run1","host1","/bin/workload"); /* no run_features rows at all */

  fd = mkstemp(tmpfile);
  assert(fd >= 0);
  out = fdopen(fd,"w+");
  assert(out != NULL);

  rows = score_runs(db,"","",1,out);
  assert(rows == 0);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: score_runs skips runs with no run_features\n");
}

static void test_score_runs_memory_attribution_end_to_end(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_memattr_XXXXXX";
  int fd;
  FILE *out;
  char line[4096];
  sqlite3_int64 run1;
  int rows;

  printf("Testing score_runs: memory_attribution flows through the full run_features -> scorecard "
         "-> CSV pipeline, corroborated by an IBS-derived feature...\n");
  db = open_memory_db();
  run1 = insert_run(db,"run1","host1","/bin/workload");
  insert_feature_measured(db,run1,"retire_pct",20.0);
  insert_feature_measured(db,run1,"frontend_pct",15.0);
  insert_feature_measured(db,run1,"backend_pct",55.0);
  insert_feature_measured(db,run1,"speculate_pct",10.0);
  insert_feature_measured(db,run1,"l3_miss_pct",42.0);
  insert_feature_measured(db,run1,"ibs_dram_pct",38.0);

  fd = mkstemp(tmpfile);
  assert(fd >= 0);
  out = fdopen(fd,"w+");
  assert(out != NULL);

  rows = score_runs(db,"","",1,out);
  assert(rows == 1);

  rewind(out);
  assert(fgets(line,sizeof(line),out) != NULL); /* header */
  assert(strstr(line,"memory_attribution") != NULL);
  assert(fgets(line,sizeof(line),out) != NULL);
  assert(strstr(line,"corroborated") != NULL);
  assert(strstr(line,"l3_miss_pct=42") != NULL);
  assert(strstr(line,"ibs_dram_pct=38") != NULL);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: score_runs memory_attribution end-to-end\n");
}

/* --- end-to-end: trace_run_archetype() (--run mode) --- */

static void test_trace_run_archetype_found(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_trace_XXXXXX";
  int fd;
  FILE *out;
  char buf[8192];
  size_t n;
  sqlite3_int64 run1;
  int rc;

  printf("Testing trace_run_archetype: found run prints key=value lines including top-2/confidence...\n");
  db = open_memory_db();
  run1 = insert_run(db,"run1","host1","/bin/workload");
  insert_feature_measured(db,run1,"retire_pct",70.0);
  insert_feature_measured(db,run1,"frontend_pct",5.0);
  insert_feature_measured(db,run1,"backend_pct",20.0);
  insert_feature_measured(db,run1,"speculate_pct",5.0);

  fd = mkstemp(tmpfile);
  assert(fd >= 0);
  out = fdopen(fd,"w+");
  assert(out != NULL);

  rc = trace_run_archetype(db,"host1","run1",out);
  assert(rc == 0);

  rewind(out);
  n = fread(buf,1,sizeof(buf)-1,out);
  buf[n] = '\0';
  assert(strstr(buf,"resource_dominance=compute-bound\n") != NULL);
  assert(strstr(buf,"alternative=memory-bound\n") != NULL);
  assert(strstr(buf,"parallelism_shape=unknown\n") != NULL);
  assert(strstr(buf,"confidence=") != NULL);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: trace_run_archetype found\n");
}

static void test_trace_run_archetype_not_found(void){
  sqlite3 *db;
  FILE *devnull;
  int rc;

  printf("Testing trace_run_archetype: unknown (hostname,run_id) returns 1, not a usage error...\n");
  db = open_memory_db();
  insert_run(db,"run1","host1","/bin/workload");

  devnull = fopen("/dev/null","w");
  assert(devnull != NULL);
  rc = trace_run_archetype(db,"host1","no-such-run",devnull);
  assert(rc == 1);

  fclose(devnull);
  sqlite3_close(db);
  printf("PASS: trace_run_archetype not found\n");
}

/* --- end-to-end: nearest_neighbors() (--nearest mode) --- */

static FILE *open_tmp_out(char *tmpfile_template){
  int fd = mkstemp(tmpfile_template);
  FILE *out;
  assert(fd >= 0);
  out = fdopen(fd,"w+");
  assert(out != NULL);
  return out;
}

static char *slurp(FILE *out,char *buf,size_t bufsize){
  size_t n;
  rewind(out);
  n = fread(buf,1,bufsize - 1,out);
  buf[n] = '\0';
  return buf;
}

/* CSV row shape is "hostname,run_id,distance,compared_features\n" -- finds the
 * row for run_id_text and returns its compared_features column, or -1 if the
 * run isn't present in the output at all. */
static int csv_compared_features_for(const char *buf,const char *run_id_text){
  char needle[80];
  const char *p;
  int shared;
  snprintf(needle,sizeof(needle),",%s,",run_id_text);
  p = strstr(buf,needle);
  if (!p) return -1;
  p += strlen(needle);
  if (sscanf(p,"%*[^,],%d",&shared) != 1) return -1;
  return shared;
}

static void test_nearest_basic_ranking(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_basic_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t,near,far;
  int rc;
  char *p_near,*p_far;

  printf("Testing nearest_neighbors: the more-similar run ranks ahead of the more-different run...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload");
  near = insert_run(db,"runNear","host1","/bin/workload");
  far = insert_run(db,"runFar","host1","/bin/workload");
  (void)near; (void)far;

  insert_feature_measured(db,t,"f1",10.0);
  insert_feature_measured(db,t,"f2",10.0);
  insert_feature_measured(db,insert_run(db,"dummy1","host1","/bin/workload"),"f1",10.0); /* pad variance */

  insert_feature_measured(db,near,"f1",11.0);
  insert_feature_measured(db,near,"f2",11.0);

  insert_feature_measured(db,far,"f1",50.0);
  insert_feature_measured(db,far,"f2",50.0);

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  p_near = strstr(buf,"runNear");
  p_far = strstr(buf,"runFar");
  assert(p_near != NULL && p_far != NULL);
  assert(p_near < p_far); /* runNear is genuinely closer to runT, must rank first */

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors basic ranking\n");
}

static void test_nearest_common_subspace_normalization(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_subspace_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t,x,y;
  int rc;
  char *p_x,*p_y;

  printf("Testing nearest_neighbors: fewer shared features (but genuinely closer) still ranks correctly, "
         "RMS-normalized rather than penalized for lower overlap...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload");
  x = insert_run(db,"runX","host1","/bin/workload"); /* shares only f1,f2 -- very close values */
  y = insert_run(db,"runY","host1","/bin/workload"); /* shares f1..f4 -- moderately further on each */

  insert_feature_measured(db,t,"f1",10.0);
  insert_feature_measured(db,t,"f2",10.0);
  insert_feature_measured(db,t,"f3",10.0);
  insert_feature_measured(db,t,"f4",10.0);

  insert_feature_measured(db,x,"f1",10.1);
  insert_feature_measured(db,x,"f2",10.1);

  insert_feature_measured(db,y,"f1",12.0);
  insert_feature_measured(db,y,"f2",12.0);
  insert_feature_measured(db,y,"f3",12.0);
  insert_feature_measured(db,y,"f4",12.0);

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  assert(csv_compared_features_for(buf,"runX") == 2);
  assert(csv_compared_features_for(buf,"runY") == 4);

  p_x = strstr(buf,"runX");
  p_y = strstr(buf,"runY");
  assert(p_x != NULL && p_y != NULL);
  assert(p_x < p_y); /* runX is genuinely closer despite sharing fewer features */

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors common-subspace normalization\n");
}

static void test_nearest_zero_shared_features_excluded(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_noshare_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t,disjoint,near;
  int rc;

  printf("Testing nearest_neighbors: a candidate with zero overlapping measured features is excluded "
         "entirely, not shown with a fabricated distance...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload");
  disjoint = insert_run(db,"runDisjoint","host1","/bin/workload");
  near = insert_run(db,"runNear","host1","/bin/workload");

  insert_feature_measured(db,t,"f1",10.0);
  insert_feature_measured(db,near,"f1",11.0);
  insert_feature_measured(db,disjoint,"g1",99.0); /* no feature name in common with runT */

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  assert(strstr(buf,"runNear") != NULL);
  assert(strstr(buf,"runDisjoint") == NULL);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors zero-shared-features exclusion\n");
}

static void test_nearest_zero_variance_feature_no_crash(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_zerovar_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t,near,far;
  int rc;
  char *p_near,*p_far;

  printf("Testing nearest_neighbors: a feature with identical value everywhere (zero variance) doesn't "
         "crash (divide-by-zero) or distort ranking...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload");
  near = insert_run(db,"runNear","host1","/bin/workload");
  far = insert_run(db,"runFar","host1","/bin/workload");

  insert_feature_measured(db,t,"f1",10.0);
  insert_feature_measured(db,t,"const_feat",5.0);
  insert_feature_measured(db,near,"f1",11.0);
  insert_feature_measured(db,near,"const_feat",5.0);
  insert_feature_measured(db,far,"f1",50.0);
  insert_feature_measured(db,far,"const_feat",5.0);

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  assert(strstr(buf,"nan") == NULL);
  assert(strstr(buf,"inf") == NULL);
  p_near = strstr(buf,"runNear");
  p_far = strstr(buf,"runFar");
  assert(p_near != NULL && p_far != NULL);
  assert(p_near < p_far);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors zero-variance feature no-crash\n");
}

static void test_nearest_k_limiting(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_klimit_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t;
  int rc,i,rows = 0;
  char *p;
  double last_distance = -1.0;

  printf("Testing nearest_neighbors: --k limits output to the closest k, ascending by distance...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload");
  insert_feature_measured(db,t,"f1",0.0);
  for (i = 0; i < 6; i++){
    char run_id[32],cmd[64];
    sqlite3_int64 r;
    snprintf(run_id,sizeof(run_id),"run%d",i);
    snprintf(cmd,sizeof(cmd),"/bin/workload");
    r = insert_run(db,run_id,"host1",cmd);
    insert_feature_measured(db,r,"f1",(double)(i + 1) * 10.0); /* strictly increasing distance from runT */
  }

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",3,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  p = buf;
  p = strstr(p,"\n"); /* skip header */
  assert(p != NULL);
  p++;
  while (*p){
    char host[64],rid[64];
    double dist;
    int shared;
    if (sscanf(p,"%63[^,],%63[^,],%lf,%d",host,rid,&dist,&shared) == 4){
      assert(dist >= last_distance);
      last_distance = dist;
      rows++;
    }
    p = strchr(p,'\n');
    if (!p) break;
    p++;
  }
  assert(rows == 3);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors --k limiting\n");
}

static void test_nearest_filters_restrict_pool(void){
  sqlite3 *db;
  char tmpfile1[] = "/tmp/test_archetype_nn_filter1_XXXXXX";
  char tmpfile2[] = "/tmp/test_archetype_nn_filter2_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 t,same_host,other_host,other_cmd;
  int rc;

  printf("Testing nearest_neighbors: --hostname/--command filters restrict the candidate pool...\n");
  db = open_memory_db();
  t = insert_run(db,"runT","host1","/bin/workload_a");
  same_host = insert_run(db,"runSameHost","host1","/bin/workload_a");
  other_host = insert_run(db,"runOtherHost","host2","/bin/workload_a");
  other_cmd = insert_run(db,"runOtherCmd","host1","/bin/workload_b");

  insert_feature_measured(db,t,"f1",10.0);
  insert_feature_measured(db,same_host,"f1",11.0);
  insert_feature_measured(db,other_host,"f1",12.0);
  insert_feature_measured(db,other_cmd,"f1",13.0);

  out = open_tmp_out(tmpfile1);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","host1",1,out); /* hostname filter */
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));
  assert(strstr(buf,"runSameHost") != NULL);
  assert(strstr(buf,"runOtherHost") == NULL);
  assert(strstr(buf,"runOtherCmd") != NULL); /* same host, different command -- not filtered by hostname */
  fclose(out);
  remove(tmpfile1);

  out = open_tmp_out(tmpfile2);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"workload_a","",1,out); /* command filter */
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));
  assert(strstr(buf,"runSameHost") != NULL);
  assert(strstr(buf,"runOtherHost") != NULL); /* same command, different host -- not filtered by command */
  assert(strstr(buf,"runOtherCmd") == NULL);
  fclose(out);
  remove(tmpfile2);

  sqlite3_close(db);
  printf("PASS: nearest_neighbors filters restrict pool\n");
}

static void test_nearest_target_not_found(void){
  sqlite3 *db;
  FILE *devnull;
  int rc;

  printf("Testing nearest_neighbors: unknown (hostname,run_id) returns 1, matching --run's convention...\n");
  db = open_memory_db();
  insert_run(db,"run1","host1","/bin/workload");

  devnull = fopen("/dev/null","w");
  assert(devnull != NULL);
  rc = nearest_neighbors(db,"host1","no-such-run",NEAREST_DEFAULT_K,"","",1,devnull);
  assert(rc == 1);

  fclose(devnull);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors target not found\n");
}

static void test_nearest_target_no_features_empty_result(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_nn_empty_XXXXXX";
  FILE *out;
  char buf[8192];
  int rc;

  printf("Testing nearest_neighbors: target exists but has zero measured features -> graceful empty "
         "result, not a crash...\n");
  db = open_memory_db();
  insert_run(db,"runT","host1","/bin/workload"); /* no run_features rows at all */

  out = open_tmp_out(tmpfile);
  rc = nearest_neighbors(db,"host1","runT",NEAREST_DEFAULT_K,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  assert(!strcmp(buf,"hostname,run_id,distance,compared_features\n")); /* header only, no data rows */

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: nearest_neighbors target with no features -> empty result\n");
}

/* --- end-to-end: kmeans_report() (--kmeans mode) --- */

/* CSV row shape is "cluster,size,hostname,run_id,distance,top_features\n" --
 * finds the row for run_id_text and returns its cluster id, or -1 if the
 * run isn't present in the output at all. */
static int csv_cluster_id_for(const char *buf,const char *run_id_text){
  char needle[80];
  const char *p,*line_start;
  int cluster_id;

  snprintf(needle,sizeof(needle),",%s,",run_id_text);
  p = strstr(buf,needle);
  if (!p) return -1;
  line_start = p;
  while (line_start > buf && line_start[-1] != '\n') line_start--;
  if (sscanf(line_start,"%d,",&cluster_id) != 1) return -1;
  return cluster_id;
}

/* Number of data rows (excludes the header line) -- counts '\n' after the
 * first one. */
static int csv_row_count(const char *buf){
  const char *nl = strchr(buf,'\n');
  int count = 0;
  if (!nl) return 0;
  for (nl++; *nl; nl++) if (*nl == '\n') count++;
  return count;
}

static void test_kmeans_basic_separation(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_km_basic_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 a,b,c,d,e,f;
  int rc,ca,cb,cc,cd,ce,cf;

  printf("Testing kmeans_report: two well-separated groups land in two distinct clusters...\n");
  db = open_memory_db();
  a = insert_run(db,"runA","host1","cpu_bound_a");
  b = insert_run(db,"runB","host1","cpu_bound_b");
  c = insert_run(db,"runC","host1","cpu_bound_c");
  d = insert_run(db,"runD","host1","mem_bound_a");
  e = insert_run(db,"runE","host1","mem_bound_b");
  f = insert_run(db,"runF","host1","mem_bound_c");

  insert_feature_measured(db,a,"ipc_mean",2.0); insert_feature_measured(db,a,"dcache_miss_pct",1.0);
  insert_feature_measured(db,b,"ipc_mean",2.1); insert_feature_measured(db,b,"dcache_miss_pct",1.2);
  insert_feature_measured(db,c,"ipc_mean",1.9); insert_feature_measured(db,c,"dcache_miss_pct",0.9);
  insert_feature_measured(db,d,"ipc_mean",0.3); insert_feature_measured(db,d,"dcache_miss_pct",25.0);
  insert_feature_measured(db,e,"ipc_mean",0.35); insert_feature_measured(db,e,"dcache_miss_pct",26.0);
  insert_feature_measured(db,f,"ipc_mean",0.25); insert_feature_measured(db,f,"dcache_miss_pct",24.0);

  out = open_tmp_out(tmpfile);
  rc = kmeans_report(db,2,KMEANS_DEFAULT_SEED,KMEANS_DEFAULT_ITERATIONS,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));

  ca = csv_cluster_id_for(buf,"runA"); cb = csv_cluster_id_for(buf,"runB"); cc = csv_cluster_id_for(buf,"runC");
  cd = csv_cluster_id_for(buf,"runD"); ce = csv_cluster_id_for(buf,"runE"); cf = csv_cluster_id_for(buf,"runF");
  assert(ca >= 0 && cb >= 0 && cc >= 0 && cd >= 0 && ce >= 0 && cf >= 0);
  assert(ca == cb && cb == cc); /* cpu_bound group agrees */
  assert(cd == ce && ce == cf); /* mem_bound group agrees */
  assert(ca != cd); /* the two groups land in different clusters */
  assert(csv_row_count(buf) == 6);

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: kmeans_report basic separation\n");
}

static void test_kmeans_coverage_aware_centroid_no_crash(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_km_coverage_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 a,b,c,d;
  int rc;

  printf("Testing kmeans_report: heterogeneous feature coverage across members clusters without "
         "crashing (partial-distance / available-case centroid)...\n");
  db = open_memory_db();
  a = insert_run(db,"runA","host1","a");
  b = insert_run(db,"runB","host1","b");
  c = insert_run(db,"runC","host1","c");
  d = insert_run(db,"runD","host1","d");

  /* runA/runB have both features (e.g. ran with --branch); runC/runD only
   * have ipc_mean (no --branch that time). */
  insert_feature_measured(db,a,"ipc_mean",2.0); insert_feature_measured(db,a,"branch_mpki",1.0);
  insert_feature_measured(db,b,"ipc_mean",2.1); insert_feature_measured(db,b,"branch_mpki",1.1);
  insert_feature_measured(db,c,"ipc_mean",0.3);
  insert_feature_measured(db,d,"ipc_mean",0.35);

  out = open_tmp_out(tmpfile);
  rc = kmeans_report(db,2,KMEANS_DEFAULT_SEED,KMEANS_DEFAULT_ITERATIONS,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));
  assert(csv_row_count(buf) == 4);
  assert(csv_cluster_id_for(buf,"runA") == csv_cluster_id_for(buf,"runB"));
  assert(csv_cluster_id_for(buf,"runC") == csv_cluster_id_for(buf,"runD"));

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: kmeans_report coverage-aware centroid no-crash\n");
}

static void test_kmeans_insufficient_candidates(void){
  sqlite3 *db;
  sqlite3_int64 a,b;
  FILE *devnull;
  int rc;

  printf("Testing kmeans_report: fewer candidate runs than k returns 1, not a crash or a bogus "
         "partition...\n");
  db = open_memory_db();
  a = insert_run(db,"runA","host1","a");
  b = insert_run(db,"runB","host1","b");
  insert_feature_measured(db,a,"ipc_mean",1.0);
  insert_feature_measured(db,b,"ipc_mean",1.1);

  devnull = fopen("/dev/null","w");
  assert(devnull != NULL);
  rc = kmeans_report(db,5,KMEANS_DEFAULT_SEED,KMEANS_DEFAULT_ITERATIONS,"","",1,devnull);
  assert(rc == 1);

  fclose(devnull);
  sqlite3_close(db);
  printf("PASS: kmeans_report insufficient candidates\n");
}

static void test_kmeans_seed_determinism(void){
  sqlite3 *db;
  char tmpfile1[] = "/tmp/test_archetype_km_seed1_XXXXXX";
  char tmpfile2[] = "/tmp/test_archetype_km_seed2_XXXXXX";
  FILE *out1,*out2;
  char buf1[8192],buf2[8192];
  sqlite3_int64 a,b,c,d,e,f;
  int rc;

  printf("Testing kmeans_report: same seed + same data yields identical clustering...\n");
  db = open_memory_db();
  a = insert_run(db,"runA","host1","a"); b = insert_run(db,"runB","host1","b");
  c = insert_run(db,"runC","host1","c"); d = insert_run(db,"runD","host1","d");
  e = insert_run(db,"runE","host1","e"); f = insert_run(db,"runF","host1","f");
  insert_feature_measured(db,a,"ipc_mean",2.0);
  insert_feature_measured(db,b,"ipc_mean",2.1);
  insert_feature_measured(db,c,"ipc_mean",1.9);
  insert_feature_measured(db,d,"ipc_mean",0.3);
  insert_feature_measured(db,e,"ipc_mean",0.35);
  insert_feature_measured(db,f,"ipc_mean",0.25);

  out1 = open_tmp_out(tmpfile1);
  rc = kmeans_report(db,2,7,KMEANS_DEFAULT_ITERATIONS,"","",1,out1);
  assert(rc == 0);
  slurp(out1,buf1,sizeof(buf1));
  fclose(out1);
  remove(tmpfile1);

  out2 = open_tmp_out(tmpfile2);
  rc = kmeans_report(db,2,7,KMEANS_DEFAULT_ITERATIONS,"","",1,out2);
  assert(rc == 0);
  slurp(out2,buf2,sizeof(buf2));
  fclose(out2);
  remove(tmpfile2);

  assert(!strcmp(buf1,buf2));

  sqlite3_close(db);
  printf("PASS: kmeans_report seed determinism\n");
}

static void test_kmeans_no_empty_clusters(void){
  sqlite3 *db;
  char tmpfile[] = "/tmp/test_archetype_km_noempty_XXXXXX";
  FILE *out;
  char buf[8192];
  sqlite3_int64 ids[6];
  double values[6] = { 1.0,1.1,0.9,1.05,0.95,1.02 }; /* one tight cluster, k=3 forces reinit */
  int rc,i,seen[3] = {0,0,0};
  char run_id[16];

  printf("Testing kmeans_report: k larger than the natural number of groups still yields k non-empty "
         "clusters (empty-cluster reinit)...\n");
  db = open_memory_db();
  for (i = 0; i < 6; i++){
    snprintf(run_id,sizeof(run_id),"run%d",i);
    ids[i] = insert_run(db,run_id,"host1","same_workload");
    insert_feature_measured(db,ids[i],"ipc_mean",values[i]);
  }

  out = open_tmp_out(tmpfile);
  rc = kmeans_report(db,3,KMEANS_DEFAULT_SEED,KMEANS_DEFAULT_ITERATIONS,"","",1,out);
  assert(rc == 0);
  slurp(out,buf,sizeof(buf));
  assert(csv_row_count(buf) == 6);

  for (i = 0; i < 6; i++){
    int cid;
    snprintf(run_id,sizeof(run_id),"run%d",i);
    cid = csv_cluster_id_for(buf,run_id);
    assert(cid >= 0 && cid < 3);
    seen[cid] = 1;
  }
  assert(seen[0] && seen[1] && seen[2]); /* every cluster got at least one member */

  fclose(out);
  remove(tmpfile);
  sqlite3_close(db);
  printf("PASS: kmeans_report no empty clusters\n");
}

int main(void){
  test_classify_simple_axis_thresholds();
  test_classify_simple_axis_unavailable();
  test_classify_resource_dominance_basic();
  test_classify_resource_dominance_all_unavailable();
  test_classify_resource_dominance_partial_availability();
  test_classify_memory_attribution_corroborated();
  test_classify_memory_attribution_uncorroborated();
  test_classify_memory_attribution_not_memory_bound();
  test_classify_memory_attribution_unknown_no_backend_data();
  test_classify_memory_attribution_unknown_no_corroborating_data();
  test_confidence_insufficient_data();
  test_confidence_high();
  test_confidence_medium();
  test_confidence_low_narrow_margin();
  test_score_runs_end_to_end();
  test_score_runs_skips_runs_with_no_features();
  test_score_runs_memory_attribution_end_to_end();
  test_trace_run_archetype_found();
  test_trace_run_archetype_not_found();
  test_nearest_basic_ranking();
  test_nearest_common_subspace_normalization();
  test_nearest_zero_shared_features_excluded();
  test_nearest_zero_variance_feature_no_crash();
  test_nearest_k_limiting();
  test_nearest_filters_restrict_pool();
  test_nearest_target_not_found();
  test_nearest_target_no_features_empty_result();
  test_kmeans_basic_separation();
  test_kmeans_coverage_aware_centroid_no_crash();
  test_kmeans_insufficient_candidates();
  test_kmeans_seed_determinism();
  test_kmeans_no_empty_clusters();

  printf("\nAll test_archetype tests passed.\n");
  return 0;
}
