#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_PLOT 1
#include "plot.c"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void test_split_csv_line(void){
  char line[] = "time,retire,frontend,backend,speculate,";
  char *fields[MAX_CSV_FIELDS];
  int n;

  printf("Testing split_csv_line...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  assert(n == 6); /* trailing comma yields a final empty field, same as store.c/validate.c */
  assert(!strcmp(fields[0],"time"));
  assert(!strcmp(fields[4],"speculate"));
  assert(!strcmp(fields[5],""));
  printf("PASS: split_csv_line\n");
}

static void test_topdown_template_matches(void){
  char line[] = "time,retire,frontend,backend,speculate,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing topdown template matching...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  assert(time_col == 0);

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 1);
  assert(!strcmp(matches[0].name,"topdown"));
  assert(matches[0].ncols == 4);
  assert(matches[0].cols[0] == 1); /* retire */
  assert(matches[0].cols[3] == 4); /* speculate */
  assert(claimed[1] && claimed[2] && claimed[3] && claimed[4]);
  printf("PASS: topdown template matches on amdtopdown-shaped header\n");
}

static void test_extra_columns_dont_block_match(void){
  /* A newer wspy adding confidence/sanity after the core four topdown
   * columns must not stop the template from firing -- extra, unclaimed
   * columns should fall through to the generic fallback instead. */
  char line[] = "time,retire,frontend,backend,speculate,confidence,sanity,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing that extra unrelated columns don't block a template match...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 1);
  assert(!strcmp(matches[0].name,"topdown"));
  assert(matches[0].ncols == 4);

  nmatches = add_fallback_match(fields,n,time_col,-1,-1,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 2);
  assert(!strcmp(matches[1].name,"metrics"));
  assert(matches[1].ncols == 2); /* confidence, sanity */
  printf("PASS: extra columns fall through to the generic fallback template\n");
}

static void test_system_cpu_template(void){
  char line[] = "time,load,runnable,cpu,idle,iowait,irq,net lo,net enp2s0,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing system-cpu template matching (systemtime-shaped header)...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 1);
  assert(!strcmp(matches[0].name,"system-cpu"));
  assert(matches[0].ncols == 4); /* cpu, idle, iowait, irq -- not load/runnable */

  nmatches = add_network_fallback_match(fields,n,time_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 2);
  assert(!strcmp(matches[1].name,"network-io"));
  assert(matches[1].ncols == 2); /* net lo, net enp2s0 */

  nmatches = add_fallback_match(fields,n,time_col,-1,-1,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 3);
  assert(!strcmp(matches[2].name,"metrics"));
  assert(matches[2].ncols == 2); /* load, runnable -- net columns already claimed above */
  printf("PASS: system-cpu template matches, network columns split out, leftovers get the fallback\n");
}

static void test_network_fallback_absent_when_no_net_columns(void){
  char line[] = "time,retire,frontend,backend,speculate,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing add_network_fallback_match() is a no-op without any 'net ' column...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 1);
  nmatches = add_network_fallback_match(fields,n,time_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 1); /* unchanged -- no "net " column to claim */
  printf("PASS: no network-io bucket appears when there's nothing to put in it\n");
}

static void test_coverage_columns_excluded_from_fallback(void){
  /* counters_measured/counters_requested (coverage.c's per-row bookkeeping,
   * present regardless of counter group) must never show up as a plotted
   * "metric" -- they're not a per-tick workload measurement. */
  char line[] = "time,retire,frontend,backend,speculate,counters_measured,counters_requested,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing counters_measured/counters_requested are excluded from the generic fallback...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 1); /* topdown */
  nmatches = add_network_fallback_match(fields,n,time_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 1); /* no "net " columns here */
  nmatches = add_fallback_match(fields,n,time_col,-1,-1,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 1); /* counters_measured/counters_requested excluded -- nothing left over */
  printf("PASS: coverage bookkeeping columns never appear in a plot\n");
}

static void test_no_time_column_skipped(void){
  /* An aggregate (non---interval) CSV has no "time" column and is not a
   * time series -- no template should ever be evaluated against it. */
  char line[] = "elapsed,utime,stime,ipc,";
  char *fields[MAX_CSV_FIELDS];
  int n;

  printf("Testing that a header without 'time' has no time column...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  assert(find_col(fields,n,"time") == -1);
  printf("PASS: aggregate-shaped header has no 'time' column\n");
}

static void test_no_match_no_fallback_needed(void){
  /* Only dimension columns present (plus an empty trailing header cell) --
   * nothing to plot, and the fallback correctly finds nothing left over. */
  char line[] = "time,core,phase,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,core_col,phase_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing an all-dimension header produces no plots...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  core_col = find_col(fields,n,"core");
  phase_col = find_col(fields,n,"phase");

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed);
  assert(nmatches == 0);
  nmatches = add_fallback_match(fields,n,time_col,core_col,phase_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 0);
  printf("PASS: an all-dimension header yields no matches, not a spurious empty plot\n");
}

static void test_basename_without_ext(void){
  char out[MAX_NAME_LEN];

  printf("Testing basename_without_ext...\n");
  basename_without_ext("/a/b/amdtopdown.csv",out,sizeof(out));
  assert(!strcmp(out,"amdtopdown"));
  basename_without_ext("systemtime.csv",out,sizeof(out));
  assert(!strcmp(out,"systemtime"));
  printf("PASS: basename_without_ext\n");
}

static void test_process_csv_skips_aggregate_csv(void){
  const char *path = "/tmp/test_plot_aggregate.csv";
  int any_failed = 0;
  int rendered;

  printf("Testing process_csv() skips a 'time'-less (aggregate) CSV...\n");
  write_file(path,"elapsed,utime,stime,ipc,\n 1.234,0.010,0.005,1.23,\n");
  rendered = process_csv(path,"/tmp",1 /* quiet */,&any_failed);
  assert(rendered == 0);
  assert(any_failed == 0);
  remove(path);
  printf("PASS: process_csv skips an aggregate CSV without treating it as a failure\n");
}

static void test_process_csv_empty_file(void){
  const char *path = "/tmp/test_plot_empty.csv";
  int any_failed = 0;
  int rendered;

  printf("Testing process_csv() on an empty file...\n");
  write_file(path,"");
  rendered = process_csv(path,"/tmp",1,&any_failed);
  assert(rendered == 0);
  assert(any_failed == 0);
  remove(path);
  printf("PASS: process_csv tolerates an empty CSV\n");
}

int main(void){
  test_split_csv_line();
  test_topdown_template_matches();
  test_extra_columns_dont_block_match();
  test_system_cpu_template();
  test_network_fallback_absent_when_no_net_columns();
  test_coverage_columns_excluded_from_fallback();
  test_no_time_column_skipped();
  test_no_match_no_fallback_needed();
  test_basename_without_ext();
  test_process_csv_skips_aggregate_csv();
  test_process_csv_empty_file();

  printf("\nAll test_plot tests passed.\n");
  return 0;
}
