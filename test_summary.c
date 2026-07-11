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
  return opts;
}

/* Minimal fixture: only the runs/metric_values columns summary.c actually
 * reads, not store.c's full SCHEMA_DDL -- keeps these tests decoupled
 * from store.c's own schema evolution. */
static sqlite3 *open_memory_db(void){
  sqlite3 *db;
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);
  assert(sqlite3_exec(db,
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL, "
    "command TEXT NOT NULL, cpu_vendor TEXT, start_time TEXT NOT NULL);"
    "CREATE TABLE metric_values (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL, "
    "metric_name TEXT NOT NULL, value REAL);",
    NULL,NULL,NULL) == SQLITE_OK);
  return db;
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
 * awareness is needed to locate it) and parses its numeric fields.
 * Returns 1 if found. */
static int find_csv_row(const char *buf,const char *group,const char *metric,
                         int *n,double *min_v,double *max_v,double *mean_v,
                         double *median_v,double *stddev_v,double *cv,int *outlier_count){
  char prefix[256];
  const char *line = buf;
  size_t prefix_len;

  snprintf(prefix,sizeof(prefix),"%s,%s,",group,metric);
  prefix_len = strlen(prefix);
  while (line && *line){
    const char *eol = strchr(line,'\n');
    size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
    if (linelen >= prefix_len && !strncmp(line,prefix,prefix_len)){
      sscanf(line + prefix_len,"%d,%lf,%lf,%lf,%lf,%lf,%lf,%d,",
             n,min_v,max_v,mean_v,median_v,stddev_v,cv,outlier_count);
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

static void test_parse_group_by(void){
  enum group_by g;

  assert(parse_group_by("command",&g) == 1 && g == GROUP_COMMAND);
  assert(parse_group_by("hostname",&g) == 1 && g == GROUP_HOSTNAME);
  assert(parse_group_by("cpu_vendor",&g) == 1 && g == GROUP_CPU_VENDOR);
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
  double min_v,max_v,mean_v,median_v,stddev_v,cv;

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

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,&outliers));
  assert(n == 3);
  assert(fabs(min_v - 1.0) < 1e-9);
  assert(fabs(max_v - 1.2) < 1e-9);
  assert(fabs(mean_v - 1.1) < 1e-9);

  assert(find_csv_row(buf,"/bin/workloadA","cache_miss",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,&outliers));
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
  double min_v,max_v,mean_v,median_v,stddev_v,cv;

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

  assert(find_csv_row(buf,"/bin/workloadA","ipc",&n,&min_v,&max_v,&mean_v,&median_v,&stddev_v,&cv,&outliers));
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
  test_parse_group_by();
  test_metric_wanted();
  test_print_csv_field_quoting();
  test_summarize_averages_per_run_and_groups_by_command();
  test_summarize_command_filter();
  test_summarize_hostname_filter();
  test_summarize_metric_filter();
  test_summarize_min_runs_skips_thin_buckets();
  test_summarize_group_by_cpu_vendor_unknown_bucket();
  test_summarize_null_only_metric_excluded();
  test_open_summary_db_schema_gate();

  printf("\nAll test_summary tests passed.\n");
  return 0;
}
