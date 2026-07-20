#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_CORE_REPORT 1
#include "core_report.c"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

/* Resets the file-scope accumulator state core_report.c's read_percore_csv()
 * populates -- each test starts from a clean slate rather than accumulating
 * across tests in the same process. */
static void reset_state(void){
  memset(metrics,0,sizeof(metrics));
  nmetrics = 0;
  memset(core_seen,0,sizeof(core_seen));
  max_core_seen = -1;
}

static void test_split_csv_line(void){
  char line[] = "a,b,c";
  char *fields[8];
  int n;

  printf("Testing split_csv_line...\n");
  n = split_csv_line(line,fields,8);
  assert(n == 3);
  assert(!strcmp(fields[0],"a"));
  assert(!strcmp(fields[1],"b"));
  assert(!strcmp(fields[2],"c"));
  printf("PASS: split_csv_line\n");
}

static void test_parse_numeric_field(void){
  double v;

  printf("Testing parse_numeric_field...\n");
  assert(parse_numeric_field("1.5",&v) && v == 1.5);
  assert(parse_numeric_field("42",&v) && v == 42.0);
  assert(parse_numeric_field("26.61%",&v) && v == 26.61);
  assert(!parse_numeric_field("",&v));
  assert(!parse_numeric_field("abc",&v));
  assert(!parse_numeric_field(NULL,&v));
  printf("PASS: parse_numeric_field\n");
}

static void test_is_dimension_or_excluded_column(void){
  printf("Testing is_dimension_or_excluded_column...\n");
  assert(is_dimension_or_excluded_column("core"));
  assert(is_dimension_or_excluded_column("time"));
  assert(is_dimension_or_excluded_column("phase"));
  assert(is_dimension_or_excluded_column("counters_measured"));
  assert(is_dimension_or_excluded_column("counters_requested"));
  assert(is_dimension_or_excluded_column("ibs_l3missonly"));
  assert(is_dimension_or_excluded_column("")); /* wspy's own trailing-comma artifact */
  assert(!is_dimension_or_excluded_column("ipc"));
  assert(!is_dimension_or_excluded_column("maxrss"));
  printf("PASS: is_dimension_or_excluded_column\n");
}

static void test_compute_core_stats_basic(void){
  double values[] = {1.0, 2.0, 3.0, 4.0};
  struct core_stats st;

  printf("Testing compute_core_stats basic...\n");
  compute_core_stats(values,4,&st);
  assert(st.n == 4);
  assert(st.min == 1.0);
  assert(st.max == 4.0);
  assert(st.mean == 2.5);
  assert(st.cold_idx == 0);
  assert(st.hot_idx == 3);
  assert(st.stddev > 1.29 && st.stddev < 1.30); /* sample stddev, n-1 denom */
  printf("PASS: compute_core_stats basic\n");
}

static void test_compute_core_stats_single_sample(void){
  double values[] = {7.5};
  struct core_stats st;

  printf("Testing compute_core_stats single sample...\n");
  compute_core_stats(values,1,&st);
  assert(st.n == 1);
  assert(st.min == 7.5 && st.max == 7.5 && st.mean == 7.5);
  assert(st.stddev == 0.0); /* n<2: nothing to vary against, not NaN */
  assert(st.cv_percent == 0.0);
  assert(st.hot_idx == 0 && st.cold_idx == 0);
  printf("PASS: compute_core_stats single sample\n");
}

static void test_compute_core_stats_zero_mean(void){
  double values[] = {-1.0, 1.0};
  struct core_stats st;

  printf("Testing compute_core_stats zero mean (cv_percent must not divide by zero)...\n");
  compute_core_stats(values,2,&st);
  assert(st.mean == 0.0);
  assert(st.cv_percent == 0.0); /* guarded, not inf/nan */
  printf("PASS: compute_core_stats zero mean\n");
}

static void test_metric_wanted(void){
  char *filters[] = {"ipc","retire"};

  printf("Testing metric_wanted...\n");
  assert(metric_wanted("ipc",filters,2));
  assert(metric_wanted("retire",filters,2));
  assert(!metric_wanted("maxrss",filters,2));
  assert(metric_wanted("anything",NULL,0)); /* no filter -- everything wanted */
  printf("PASS: metric_wanted\n");
}

static void test_core_class_name(void){
  printf("Testing core_class_name...\n");
  assert(!strcmp(core_class_name(CORE_INTEL_ATOM),"intel_atom"));
  assert(!strcmp(core_class_name(CORE_INTEL_CORE),"intel_core"));
  assert(!strcmp(core_class_name(CORE_AMD_ZEN5),"amd_zen5"));
  assert(!strcmp(core_class_name(CORE_UNKNOWN),"unknown"));
  printf("PASS: core_class_name\n");
}

#define FAKE_CSV_PATH "/tmp/test_core_report.csv"

static void test_read_percore_csv_basic(void){
  int nrows_skipped = 0;

  printf("Testing read_percore_csv basic...\n");
  reset_state();
  write_file(FAKE_CSV_PATH,
    "elapsed,core,ipc,maxrss\n"
    "1.0,0,1.9,1000\n"
    "1.0,1,0.4,2000\n");

  assert(read_percore_csv(FAKE_CSV_PATH,&nrows_skipped) == 0);
  assert(nrows_skipped == 0);
  assert(max_core_seen == 1);
  assert(core_seen[0] && core_seen[1]);
  assert(nmetrics == 3); /* elapsed, ipc, maxrss -- "core" itself is a dimension, not a metric */

  remove(FAKE_CSV_PATH);
  printf("PASS: read_percore_csv basic\n");
}

static void test_read_percore_csv_no_core_column(void){
  int nrows_skipped = 0;

  printf("Testing read_percore_csv rejects a CSV with no 'core' column...\n");
  reset_state();
  write_file(FAKE_CSV_PATH,"a,b,c\n1,2,3\n");

  assert(read_percore_csv(FAKE_CSV_PATH,&nrows_skipped) == -1);

  remove(FAKE_CSV_PATH);
  printf("PASS: read_percore_csv no core column\n");
}

static void test_read_percore_csv_missing_file(void){
  int nrows_skipped = 0;

  printf("Testing read_percore_csv on a missing file...\n");
  reset_state();
  assert(read_percore_csv("/tmp/does-not-exist-core-report-test.csv",&nrows_skipped) == -1);
  printf("PASS: read_percore_csv missing file\n");
}

static void test_read_percore_csv_skips_malformed_rows(void){
  int nrows_skipped = 0;

  printf("Testing read_percore_csv skips malformed rows, not fatal...\n");
  reset_state();
  write_file(FAKE_CSV_PATH,
    "core,ipc\n"
    "0,1.5\n"
    "1,2.5,extra\n"   /* wrong field count -- skipped */
    "notanumber,3.5\n" /* non-numeric core -- skipped */
    "2,3.5\n");

  assert(read_percore_csv(FAKE_CSV_PATH,&nrows_skipped) == 0);
  assert(nrows_skipped == 2);
  assert(core_seen[0] && core_seen[2] && !core_seen[1]);

  remove(FAKE_CSV_PATH);
  printf("PASS: read_percore_csv skips malformed rows\n");
}

static void test_read_percore_csv_header_reuse_bug_regression(void){
  /* Regression test for a real bug caught during development: header_fields[]
   * must not be left pointing into the same line[] buffer fgets() reuses for
   * every data row, or metric names get silently corrupted into whatever the
   * last-read data row's bytes happened to be. */
  int nrows_skipped = 0;
  int found_elapsed = 0,i;

  printf("Testing read_percore_csv doesn't corrupt header names via buffer reuse...\n");
  reset_state();
  write_file(FAKE_CSV_PATH,
    "elapsed,core,ipc\n"
    "1.2,0,1.85\n"
    "1.2,1,0.42\n");

  assert(read_percore_csv(FAKE_CSV_PATH,&nrows_skipped) == 0);
  for (i = 0; i < nmetrics; i++){
    assert(strcmp(metrics[i].name,"1.2") != 0); /* a data value, never a valid metric name */
    if (!strcmp(metrics[i].name,"elapsed")) found_elapsed = 1;
  }
  assert(found_elapsed);

  remove(FAKE_CSV_PATH);
  printf("PASS: read_percore_csv header reuse regression\n");
}

static void test_gather_core_values_no_filter(void){
  struct metric_accum m;
  double values[MAX_CORES];
  int core_ids[MAX_CORES];
  int n;

  printf("Testing gather_core_values without a class filter...\n");
  reset_state();
  memset(&m,0,sizeof(m));
  core_seen[0] = 1; m.sum[0] = 10.0; m.count[0] = 2; /* mean 5.0 */
  core_seen[1] = 1; m.sum[1] = 3.0;  m.count[1] = 1; /* mean 3.0 */
  max_core_seen = 1;

  n = gather_core_values(&m,NULL,NULL,0,CORE_UNKNOWN,values,core_ids);
  assert(n == 2);
  assert(core_ids[0] == 0 && values[0] == 5.0);
  assert(core_ids[1] == 1 && values[1] == 3.0);
  printf("PASS: gather_core_values no filter\n");
}

static void test_gather_core_values_class_filter(void){
  struct metric_accum m;
  enum cpu_core_type core_class[MAX_CORES];
  int class_known[MAX_CORES];
  double values[MAX_CORES];
  int core_ids[MAX_CORES];
  int n;

  printf("Testing gather_core_values with a class filter (synthetic hybrid host)...\n");
  reset_state();
  memset(&m,0,sizeof(m));
  memset(class_known,0,sizeof(class_known));
  /* cores 0-1: "big"; core 2: "little"; core 3: unclassifiable (different host) */
  core_seen[0] = 1; m.sum[0] = 18.0; m.count[0] = 1; core_class[0] = CORE_INTEL_CORE; class_known[0] = 1;
  core_seen[1] = 1; m.sum[1] = 16.0; m.count[1] = 1; core_class[1] = CORE_INTEL_CORE; class_known[1] = 1;
  core_seen[2] = 1; m.sum[2] = 5.0;  m.count[2] = 1; core_class[2] = CORE_INTEL_ATOM; class_known[2] = 1;
  core_seen[3] = 1; m.sum[3] = 99.0; m.count[3] = 1; class_known[3] = 0;
  max_core_seen = 3;

  n = gather_core_values(&m,core_class,class_known,1,CORE_INTEL_CORE,values,core_ids);
  assert(n == 2);
  assert(core_ids[0] == 0 && core_ids[1] == 1);

  n = gather_core_values(&m,core_class,class_known,1,CORE_INTEL_ATOM,values,core_ids);
  assert(n == 1);
  assert(core_ids[0] == 2 && values[0] == 5.0);
  printf("PASS: gather_core_values class filter\n");
}

static void test_distinct_classes_present(void){
  enum cpu_core_type core_class[MAX_CORES];
  int class_known[MAX_CORES];
  enum cpu_core_type out[8];
  int n;

  printf("Testing distinct_classes_present...\n");
  reset_state();
  memset(class_known,0,sizeof(class_known));
  core_seen[0] = 1; core_class[0] = CORE_INTEL_CORE; class_known[0] = 1;
  core_seen[1] = 1; core_class[1] = CORE_INTEL_CORE; class_known[1] = 1;
  core_seen[2] = 1; core_class[2] = CORE_INTEL_ATOM; class_known[2] = 1;
  core_seen[3] = 1; class_known[3] = 0; /* unclassifiable, must not appear */
  max_core_seen = 3;

  n = distinct_classes_present(core_class,class_known,out,8);
  assert(n == 2);
  assert((out[0] == CORE_INTEL_CORE && out[1] == CORE_INTEL_ATOM) ||
         (out[0] == CORE_INTEL_ATOM && out[1] == CORE_INTEL_CORE));
  printf("PASS: distinct_classes_present\n");
}

static void test_distinct_classes_present_homogeneous(void){
  enum cpu_core_type core_class[MAX_CORES];
  int class_known[MAX_CORES];
  enum cpu_core_type out[8];
  int n;

  printf("Testing distinct_classes_present on a homogeneous host (no class split)...\n");
  reset_state();
  memset(class_known,0,sizeof(class_known));
  core_seen[0] = 1; core_class[0] = CORE_AMD_ZEN5; class_known[0] = 1;
  core_seen[1] = 1; core_class[1] = CORE_AMD_ZEN5; class_known[1] = 1;
  max_core_seen = 1;

  n = distinct_classes_present(core_class,class_known,out,8);
  assert(n == 1);
  printf("PASS: distinct_classes_present homogeneous\n");
}

int main(void){
  test_split_csv_line();
  test_parse_numeric_field();
  test_is_dimension_or_excluded_column();
  test_compute_core_stats_basic();
  test_compute_core_stats_single_sample();
  test_compute_core_stats_zero_mean();
  test_metric_wanted();
  test_core_class_name();
  test_read_percore_csv_basic();
  test_read_percore_csv_no_core_column();
  test_read_percore_csv_missing_file();
  test_read_percore_csv_skips_malformed_rows();
  test_read_percore_csv_header_reuse_bug_regression();
  test_gather_core_values_no_filter();
  test_gather_core_values_class_filter();
  test_distinct_classes_present();
  test_distinct_classes_present_homogeneous();

  printf("\nAll test_core_report tests passed.\n");
  return 0;
}
