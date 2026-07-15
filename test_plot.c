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

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
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

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
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

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
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
  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
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
  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
  assert(nmatches == 1); /* topdown */
  nmatches = add_network_fallback_match(fields,n,time_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 1); /* no "net " columns here */
  nmatches = add_fallback_match(fields,n,time_col,-1,-1,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 1); /* counters_measured/counters_requested excluded -- nothing left over */
  printf("PASS: coverage bookkeeping columns never appear in a plot\n");
}

static void test_ibs_template_matches_memory_deep_shaped_header(void){
  /* ibs_l3missonly/ibs_ldlat_threshold/ibs_fetchlat_threshold are constant
   * per-run filter configuration (topdown.c's print_ibs()), not a per-tick
   * measurement -- they must be excluded from the fallback, not swept into
   * "Other Metrics" as if they varied over time. */
  char line[] = "time,ibs_fetch,ibs_op,ibs_op_unfiltered,ibs_op_accepted_ratio,"
                "ibs_l3missonly,ibs_ldlat_threshold,ibs_fetchlat_threshold,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing ibs/ibs-accepted-ratio template matching on an ibs-memory-deep-shaped header...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
  assert(nmatches == 2);
  assert(!strcmp(matches[0].name,"ibs"));
  assert(matches[0].ncols == 3); /* ibs_fetch, ibs_op, ibs_op_unfiltered */
  assert(!strcmp(matches[1].name,"ibs-accepted-ratio"));
  assert(matches[1].ncols == 1); /* ibs_op_accepted_ratio */

  nmatches = add_fallback_match(fields,n,time_col,-1,-1,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 2); /* l3missonly/ldlat_threshold/fetchlat_threshold excluded, nothing left over */
  printf("PASS: ibs templates match, static filter-config columns excluded from fallback\n");
}

static void test_ibs_template_matches_basic_shaped_header(void){
  /* ibs-basic has no deep-profile extras at all -- the "ibs" template must
   * still fire on just ibs_fetch/ibs_op, and "ibs-accepted-ratio" must not
   * fire since its one candidate column isn't present. */
  char line[] = "time,ibs_fetch,ibs_op,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches;

  printf("Testing ibs template matching on an ibs-basic-shaped header...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
  assert(nmatches == 1);
  assert(!strcmp(matches[0].name,"ibs"));
  assert(matches[0].ncols == 2); /* ibs_fetch, ibs_op */
  printf("PASS: ibs template matches on ibs-basic-shaped header, accepted-ratio template absent\n");
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

  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,0);
  assert(nmatches == 0);
  nmatches = add_fallback_match(fields,n,time_col,core_col,phase_col,claimed,matches,MAX_CSV_FIELDS,nmatches);
  assert(nmatches == 0);
  printf("PASS: an all-dimension header yields no matches, not a spurious empty plot\n");
}

static void test_parse_custom_plot_spec(void){
  struct custom_plot_spec spec;

  printf("Testing parse_custom_plot_spec...\n");
  assert(parse_custom_plot_spec("mygroup=retire,frontend",&spec) == 1);
  assert(!strcmp(spec.name,"mygroup"));
  assert(spec.ncolumns == 2);
  assert(!strcmp(spec.columns[0],"retire"));
  assert(!strcmp(spec.columns[1],"frontend"));

  /* a single-column spec is valid -- one column plotted alone is still
   * a meaningful time series (mirrors the built-in "ipc" template). */
  assert(parse_custom_plot_spec("solo=ipc",&spec) == 1);
  assert(spec.ncolumns == 1);

  /* rejected: no '=', empty name, no columns at all. */
  assert(parse_custom_plot_spec("no-equals-sign",&spec) == 0);
  assert(parse_custom_plot_spec("=retire,frontend",&spec) == 0);
  assert(parse_custom_plot_spec("mygroup=",&spec) == 0);
  printf("PASS: parse_custom_plot_spec\n");
}

static void test_custom_plot_matches_specific_columns(void){
  /* The whole point: let a user group exactly the counters they want,
   * independent of (and alongside) the built-in templates -- e.g. to keep
   * same-scale counters together when they don't match any built-in
   * template's own grouping. "extra_metric" isn't part of any built-in
   * template, so only "topdown" (not this spec's own choice of columns)
   * should additionally fire below. */
  char line[] = "time,retire,frontend,backend,speculate,extra_metric,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  struct custom_plot_spec spec;
  int nmatches;

  printf("Testing add_custom_plot_match() groups exactly the requested columns...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  assert(parse_custom_plot_spec("retire-and-extra=retire,extra_metric",&spec) == 1);

  nmatches = add_custom_plot_match(fields,n,time_col,&spec,claimed,matches,MAX_CSV_FIELDS,0,
                                    "test.csv",1 /* quiet */);
  assert(nmatches == 1);
  assert(!strcmp(matches[0].name,"retire-and-extra"));
  assert(matches[0].ncols == 2);
  assert(claimed[matches[0].cols[0]] && claimed[matches[0].cols[1]]);

  /* the built-in "topdown" template still fires normally afterward --
   * a custom plot is additive, not exclusive, unless --only-custom says
   * otherwise (that's main()'s job, not add_custom_plot_match()'s). */
  nmatches = match_templates(fields,n,time_col,matches,MAX_CSV_FIELDS,claimed,nmatches);
  assert(nmatches == 2);
  assert(!strcmp(matches[1].name,"topdown"));
  printf("PASS: custom plot groups exactly its requested columns and coexists with built-in templates\n");
}

static void test_custom_plot_missing_column_warns_not_fatal(void){
  /* A named column absent from this particular CSV is skipped (with a
   * warning, suppressed here via quiet=1), not fatal -- and if every named
   * column is absent, no plot is produced for that spec at all. */
  char line[] = "time,retire,frontend,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col,claimed[MAX_CSV_FIELDS] = {0};
  struct plot_match matches[MAX_CSV_FIELDS];
  struct custom_plot_spec spec;
  int nmatches;

  printf("Testing add_custom_plot_match() tolerates a missing column...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");

  /* "backend" isn't in this header -- "retire" still fires the plot. */
  assert(parse_custom_plot_spec("partial=retire,backend",&spec) == 1);
  nmatches = add_custom_plot_match(fields,n,time_col,&spec,claimed,matches,MAX_CSV_FIELDS,0,
                                    "test.csv",1);
  assert(nmatches == 1);
  assert(matches[0].ncols == 1);

  /* neither named column exists -- no plot produced. */
  assert(parse_custom_plot_spec("nothing=backend,speculate",&spec) == 1);
  nmatches = add_custom_plot_match(fields,n,time_col,&spec,claimed,matches,MAX_CSV_FIELDS,0,
                                    "test.csv",1);
  assert(nmatches == 0);
  printf("PASS: missing custom-plot columns warn (suppressed here) rather than fail, and an all-missing spec produces nothing\n");
}

static void test_only_custom_skips_builtins_and_fallbacks(void){
  /* build_plot_matches()'s only_custom=1 path: exactly the --plot spec(s),
   * no built-in templates, no network-io/generic fallback -- tested at the
   * matching level (no gnuplot invocation, no filesystem I/O), same as
   * every other matching test in this file. */
  char line[] = "time,retire,frontend,backend,speculate,load,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col;
  struct plot_match matches[MAX_CSV_FIELDS];
  struct custom_plot_spec spec;
  int nmatches;

  printf("Testing build_plot_matches(only_custom=1) yields only the given --plot spec...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  assert(parse_custom_plot_spec("myplot=retire,frontend",&spec) == 1);

  nmatches = build_plot_matches(fields,n,time_col,&spec,1,1 /* only_custom */,
                                 matches,MAX_CSV_FIELDS,"test.csv",1 /* quiet */);
  assert(nmatches == 1); /* not 1 (myplot) + 1 (topdown, same columns) + 1 (fallback for load) */
  assert(!strcmp(matches[0].name,"myplot"));
  assert(matches[0].ncols == 2);
  printf("PASS: --only-custom yields exactly the requested plot(s), nothing else\n");
}

static void test_default_mode_combines_custom_and_builtins(void){
  /* Without --only-custom, build_plot_matches() is additive: the custom
   * spec, the built-in templates, and the fallback all run. */
  char line[] = "time,retire,frontend,backend,speculate,load,";
  char *fields[MAX_CSV_FIELDS];
  int n,time_col;
  struct plot_match matches[MAX_CSV_FIELDS];
  struct custom_plot_spec spec;
  int nmatches;

  printf("Testing build_plot_matches(only_custom=0) combines --plot with built-ins...\n");
  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  time_col = find_col(fields,n,"time");
  assert(parse_custom_plot_spec("myplot=retire,frontend",&spec) == 1);

  nmatches = build_plot_matches(fields,n,time_col,&spec,1,0 /* only_custom */,
                                 matches,MAX_CSV_FIELDS,"test.csv",1);
  assert(nmatches == 3); /* myplot, topdown, metrics (load) */
  assert(!strcmp(matches[0].name,"myplot"));
  assert(!strcmp(matches[1].name,"topdown"));
  assert(!strcmp(matches[2].name,"metrics"));
  printf("PASS: default mode combines the custom plot with built-in templates and the fallback\n");
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
  rendered = process_csv(path,"/tmp",1 /* quiet */,&any_failed,NULL,0,0);
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
  rendered = process_csv(path,"/tmp",1,&any_failed,NULL,0,0);
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
  test_ibs_template_matches_memory_deep_shaped_header();
  test_ibs_template_matches_basic_shaped_header();
  test_no_time_column_skipped();
  test_no_match_no_fallback_needed();
  test_parse_custom_plot_spec();
  test_custom_plot_matches_specific_columns();
  test_custom_plot_missing_column_warns_not_fatal();
  test_only_custom_skips_builtins_and_fallbacks();
  test_default_mode_combines_custom_and_builtins();
  test_basename_without_ext();
  test_process_csv_skips_aggregate_csv();
  test_process_csv_empty_file();

  printf("\nAll test_plot tests passed.\n");
  return 0;
}
