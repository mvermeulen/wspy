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

int main(void){
  test_classify_simple_axis_thresholds();
  test_classify_simple_axis_unavailable();
  test_classify_resource_dominance_basic();
  test_classify_resource_dominance_all_unavailable();
  test_classify_resource_dominance_partial_availability();
  test_confidence_insufficient_data();
  test_confidence_high();
  test_confidence_medium();
  test_confidence_low_narrow_margin();
  test_score_runs_end_to_end();
  test_score_runs_skips_runs_with_no_features();
  test_trace_run_archetype_found();
  test_trace_run_archetype_not_found();

  printf("\nAll test_archetype tests passed.\n");
  return 0;
}
