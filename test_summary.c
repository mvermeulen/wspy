#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include <unistd.h>

#define TEST_SUMMARY 1
#include "summary.c"

static struct summary_opts default_opts(void){
  struct summary_opts opts;
  memset(&opts,0,sizeof(opts));
  opts.command_filter = "";
  opts.hostname_filter = "";
  opts.group_by = GROUP_COMMAND;
  opts.outlier_z = 2.0;
  opts.min_runs = 1;
  opts.max_cv = 5.0;
  return opts;
}

/* Minimal fixture: only the runs/metric_values columns summary.c actually
 * reads, not store.c's full SCHEMA_DDL -- keeps these tests decoupled
 * from store.c's own schema evolution. Includes affinity_mode/preset_name/
 * config_name (runs) and run_environment/run_config_options (both always
 * LEFT JOINed by summarize()'s query, so they must exist even for tests
 * that don't populate them) for the "Comparison matrix mode deep-dive"
 * grouping extension. */
static sqlite3 *open_memory_db(void){
  sqlite3 *db;
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);
  assert(sqlite3_exec(db,
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL, "
    "command TEXT NOT NULL, cpu_vendor TEXT, start_time TEXT NOT NULL, "
    "affinity_mode TEXT, preset_name TEXT, config_name TEXT, "
    "counters_requested INTEGER, counters_measured INTEGER, "
    "manifest_path TEXT, output_path TEXT, tree_output_path TEXT);"
    "CREATE TABLE metric_values (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL, "
    "metric_name TEXT NOT NULL, value REAL);"
    "CREATE TABLE run_environment (run_id INTEGER PRIMARY KEY, cpu_governor TEXT, virt_role TEXT);"
    "CREATE TABLE run_config_options (run_id INTEGER NOT NULL, option_name TEXT NOT NULL, "
    "option_value TEXT NOT NULL, PRIMARY KEY (run_id, option_name));",
    NULL,NULL,NULL) == SQLITE_OK);
  return db;
}

static void set_run_affinity(sqlite3 *db,int run_id,const char *affinity_mode){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,"UPDATE runs SET affinity_mode=? WHERE id=?;",-1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_text(stmt,1,affinity_mode,-1,SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt,2,run_id);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_config_option(sqlite3 *db,int run_id,const char *name,const char *value){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO run_config_options (run_id,option_name,option_value) VALUES (?,?,?);",
    -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,name,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,value,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_run_environment(sqlite3 *db,int run_id,const char *cpu_governor){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO run_environment (run_id,cpu_governor) VALUES (?,?);",
    -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,cpu_governor,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

/* trace_run() reads runs.{manifest_path,output_path,tree_output_path}, which
 * insert_run() above (used by every other fixture) leaves NULL -- this
 * variant is only for the --trace tests below. */
static void insert_run_with_paths(sqlite3 *db,int id,const char *run_id,const char *hostname,
                                   const char *command,const char *start_time,
                                   const char *manifest_path,const char *output_path,
                                   const char *tree_output_path){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO runs (id,run_id,hostname,command,start_time,manifest_path,output_path,tree_output_path) "
    "VALUES (?,?,?,?,?,?,?,?);",
    -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,id);
  sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,4,command,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,5,start_time,-1,SQLITE_TRANSIENT);
  if (manifest_path) sqlite3_bind_text(stmt,6,manifest_path,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(stmt,6);
  if (output_path) sqlite3_bind_text(stmt,7,output_path,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(stmt,7);
  if (tree_output_path) sqlite3_bind_text(stmt,8,tree_output_path,-1,SQLITE_TRANSIENT); else sqlite3_bind_null(stmt,8);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_run(sqlite3 *db,int id,const char *run_id,const char *hostname,
                        const char *command,const char *cpu_vendor,const char *start_time){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO runs (id,run_id,hostname,command,cpu_vendor,start_time) VALUES (?,?,?,?,?,?);",
    -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,id);
  sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,4,command,-1,SQLITE_TRANSIENT);
  if (cpu_vendor) sqlite3_bind_text(stmt,5,cpu_vendor,-1,SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt,5);
  sqlite3_bind_text(stmt,6,start_time,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

/* Like insert_run(), plus counters_requested/counters_measured -- for the
 * "mixed-pmu" verdict tests below, which need to control those alongside
 * cpu_vendor. Every other fixture uses plain insert_run() and leaves both
 * columns NULL (sqlite3_column_int() reads NULL as 0 uniformly), so they
 * all share the same (vendor,0,0) signature and never trigger mixed-pmu
 * by accident. */
static void insert_run_with_pmu(sqlite3 *db,int id,const char *run_id,const char *hostname,
                                 const char *command,const char *cpu_vendor,const char *start_time,
                                 int counters_requested,int counters_measured){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,
    "INSERT INTO runs (id,run_id,hostname,command,cpu_vendor,start_time,"
    "counters_requested,counters_measured) VALUES (?,?,?,?,?,?,?,?);",
    -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,id);
  sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,4,command,-1,SQLITE_TRANSIENT);
  if (cpu_vendor) sqlite3_bind_text(stmt,5,cpu_vendor,-1,SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt,5);
  sqlite3_bind_text(stmt,6,start_time,-1,SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt,7,counters_requested);
  sqlite3_bind_int(stmt,8,counters_measured);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_metric(sqlite3 *db,int run_id,const char *metric_name,double value){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,"INSERT INTO metric_values (run_id,metric_name,value) VALUES (?,?,?);",
                             -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,metric_name,-1,SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt,3,value);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

static void insert_metric_null(sqlite3 *db,int run_id,const char *metric_name){
  sqlite3_stmt *stmt;
  assert(sqlite3_prepare_v2(db,"INSERT INTO metric_values (run_id,metric_name,value) VALUES (?,?,NULL);",
                             -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int(stmt,1,run_id);
  sqlite3_bind_text(stmt,2,metric_name,-1,SQLITE_TRANSIENT);
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}

/* Finds the CSV data line beginning with "group,metric," (test data here
 * never puts a comma/quote in a group or metric name, so no CSV-quoting
 * awareness is needed to locate it) and parses its numeric fields, plus
 * verdict as a plain (unquoted) token -- the "%31[^,]" scan doesn't
 * understand print_csv_field()'s quoting, so a verdict containing a comma
 * itself (WARN:thin,noisy) isn't parseable this way; those cases are
 * checked directly against the raw buffer with strstr() instead (see
 * test_summarize_verdict_thin_and_noisy_both_fire below). verdict_buf must
 * be caller-allocated, >=32 bytes. Returns 1 if found. */
static int find_csv_row(const char *buf,const char *group,const char *metric,
                         int *n,double *min_v,double *max_v,double *mean_v,
                         double *median_v,double *stddev_v,double *cv,
                         double *ci_low,double *ci_high,char *verdict_buf,int *outlier_count){
  char prefix[256];
  const char *line = buf;
  size_t prefix_len;

  snprintf(prefix,sizeof(prefix),"%s,%s,",group,metric);
  prefix_len = strlen(prefix);
  while (line && *line){
    const char *eol = strchr(line,'\n');
    size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
    if (linelen >= prefix_len && !strncmp(line,prefix,prefix_len)){
      sscanf(line + prefix_len,"%d,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%lf,%31[^,],%d,",
             n,min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high,verdict_buf,outlier_count);
      return 1;
    }
    line = eol ? eol + 1 : NULL;
  }
  return 0;
}

static void test_compute_stats_basic(void){
  double values[] = {1,2,3,4,5};
  double min_v,max_v,mean_v,median_v,stddev_v;
  int outlier_flags[5];
  int outliers = compute_stats(values,5,2.0,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);

  assert(min_v == 1);
  assert(max_v == 5);
  assert(fabs(mean_v - 3.0) < 1e-9);
  assert(fabs(median_v - 3.0) < 1e-9);
  assert(fabs(stddev_v - sqrt(2.5)) < 1e-9);
  assert(outliers == 0);
  printf("test_compute_stats_basic passed\n");
}

static void test_compute_stats_single_sample(void){
  double values[] = {42};
  double min_v,max_v,mean_v,median_v,stddev_v;
  int outlier_flags[1];
  int outliers = compute_stats(values,1,2.0,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);

  assert(min_v == 42 && max_v == 42 && mean_v == 42 && median_v == 42);
  assert(stddev_v == 0.0);
  assert(outliers == 0);
  printf("test_compute_stats_single_sample passed\n");
}

static void test_compute_stats_even_count_median(void){
  double values[] = {1,2,3,4};
  double min_v,max_v,mean_v,median_v,stddev_v;
  int outlier_flags[4];

  compute_stats(values,4,2.0,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);
  assert(fabs(median_v - 2.5) < 1e-9);
  printf("test_compute_stats_even_count_median passed\n");
}

static void test_compute_stats_outlier_detected(void){
  double values[] = {10,10,10,10,100};
  double min_v,max_v,mean_v,median_v,stddev_v;
  int outlier_flags[5];
  int outliers = compute_stats(values,5,1.5,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);

  assert(outliers == 1);
  assert(outlier_flags[4] == 1);
  assert(outlier_flags[0] == 0);
  printf("test_compute_stats_outlier_detected passed\n");
}

static void test_compute_stats_two_samples_never_flagged(void){
  double values[] = {1,1000};
  double min_v,max_v,mean_v,median_v,stddev_v;
  int outlier_flags[2];
  /* n<3: outlier flagging never fires, even with a razor-thin threshold --
   * there's no meaningful "outlier" among just two points. */
  int outliers = compute_stats(values,2,0.0001,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);

  assert(outliers == 0);
  printf("test_compute_stats_two_samples_never_flagged passed\n");
}

static void test_t_critical_95_table_and_fallback(void){
  assert(fabs(t_critical_95(1) - 12.706) < 1e-9);
  assert(fabs(t_critical_95(30) - 2.042) < 1e-9);
  assert(fabs(t_critical_95(31) - 1.96) < 1e-9); /* beyond the table -> normal approximation */
  assert(fabs(t_critical_95(500) - 1.96) < 1e-9);
  printf("test_t_critical_95_table_and_fallback passed\n");
}

static void test_compute_ci95_single_sample_is_zero_width(void){
  double ci_low,ci_high;
  /* n<2: stddev is 0 by compute_stats()'s own convention, so the interval
   * must degenerate to the point value without consulting the t-table
   * (df=0 has no entry). */
  compute_ci95(42.0,0.0,1,&ci_low,&ci_high);
  assert(ci_low == 42.0 && ci_high == 42.0);
  printf("test_compute_ci95_single_sample_is_zero_width passed\n");
}

static void test_compute_ci95_matches_formula(void){
  double ci_low,ci_high,expected_margin;
  /* Same values as test_compute_stats_basic (mean=3, stddev=sqrt(2.5), n=5)
   * -- confirms compute_ci95() actually wires mean +/- t*stddev/sqrt(n)
   * together correctly, not an independently-sourced expected number. */
  compute_ci95(3.0,sqrt(2.5),5,&ci_low,&ci_high);
  expected_margin = t_critical_95(4) * sqrt(2.5) / sqrt(5.0);
  assert(fabs(ci_low - (3.0 - expected_margin)) < 1e-9);
  assert(fabs(ci_high - (3.0 + expected_margin)) < 1e-9);
  assert(ci_low < 3.0 && ci_high > 3.0);
  printf("test_compute_ci95_matches_formula passed\n");
}

static void test_compute_verdict_pass(void){
  char verdict[32];
  compute_verdict(5,2.0,5.0,0,verdict,sizeof(verdict)); /* n>=3, cv < max_cv, not mixed */
  assert(strcmp(verdict,"PASS") == 0);
  printf("test_compute_verdict_pass passed\n");
}

static void test_compute_verdict_thin_only(void){
  char verdict[32];
  compute_verdict(2,0.0,5.0,0,verdict,sizeof(verdict)); /* n<3, cv well under max_cv */
  assert(strcmp(verdict,"WARN:thin") == 0);
  printf("test_compute_verdict_thin_only passed\n");
}

static void test_compute_verdict_noisy_only(void){
  char verdict[32];
  compute_verdict(10,50.0,5.0,0,verdict,sizeof(verdict)); /* n>=3, cv far over max_cv */
  assert(strcmp(verdict,"WARN:noisy") == 0);
  printf("test_compute_verdict_noisy_only passed\n");
}

static void test_compute_verdict_thin_and_noisy(void){
  char verdict[32];
  compute_verdict(2,50.0,5.0,0,verdict,sizeof(verdict));
  assert(strcmp(verdict,"WARN:thin,noisy") == 0);
  printf("test_compute_verdict_thin_and_noisy passed\n");
}

static void test_compute_verdict_boundary_not_noisy(void){
  char verdict[32];
  /* Exactly at --max-cv: the check is strictly-greater-than, so the
   * boundary itself is not flagged. */
  compute_verdict(5,5.0,5.0,0,verdict,sizeof(verdict));
  assert(strcmp(verdict,"PASS") == 0);
  printf("test_compute_verdict_boundary_not_noisy passed\n");
}

static void test_compute_verdict_mixed_pmu_only(void){
  char verdict[32];
  compute_verdict(5,2.0,5.0,1,verdict,sizeof(verdict)); /* n>=3, cv low, but mixed_pmu set */
  assert(strcmp(verdict,"WARN:mixed-pmu") == 0);
  printf("test_compute_verdict_mixed_pmu_only passed\n");
}

static void test_compute_verdict_all_three_reasons(void){
  char verdict[32];
  compute_verdict(2,50.0,5.0,1,verdict,sizeof(verdict));
  /* Fixed reason order (thin, noisy, mixed-pmu) regardless of internal
   * evaluation order -- see compute_verdict()'s own comment. */
  assert(strcmp(verdict,"WARN:thin,noisy,mixed-pmu") == 0);
  printf("test_compute_verdict_all_three_reasons passed\n");
}

static void test_parse_group_by(void){
  enum group_by g;

  assert(parse_group_by("command",&g) == 1 && g == GROUP_COMMAND);
  assert(parse_group_by("hostname",&g) == 1 && g == GROUP_HOSTNAME);
  assert(parse_group_by("cpu_vendor",&g) == 1 && g == GROUP_CPU_VENDOR);
  assert(parse_group_by("affinity_mode",&g) == 1 && g == GROUP_AFFINITY_MODE);
  assert(parse_group_by("preset_name",&g) == 1 && g == GROUP_PRESET_NAME);
  assert(parse_group_by("config_name",&g) == 1 && g == GROUP_CONFIG_NAME);
  assert(parse_group_by("cpu_governor",&g) == 1 && g == GROUP_CPU_GOVERNOR);
  assert(parse_group_by("virt_role",&g) == 1 && g == GROUP_VIRT_ROLE);
  assert(parse_group_by("bogus",&g) == 0);
  printf("test_parse_group_by passed\n");
}

static void test_metric_wanted(void){
  struct summary_opts opts = default_opts();

  assert(metric_wanted(&opts,"anything") == 1); /* nmetrics==0 -> all metrics */
  opts.metrics[0] = "ipc";
  opts.metrics[1] = "retire";
  opts.nmetrics = 2;
  assert(metric_wanted(&opts,"ipc") == 1);
  assert(metric_wanted(&opts,"cache_miss") == 0);
  printf("test_metric_wanted passed\n");
}

static void test_print_csv_field_quoting(void){
  char *buf;
  size_t size;
  FILE *fp = open_memstream(&buf,&size);

  print_csv_field(fp,"plain");
  fputc('|',fp);
  print_csv_field(fp,"has,comma");
  fputc('|',fp);
  print_csv_field(fp,"has\"quote");
  fclose(fp);
  assert(strcmp(buf,"plain|\"has,comma\"|\"has\"\"quote\"") == 0);
  free(buf);
  printf("test_print_csv_field_quoting passed\n");
}

static void test_summarize_averages_per_run_and_groups_by_command(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  /* workloadA: 3 runs, one aggregate "ipc" value each -> stats span those
   * 3 per-run values directly. */
  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",1.1);
  insert_run(db,3,"r3","host1","/bin/workloadA",NULL,"2026-01-01T00:02:00Z");
  insert_metric(db,3,"ipc",1.2);

  /* A single run whose "cache_miss" metric has a 3-tick --interval-shaped
   * series -- must collapse (via AVG) to one number for this one run. */
  insert_run(db,4,"r4","host1","/bin/workloadA",NULL,"2026-01-01T00:03:00Z");
  insert_metric(db,4,"cache_miss",10.0);
  insert_metric(db,4,"cache_miss",20.0);
  insert_metric(db,4,"cache_miss",30.0);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(n == 3);
  assert(fabs(min_v - 1.0) < 1e-9);
  assert(fabs(max_v - 1.2) < 1e-9);
  assert(fabs(mean_v - 1.1) < 1e-9);

  assert(find_csv_row(buf,"/bin/workloadA","cache_miss",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(n == 1);
  assert(fabs(mean_v - 20.0) < 1e-9); /* the one run's 3 ticks averaged */

  assert(totals.groups_reported == 2);
  assert(totals.groups_skipped_min_runs == 0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_averages_per_run_and_groups_by_command passed\n");
}

static void test_summarize_command_filter(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadB",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,2,"ipc",2.0);

  opts.csvflag = 1;
  opts.command_filter = "workloadB";
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"workloadA") == NULL);
  assert(strstr(buf,"workloadB") != NULL);
  assert(totals.groups_reported == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_command_filter passed\n");
}

static void test_summarize_hostname_filter(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host2","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,2,"ipc",2.0);

  opts.csvflag = 1;
  opts.hostname_filter = "host2";
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(n == 1);
  assert(fabs(min_v - 2.0) < 1e-9 && fabs(max_v - 2.0) < 1e-9 && fabs(mean_v - 2.0) < 1e-9);
  assert(stddev_v == 0.0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_hostname_filter passed\n");
}

static void test_summarize_metric_filter(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_metric(db,1,"cache_miss",5.0);

  opts.csvflag = 1;
  opts.metrics[0] = "ipc";
  opts.nmetrics = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,",ipc,") != NULL);
  assert(strstr(buf,"cache_miss") == NULL);
  assert(totals.rows_scanned == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_metric_filter passed\n");
}

static void test_summarize_min_runs_skips_thin_buckets(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);

  opts.csvflag = 1;
  opts.min_runs = 2;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strlen(buf) == 0);
  assert(totals.groups_reported == 0);
  assert(totals.groups_skipped_min_runs == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_min_runs_skips_thin_buckets passed\n");
}

static void test_summarize_verdict_pass_low_cv_enough_runs(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers,i;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  for (i = 0; i < 4; i++){
    char run_id[8],start_time[32];
    snprintf(run_id,sizeof(run_id),"r%d",i);
    snprintf(start_time,sizeof(start_time),"2026-01-01T00:0%d:00Z",i);
    insert_run(db,i + 1,run_id,"host1","/bin/workloadA",NULL,start_time);
    insert_metric(db,i + 1,"ipc",1.0); /* identical values -> cv=0, n=4>=3 */
  }

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(strcmp(verdict,"PASS") == 0);
  assert(totals.groups_warned == 0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_pass_low_cv_enough_runs passed\n");
}

static void test_summarize_verdict_thin_when_fewer_than_three_runs(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",1.0); /* n=2: identical values, cv=0 -- isolates "thin" from "noisy" */

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(n == 2);
  assert(strcmp(verdict,"WARN:thin") == 0);
  assert(totals.groups_warned == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_thin_when_fewer_than_three_runs passed\n");
}

static void test_summarize_verdict_noisy_when_cv_exceeds_default_max_cv(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  /* n=3 (clears the "thin" threshold), but CV ~9.1% is well over the
   * default --max-cv of 5.0. */
  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",1.1);
  insert_run(db,3,"r3","host1","/bin/workloadA",NULL,"2026-01-01T00:02:00Z");
  insert_metric(db,3,"ipc",1.2);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(n == 3);
  assert(cv > 5.0);
  assert(strcmp(verdict,"WARN:noisy") == 0);
  assert(totals.groups_warned == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_noisy_when_cv_exceeds_default_max_cv passed\n");
}

static void test_summarize_max_cv_flag_raises_noisy_threshold(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  /* Same ~9.1%-CV data as the default-threshold test above, but with
   * --max-cv raised past it -- confirms the flag actually changes the
   * verdict, not just that the default threshold does something. */
  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",1.1);
  insert_run(db,3,"r3","host1","/bin/workloadA",NULL,"2026-01-01T00:02:00Z");
  insert_metric(db,3,"ipc",1.2);

  opts.csvflag = 1;
  opts.max_cv = 20.0;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(strcmp(verdict,"PASS") == 0);
  assert(totals.groups_warned == 0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_max_cv_flag_raises_noisy_threshold passed\n");
}

/* verdict "WARN:thin,noisy" contains a literal comma, so print_csv_field()
 * quotes it -- find_csv_row()'s unquoting-unaware parser can't extract it
 * (see its own comment), so this checks the raw CSV text directly instead. */
static void test_summarize_verdict_thin_and_noisy_both_fire(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",100.0); /* n=2 (thin) and wildly noisy */

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"\"WARN:thin,noisy\"") != NULL);
  assert(totals.groups_warned == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_thin_and_noisy_both_fire passed\n");
}

/* INVESTIGATION.md's 4.2 Tier 1 "PMU-capability-aware comparability
 * warnings" item: a bucket blending runs from two different cpu_vendor
 * values (same command, same metric name -- e.g. two hosts' "retire"
 * column, computed from genuinely different per-vendor raw events) must
 * be flagged, even with plenty of runs and low variance (n=3, identical
 * values -> cv=0, would otherwise be a clean PASS). */
static void test_summarize_verdict_mixed_pmu_different_vendor(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run_with_pmu(db,1,"r1","host1","/bin/workloadA","AMD","2026-01-01T00:00:00Z",9,9);
  insert_metric(db,1,"retire",50.0);
  insert_run_with_pmu(db,2,"r2","host2","/bin/workloadA","AMD","2026-01-01T00:01:00Z",9,9);
  insert_metric(db,2,"retire",50.0);
  insert_run_with_pmu(db,3,"r3","host3","/bin/workloadA","Intel","2026-01-01T00:02:00Z",9,9);
  insert_metric(db,3,"retire",50.0);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,",WARN:mixed-pmu,") != NULL);
  assert(totals.groups_warned == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_mixed_pmu_different_vendor passed\n");
}

/* Same vendor, but one run's counter setup measured fewer counters than it
 * requested (e.g. a permission-denied counter this run, none the others) --
 * wspy-validate already flags this *within* one run's own manifest, but
 * summarize() aggregating a degraded run alongside clean ones is something
 * only this cross-run view can see. */
static void test_summarize_verdict_mixed_pmu_different_coverage(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run_with_pmu(db,1,"r1","host1","/bin/workloadA","AMD","2026-01-01T00:00:00Z",9,9);
  insert_metric(db,1,"ipc",1.0);
  insert_run_with_pmu(db,2,"r2","host1","/bin/workloadA","AMD","2026-01-01T00:01:00Z",9,6);
  insert_metric(db,2,"ipc",1.0);
  insert_run_with_pmu(db,3,"r3","host1","/bin/workloadA","AMD","2026-01-01T00:02:00Z",9,9);
  insert_metric(db,3,"ipc",1.0);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,",WARN:mixed-pmu,") != NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_mixed_pmu_different_coverage passed\n");
}

/* Sanity check for the opposite case: identical (vendor,requested,measured)
 * signature across every contributing run must never spuriously flag
 * mixed-pmu, even across different hostnames/run_ids. */
static void test_summarize_verdict_same_pmu_signature_not_mixed(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int n,outliers;
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
  char verdict[32];

  insert_run_with_pmu(db,1,"r1","host1","/bin/workloadA","AMD","2026-01-01T00:00:00Z",9,9);
  insert_metric(db,1,"retire",50.0);
  insert_run_with_pmu(db,2,"r2","host2","/bin/workloadA","AMD","2026-01-01T00:01:00Z",9,9);
  insert_metric(db,2,"retire",51.0);
  insert_run_with_pmu(db,3,"r3","host3","/bin/workloadA","AMD","2026-01-01T00:02:00Z",9,9);
  insert_metric(db,3,"retire",49.0);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"mixed-pmu") == NULL);
  assert(find_csv_row(buf,"/bin/workloadA","retire",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,
                       &ci_low,&ci_high,verdict,&outliers));
  assert(strcmp(verdict,"PASS") == 0);
  assert(totals.groups_warned == 0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_verdict_same_pmu_signature_not_mixed passed\n");
}

static void test_summarize_group_by_cpu_vendor_unknown_bucket(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z"); /* no manifest enrichment yet */
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA","AMD","2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",2.0);

  opts.csvflag = 1;
  opts.group_by = GROUP_CPU_VENDOR;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"(unknown),ipc,") != NULL);
  assert(strstr(buf,"AMD,ipc,") != NULL);
  assert(totals.groups_reported == 2);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_cpu_vendor_unknown_bucket passed\n");
}

/* affinity_mode is a plain runs column (INVESTIGATION.md's "Comparison
 * matrix mode deep-dive", piece 1) -- the SMT-on/off comparison this whole
 * item exists for. */
static void test_summarize_group_by_affinity_mode(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  set_run_affinity(db,1,"all");
  insert_metric(db,1,"ipc",1.1);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  set_run_affinity(db,2,"nosmt");
  insert_metric(db,2,"ipc",1.4);

  opts.csvflag = 1;
  opts.group_by = GROUP_AFFINITY_MODE;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"all,ipc,") != NULL);
  assert(strstr(buf,"nosmt,ipc,") != NULL);
  assert(totals.groups_reported == 2);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_affinity_mode passed\n");
}

/* cpu_governor lives in run_environment, not runs -- summarize()'s query
 * must LEFT JOIN it for this grouping to work at all (a pre-4.2 build had
 * no join to run_environment, so this would previously have been a SQL
 * error, not just an unsupported --group-by value). */
static void test_summarize_group_by_cpu_governor_via_environment_join(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_run_environment(db,1,"performance");
  insert_metric(db,1,"ipc",1.5);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_run_environment(db,2,"powersave");
  insert_metric(db,2,"ipc",1.0);

  opts.csvflag = 1;
  opts.group_by = GROUP_CPU_GOVERNOR;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"performance,ipc,") != NULL);
  assert(strstr(buf,"powersave,ipc,") != NULL);
  assert(totals.groups_reported == 2);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_cpu_governor_via_environment_join passed\n");
}

/* The actual comparison-matrix payoff: --group-by command (unchanged) plus
 * --group-by-option composes a second axis from an arbitrary --config-option
 * key, e.g. a wspy-sweep cell tag -- "for this workload, broken out by
 * SMT on/off" rather than one flat regrouping. */
static void test_summarize_group_by_option_composes_with_primary_group(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/coremark",NULL,"2026-01-01T00:00:00Z");
  insert_config_option(db,1,"affinity_axis","all");
  insert_metric(db,1,"ipc",1.1);
  insert_run(db,2,"r2","host1","/bin/coremark",NULL,"2026-01-01T00:01:00Z");
  insert_config_option(db,2,"affinity_axis","nosmt");
  insert_metric(db,2,"ipc",1.4);
  /* A second workload sharing the same axis values -- must not be merged
   * into the first workload's buckets, confirming the two axes actually
   * compose (2 groups x 2 secondary values = 4 buckets) rather than one
   * replacing the other. */
  insert_run(db,3,"r3","host1","/bin/sha256",NULL,"2026-01-01T00:02:00Z");
  insert_config_option(db,3,"affinity_axis","all");
  insert_metric(db,3,"ipc",2.1);

  opts.csvflag = 1;
  opts.group_by_option = "affinity_axis";
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"/bin/coremark,all,ipc,") != NULL);
  assert(strstr(buf,"/bin/coremark,nosmt,ipc,") != NULL);
  assert(strstr(buf,"/bin/sha256,all,ipc,") != NULL);
  assert(totals.groups_reported == 3);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_option_composes_with_primary_group passed\n");
}

/* Without --group-by-option, output must be byte-for-byte the pre-existing
 * shape -- no spurious column, no accidental extra bucket split, even
 * though run_config_options is now always LEFT JOINed under the hood. */
static void test_summarize_group_by_option_unset_is_inert(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_config_option(db,1,"affinity_axis","all"); /* present in the store, just not asked for */
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host1","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_config_option(db,2,"affinity_axis","nosmt");
  insert_metric(db,2,"ipc",1.2);

  opts.csvflag = 1;
  assert(opts.group_by_option == NULL);
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  /* Exactly the pre-4.2 row shape ("group,metric,..." -- no secondary
   * column spliced in between) despite run_config_options holding two
   * distinct values across these two runs. */
  assert(strstr(buf,"/bin/workloadA,ipc,2,") != NULL);
  assert(totals.groups_reported == 1); /* both runs land in one bucket, as before this item */

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_option_unset_is_inert passed\n");
}

/* A run with no matching run_config_options row for the requested option
 * name degrades to "(unknown)" (the same sentinel a NULL primary group
 * value already uses), not a crash or an empty/garbage column. */
static void test_summarize_group_by_option_missing_value_shows_unknown(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  /* no run_config_options row for this run at all */
  insert_metric(db,1,"ipc",1.0);

  opts.csvflag = 1;
  opts.group_by_option = "affinity_axis";
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"/bin/workloadA,(unknown),ipc,") != NULL);
  assert(totals.groups_reported == 1);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_group_by_option_missing_value_shows_unknown passed\n");
}

static void test_summarize_null_only_metric_excluded(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric_null(db,1,"nan_only"); /* topdown.c can emit a literal "-nan" cell; store.c parses that to NULL */

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strlen(buf) == 0);
  assert(totals.groups_reported == 0);
  assert(totals.rows_scanned == 0);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_null_only_metric_excluded passed\n");
}

static void test_summarize_show_runs_lists_contributors(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);
  insert_run(db,2,"r2","host2","/bin/workloadA",NULL,"2026-01-01T00:01:00Z");
  insert_metric(db,2,"ipc",1.2);

  opts.csvflag = 1;
  opts.show_runs = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  /* Ordered by (group,metric,start_time,id), so host1's earlier run lists first. */
  assert(strstr(buf,"host1:r1;host2:r2") != NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_show_runs_lists_contributors passed\n");
}

static void test_summarize_without_show_runs_omits_contributors(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");
  insert_metric(db,1,"ipc",1.0);

  opts.csvflag = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  /* Default (no --show-runs) keeps the pre-existing column set -- no
   * hostname:run_id identity leaks into output unless asked for. */
  assert(strstr(buf,"host1:r1") == NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_without_show_runs_omits_contributors passed\n");
}

/* trace_run() is the other half of item 14 -- given a hostname:run_id (the
 * same identity --show-runs prints), resolve it to the manifest/raw CSV/
 * tree/plots artifact chain. Exercised against a real temp directory (not
 * just string paths) so manifest_exists/output_exists/plots_count reflect
 * actual stat()/opendir() results, not just what was recorded at ingest
 * time -- the whole point of this tool is noticing when a recorded path no
 * longer resolves. */
static void test_trace_run_resolves_existing_run(void){
  sqlite3 *db = open_memory_db();
  char base[] = "/tmp/test_summary_trace_XXXXXX";
  char manifest_path[256],output_path[256],plots_dir[256],png_path[512];
  char *buf;
  size_t size;
  FILE *fp,*f;
  int rc;

  {
    int fd = mkstemp(base);
    assert(fd >= 0);
    close(fd);
    unlink(base);
  }
  assert(mkdir(base,0755) == 0);
  snprintf(manifest_path,sizeof(manifest_path),"%s/run.manifest.json",base);
  snprintf(output_path,sizeof(output_path),"%s/run.csv",base);
  snprintf(plots_dir,sizeof(plots_dir),"%s/plots",base);
  snprintf(png_path,sizeof(png_path),"%s/run.topdown.png",plots_dir);

  f = fopen(manifest_path,"w"); assert(f); fclose(f);
  f = fopen(output_path,"w"); assert(f); fclose(f);
  assert(mkdir(plots_dir,0755) == 0);
  f = fopen(png_path,"w"); assert(f); fclose(f);

  insert_run_with_paths(db,1,"r1","host1","/bin/workloadA","2026-01-01T00:00:00Z",
                         manifest_path,output_path,NULL);

  fp = open_memstream(&buf,&size);
  rc = trace_run(db,"host1","r1",fp);
  fclose(fp);

  assert(rc == 0);
  assert(strstr(buf,"command=/bin/workloadA") != NULL);
  assert(strstr(buf,"manifest_exists=1") != NULL);
  assert(strstr(buf,"output_exists=1") != NULL);
  assert(strstr(buf,"tree_output_path=\n") != NULL); /* NULL in the store -- no tree pass this run */
  assert(strstr(buf,"tree_exists=0") != NULL);
  assert(strstr(buf,"plots_exist=1") != NULL);
  assert(strstr(buf,"plots_count=1") != NULL);

  free(buf);
  sqlite3_close(db);
  unlink(png_path);
  rmdir(plots_dir);
  unlink(manifest_path);
  unlink(output_path);
  rmdir(base);
  printf("test_trace_run_resolves_existing_run passed\n");
}

static void test_trace_run_stale_paths_degrade_not_fail(void){
  sqlite3 *db = open_memory_db();
  char *buf;
  size_t size;
  FILE *fp;
  int rc;

  /* Recorded at ingest time, but the files no longer exist here -- e.g. a
   * run-index copied in from a different host (doc/ARTIFACT_CONTRACT.md's
   * "Normalized store" section notes this is the common case for
   * multi-host aggregation). Must report exists=0, not fail the lookup. */
  insert_run_with_paths(db,1,"r1","host1","/bin/workloadA","2026-01-01T00:00:00Z",
                         "/nonexistent/run.manifest.json","/nonexistent/run.csv",
                         "/nonexistent/run.tree.txt");

  fp = open_memstream(&buf,&size);
  rc = trace_run(db,"host1","r1",fp);
  fclose(fp);

  assert(rc == 0);
  assert(strstr(buf,"manifest_exists=0") != NULL);
  assert(strstr(buf,"output_exists=0") != NULL);
  assert(strstr(buf,"tree_exists=0") != NULL);
  assert(strstr(buf,"plots_exist=0") != NULL);
  assert(strstr(buf,"plots_count=0") != NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_trace_run_stale_paths_degrade_not_fail passed\n");
}

/* A bare relative output_path (no '/' at all) has no directory of its own
 * to derive a sibling "plots" path from -- must degrade to "can't tell"
 * rather than guessing a "plots" path relative to wherever wspy-summary's
 * own cwd happens to be, which could silently attribute an unrelated
 * directory's contents to this run. */
static void test_trace_run_relative_output_path_skips_plots_guess(void){
  sqlite3 *db = open_memory_db();
  char *buf;
  size_t size;
  FILE *fp;
  int rc;

  insert_run_with_paths(db,1,"r1","host1","/bin/workloadA","2026-01-01T00:00:00Z",
                         NULL,"run.csv",NULL);

  fp = open_memstream(&buf,&size);
  rc = trace_run(db,"host1","r1",fp);
  fclose(fp);

  assert(rc == 0);
  assert(strstr(buf,"output_path=run.csv\n") != NULL);
  assert(strstr(buf,"plots_dir=\n") != NULL);
  assert(strstr(buf,"plots_exist=0\n") != NULL);
  assert(strstr(buf,"plots_count=0\n") != NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_trace_run_relative_output_path_skips_plots_guess passed\n");
}

static void test_summarize_show_runs_truncates_with_marker(void){
  sqlite3 *db = open_memory_db();
  struct summary_opts opts = default_opts();
  struct summary_totals totals;
  char *buf;
  size_t size;
  FILE *fp;
  int i;

  /* Enough contributing runs that the hostname:run_id list can't fit in
   * contributing_runs' 4096-byte buffer (format_contributing_runs()) --
   * must report a "(+N more)" marker instead of silently dropping the
   * tail, unlike outlier_ids' pre-existing (rare-in-practice) truncation. */
  for (i = 0; i < 400; i++){
    char run_id[32],start_time[32];
    snprintf(run_id,sizeof(run_id),"r%d",i);
    snprintf(start_time,sizeof(start_time),"2026-01-01T00:%02d:00Z",i % 60);
    insert_run(db,i + 1,run_id,"host-with-a-fairly-long-name","/bin/workloadA",NULL,start_time);
    insert_metric(db,i + 1,"ipc",1.0);
  }

  opts.csvflag = 1;
  opts.show_runs = 1;
  memset(&totals,0,sizeof(totals));
  fp = open_memstream(&buf,&size);
  assert(summarize(db,&opts,fp,&totals) == 0);
  fclose(fp);

  assert(strstr(buf,"more)") != NULL);

  free(buf);
  sqlite3_close(db);
  printf("test_summarize_show_runs_truncates_with_marker passed\n");
}

static void test_trace_run_no_such_run(void){
  sqlite3 *db = open_memory_db();
  char *buf;
  size_t size;
  FILE *fp;
  int rc;

  insert_run(db,1,"r1","host1","/bin/workloadA",NULL,"2026-01-01T00:00:00Z");

  fp = open_memstream(&buf,&size);
  rc = trace_run(db,"host1","no-such-run",fp);
  fclose(fp);

  assert(rc == 1);
  assert(strlen(buf) == 0); /* nothing printed to out on a miss -- the "not found" is stderr's job */

  free(buf);
  sqlite3_close(db);
  printf("test_trace_run_no_such_run passed\n");
}

static void test_open_summary_db_schema_gate(void){
  char path[] = "/tmp/test_summary_db_XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  unlink(path); /* let sqlite create it fresh on the next open */

  {
    sqlite3 *setup;
    assert(sqlite3_open(path,&setup) == SQLITE_OK);
    assert(sqlite3_exec(setup,
      "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT, hostname TEXT, command TEXT, "
      "cpu_vendor TEXT, start_time TEXT);"
      "CREATE TABLE metric_values (id INTEGER PRIMARY KEY, run_id INTEGER, metric_name TEXT, value REAL);"
      "PRAGMA user_version = 1;",
      NULL,NULL,NULL) == SQLITE_OK);
    sqlite3_close(setup);
  }
  assert(open_summary_db(path) == NULL); /* schema version 1 predates metric_values */

  {
    sqlite3 *setup;
    assert(sqlite3_open(path,&setup) == SQLITE_OK);
    assert(sqlite3_exec(setup,"PRAGMA user_version = 2;",NULL,NULL,NULL) == SQLITE_OK);
    sqlite3_close(setup);
  }
  {
    sqlite3 *db = open_summary_db(path);
    assert(db != NULL);
    sqlite3_close(db);
  }

  unlink(path);
  printf("test_open_summary_db_schema_gate passed\n");
}

int main(void){
  test_compute_stats_basic();
  test_compute_stats_single_sample();
  test_compute_stats_even_count_median();
  test_compute_stats_outlier_detected();
  test_compute_stats_two_samples_never_flagged();
  test_t_critical_95_table_and_fallback();
  test_compute_ci95_single_sample_is_zero_width();
  test_compute_ci95_matches_formula();
  test_compute_verdict_pass();
  test_compute_verdict_thin_only();
  test_compute_verdict_noisy_only();
  test_compute_verdict_thin_and_noisy();
  test_compute_verdict_boundary_not_noisy();
  test_compute_verdict_mixed_pmu_only();
  test_compute_verdict_all_three_reasons();
  test_parse_group_by();
  test_metric_wanted();
  test_print_csv_field_quoting();
  test_summarize_averages_per_run_and_groups_by_command();
  test_summarize_command_filter();
  test_summarize_hostname_filter();
  test_summarize_metric_filter();
  test_summarize_min_runs_skips_thin_buckets();
  test_summarize_verdict_pass_low_cv_enough_runs();
  test_summarize_verdict_thin_when_fewer_than_three_runs();
  test_summarize_verdict_noisy_when_cv_exceeds_default_max_cv();
  test_summarize_max_cv_flag_raises_noisy_threshold();
  test_summarize_verdict_thin_and_noisy_both_fire();
  test_summarize_verdict_mixed_pmu_different_vendor();
  test_summarize_verdict_mixed_pmu_different_coverage();
  test_summarize_verdict_same_pmu_signature_not_mixed();
  test_summarize_group_by_affinity_mode();
  test_summarize_group_by_cpu_governor_via_environment_join();
  test_summarize_group_by_option_composes_with_primary_group();
  test_summarize_group_by_option_unset_is_inert();
  test_summarize_group_by_option_missing_value_shows_unknown();
  test_summarize_group_by_cpu_vendor_unknown_bucket();
  test_summarize_null_only_metric_excluded();
  test_summarize_show_runs_lists_contributors();
  test_summarize_without_show_runs_omits_contributors();
  test_trace_run_resolves_existing_run();
  test_trace_run_stale_paths_degrade_not_fail();
  test_trace_run_relative_output_path_skips_plots_guess();
  test_summarize_show_runs_truncates_with_marker();
  test_trace_run_no_such_run();
  test_open_summary_db_schema_gate();

  printf("\nAll test_summary tests passed.\n");
  return 0;
}
