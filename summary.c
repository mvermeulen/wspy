/*
 * summary.c - wspy-summary: generates a per-metric summary table (min/max/
 * mean/median/stddev/outlier flags) across repeated wspy runs recorded in
 * a normalized store (wspy-store --db <path>) -- INVESTIGATION.md's
 * 4.1 Tier 1 "summary table generator (min/max/median/mean/stddev/outlier
 * flags) from indexed data" item, closing the "a summary page can be
 * regenerated from data only (no manual copy/paste)" criterion deferred
 * from 4.0 (see that doc's "Success criteria for a 4.0 kickoff").
 *
 * Reads store.c's metric_values/runs tables directly via SQL rather than
 * re-parsing run-index/CSV files itself -- the normalized store already
 * did the CSV-shape-independent parsing (aggregate/--interval/--per-core
 * all land in the same long/tall metric_values table), so this tool's
 * only job is the across-run statistics layer on top of it. Opens the
 * database read-only: this is a query/report tool, never a writer.
 *
 * For each run contributing to a group, per-run values are first
 * collapsed to one number via AVG(value) over that run's rows for a given
 * metric -- a no-op for the common single-row aggregate CSV shape, and
 * exactly the right collapse for --interval (average over ticks) or
 * --per-core (average over cores) shaped runs, without needing to know
 * which shape produced the data. Statistics (min/max/mean/median/stddev,
 * a z-score outlier flag) are then computed across those per-run values
 * within each (group, metric) bucket, where "group" is workload command,
 * hostname, or cpu_vendor (--group-by) -- so "how much does this metric
 * vary across repeated runs of this workload," the actual point of a
 * summary table, has a real answer.
 *
 * Like ledger.c/validate.c, degrades gracefully rather than failing hard:
 * a (group,metric) bucket with too few contributing runs to trust
 * (--min-runs) is skipped (counted, not fatal), and a database whose
 * PRAGMA user_version is newer than METRIC_VALUES_MIN_SCHEMA_VERSION
 * still works as long as the columns this tool reads are still present --
 * store.c's own MINOR-vs-MAJOR versioning convention (see CLAUDE.md's
 * "New normalized-store field") never removes/renames a column on a MINOR
 * bump, only adds one.
 *
 * --show-runs and --trace are the "Traceability links (summary row ->
 * manifest -> raw CSV -> plots -> tree artifacts)" item (INVESTIGATION.md's
 * "What shipped in 4.1"), closing the other criterion deferred from 4.0 ("every
 * published benchmark row can be traced back to command line, environment,
 * and raw artifacts" -- see "Success criteria for a 4.0 kickoff"). --show-runs
 * appends the hostname:run_id of every run contributing to a bucket (not just
 * outliers -- outlier_run_ids already covered those) so a surprising summary
 * number has a concrete run identity to chase; --trace <hostname>:<run_id>
 * takes one of those identities and resolves it, straight out of the
 * store's own runs.{manifest_path,output_path,tree_output_path} columns
 * store.c already populated at ingest time, to the actual command line,
 * environment (via the manifest), raw CSV, tree artifact, and (derived --
 * wspy-plot writes into <rundir>/plots, a sibling of the CSV, not a stored
 * column of its own) plots directory -- the rest of the chain this item
 * closes. Neither mode requires the other -- --trace works against any
 * hostname:run_id already known by other means (e.g. from a run-index file
 * directly), and --show-runs is useful on its own as a "which runs made up
 * this row" audit even without immediately tracing one.
 *
 * ci95_low/ci95_high (a 95% confidence interval of the mean, Student's t,
 * compute_ci95()) and verdict (PASS, or WARN:thin/WARN:noisy/WARN:thin,noisy,
 * plus a combinable "mixed-pmu" reason -- see below -- compute_verdict())
 * are 4.2's "Repeatability policy + confidence metadata"
 * item -- default output for every reported bucket alongside the pre-existing
 * mean/stddev/cv_percent, no flag needed. "thin" means n < VERDICT_MIN_RUNS_
 * FOR_CONFIDENCE (3, the same threshold outlier flagging already uses);
 * "noisy" means cv_percent exceeds --max-cv (default 5.0, a single global
 * threshold, not per-metric). --strict now also fails on any WARN verdict,
 * matching wspy-validate's own --strict convention. See INVESTIGATION.md's
 * "Repeatability policy + confidence metadata deep-dive" for the full design,
 * including a caveat that a workload wrapping its own multi-trial benchmark
 * harness (Phoronix's adaptive-N, SPEC's fixed multi-run high/low/median) can
 * trigger WARN:noisy from the harness's own internal repeat-count variance as
 * much as from real measurement noise -- this tool has no visibility into
 * what happens inside the wrapped child process to tell the two apart, and
 * deliberately doesn't try to (stays harness-agnostic, same as everywhere
 * else in this tool).
 *
 * "mixed-pmu" (INVESTIGATION.md's 4.2 Tier 1, "PMU-capability-aware
 * comparability warnings" item) is a distinct verdict reason, combinable
 * with thin/noisy, for a different kind of untrustworthy bucket: not noisy
 * data, but data that was never measured the same way in the first place.
 * "PMU capability" here is deliberately coverage.c's own vocabulary (see
 * CLAUDE.md's coverage.c entry) -- runs.cpu_vendor/counters_requested/
 * counters_measured, already stored per run by store.c, no new schema. A
 * bucket is flagged when its contributing runs don't all share the same
 * (cpu_vendor, counters_requested, counters_measured) triple: different
 * cpu_vendor means the same-named CSV column (e.g. "retire") was likely
 * computed from genuinely different raw hardware events -- see topdown.c's
 * per-vendor formula tables -- not just a different machine; different
 * counters_requested means the runs weren't even asking for the same
 * counters (e.g. one used --topdown, another --topdown2, for the same
 * "retire" column name); different counters_measured (with requested held
 * equal) means one run's counter setup degraded (permission denial,
 * multiplexing) while another's didn't. wspy-validate already warns about
 * partial coverage *within* one run; this is the complementary check this
 * codebase had no visibility into before -- a bucket blending a fully-
 * measured run with a degraded one, which validate.c (never aggregates
 * across runs) can't see. Deliberately exact-match, not a numeric
 * closeness threshold like --max-cv's noisy check: there's no principled
 * "how different is different enough" for a coverage triple the way there
 * is for a percentage, so any deviation from the bucket's first-seen
 * signature flags it (see bucket_add()/struct bucket's pmu_* fields).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <dirent.h>

/* metric_values didn't exist before store.c's schema version 2
 * (MIGRATION_V1_TO_V2 in store.c) -- a database older than that has
 * nothing for this tool to summarize. */
#define METRIC_VALUES_MIN_SCHEMA_VERSION 2

/* Repeatability-verdict "thin" threshold -- reuses, rather than invents, the
 * exact n>=3 threshold compute_stats() already applies to outlier flagging
 * ("flagging with fewer samples has no real meaning"). Independent of
 * --min-runs: --min-runs controls whether a bucket is shown at all, this
 * controls whether it's shown *with* a confidence caveat attached. See
 * INVESTIGATION.md's "Repeatability policy + confidence metadata deep-dive". */
#define VERDICT_MIN_RUNS_FOR_CONFIDENCE 3

/* "mixed-env" verdict tracking -- INVESTIGATION.md's 4.3 "Machine/environment
 * comparability scoring": the deferred, *scored* version of "mixed-pmu"
 * above, across run_environment's provenance surface rather than just
 * (cpu_vendor,counters_requested,counters_measured). Deliberately excludes
 * cpu_scaling_driver (redundant companion to cpu_governor, provenance.h's
 * own "not a separate probe" framing), cpu_governor_uniform (a within-run
 * property of the governor measurement, not a cross-run comparability
 * axis), and compiler_version/libc_version (wspy's own build toolchain, not
 * the machine being benchmarked -- the backlog's own field list never
 * mentions them). Every field contributes equally to the score -- no
 * hand-tuned per-field point values, the same reasoning that kept IBS
 * sampling-mode's cross-scheme data-source category mapping out of a
 * permanent CSV schema applies here too: an invented weighting scheme would
 * be exactly as arbitrary and just as hard to walk back once shipped. */
enum env_field {
  ENV_VIRT_ROLE, ENV_HYPERVISOR_VENDOR, ENV_MICROCODE_VERSION,
  ENV_BIOS_VENDOR, ENV_BIOS_VERSION, ENV_BIOS_DATE,
  ENV_CPU_GOVERNOR, ENV_MEMORY_TOTAL_KB,
  ENV_FIELD_COUNT
};

/* run_environment column name for each enum value -- drives both the SQL
 * SELECT list (summarize()/load_baseline_buckets()) and env_score's field
 * loop from one table instead of two hand-kept lists. */
static const char *const ENV_FIELD_COLUMN[ENV_FIELD_COUNT] = {
  "virt_role","hypervisor_vendor","microcode_version",
  "bios_vendor","bios_version","bios_date","cpu_governor","memory_total_kb"
};

/* memory_total_kb needs a tolerance, not exact match: firmware-reserved
 * memory and DIMM population legitimately vary a few percent between
 * otherwise-identical machines (e.g. /proc/meminfo's MemTotal on nominally-
 * identical boxes commonly differs by tens of MB across BIOS/firmware
 * revisions) -- exact match would produce false "mixed-env" noise. 5%
 * tolerates that routine jitter while still catching a genuinely different
 * memory configuration (e.g. 16GB vs 32GB). */
#define ENV_MEMORY_TOLERANCE_FRACTION 0.05

/* command/hostname/cpu_vendor are the original 4.1 whitelist; affinity_mode/
 * preset_name/config_name (runs columns) and cpu_governor/virt_role
 * (run_environment columns, already ingested since 4.0's provenance.c but
 * never groupable until now) are INVESTIGATION.md's "Comparison matrix mode
 * deep-dive" -- piece 1, making sweep/provenance data actually comparable.
 * See --group-by-option below for the complementary open-ended case
 * (an arbitrary --config-option key), which this fixed enum deliberately
 * does not try to cover. */
enum group_by { GROUP_COMMAND, GROUP_HOSTNAME, GROUP_CPU_VENDOR,
                 GROUP_AFFINITY_MODE, GROUP_PRESET_NAME, GROUP_CONFIG_NAME,
                 GROUP_CPU_GOVERNOR, GROUP_VIRT_ROLE };

struct summary_opts {
  const char *db_path;
  const char *command_filter;   /* substring match against runs.command, "" = no filter */
  const char *hostname_filter;  /* exact match against runs.hostname, "" = no filter */
  const char *metrics[64];
  int nmetrics;                 /* 0 = all metrics */
  enum group_by group_by;
  const char *group_by_option;  /* --group-by-option <name>: composed secondary grouping axis,
                                  * an arbitrary run_config_options.option_name (NULL = none) */
  double outlier_z;
  double max_cv;      /* --max-cv: CV percent threshold above which a bucket's verdict is WARN:noisy */
  double min_env_score; /* --min-env-score: below this fraction (0.0-1.0), a bucket's verdict is WARN:mixed-env */
  int min_runs;
  int csvflag;
  int quiet;
  int strict;
  int show_runs;         /* --show-runs: append every contributing hostname:run_id to a bucket */
  const char *trace_key; /* --trace <hostname>:<run_id>: standalone artifact-resolution mode */
  const char *check_regression_key; /* --check-regression <hostname>:<run_id>: standalone
                                      * baseline/anomaly-check mode, see check_regression() */
};

/* --check-regression's target-run identity: which bucket (per --group-by/--group-by-option)
 * the named run belongs to, and its own runs.id/start_time -- resolved once by
 * resolve_target_run() and then used both to build the baseline query (strictly-earlier runs
 * sharing this same bucket) and to know which "runs.id" load_target_metrics() should read. The
 * *_isnull flags matter because plain SQL "=" never matches NULL -- the baseline query below
 * binds NULL (not an empty string) for a NULL bucket key, using "IS ?" instead of "= ?". */
struct target_run {
  sqlite3_int64 id;
  char start_time[64];
  int group_val_isnull;
  char group_val[160];
  int secondary_val_isnull;
  char secondary_val[160];
};

/* Target run's own metric_name -> AVG(value) map, loaded once by load_target_metrics() and
 * consulted per baseline bucket in check_regression(). Same growable-array idiom as
 * struct bucket's values/run_ids/hostnames arrays (bucket_add(), below). */
struct metric_value_map {
  char **names;
  double *values;
  int n,cap;
};

static void metric_map_add(struct metric_value_map *m,const char *name,double value){
  if (m->n == m->cap){
    m->cap = m->cap ? m->cap * 2 : 8;
    m->names = realloc(m->names,sizeof(char *) * (size_t)m->cap);
    m->values = realloc(m->values,sizeof(double) * (size_t)m->cap);
  }
  m->names[m->n] = strdup(name);
  m->values[m->n] = value;
  m->n++;
}

static void metric_map_free(struct metric_value_map *m){
  int i;
  for (i = 0; i < m->n; i++) free(m->names[i]);
  free(m->names);
  free(m->values);
}

/* Linear scan -- metric_value_map is per-run (typically a few dozen metrics at most), not worth
 * a hash table. Returns 1 and sets *value_out if found, else 0. */
static int metric_map_find(const struct metric_value_map *m,const char *name,double *value_out){
  int i;
  for (i = 0; i < m->n; i++){
    if (!strcmp(m->names[i],name)){ *value_out = m->values[i]; return 1; }
  }
  return 0;
}

struct summary_totals {
  int groups_reported;
  int groups_skipped_min_runs;
  int groups_warned;   /* of groups_reported, how many carried a non-PASS verdict */
  int rows_scanned;
};

/* Returns a fully-qualified column reference ("r.command", "e.cpu_governor")
 * -- the table alias varies (r=runs, e=run_environment), so unlike the
 * pre-4.2 version of this function the caller no longer supplies its own
 * "r." prefix. */
static const char *group_by_column(enum group_by g){
  switch (g){
  case GROUP_HOSTNAME: return "r.hostname";
  case GROUP_CPU_VENDOR: return "r.cpu_vendor";
  case GROUP_AFFINITY_MODE: return "r.affinity_mode";
  case GROUP_PRESET_NAME: return "r.preset_name";
  case GROUP_CONFIG_NAME: return "r.config_name";
  case GROUP_CPU_GOVERNOR: return "e.cpu_governor";
  case GROUP_VIRT_ROLE: return "e.virt_role";
  case GROUP_COMMAND: default: return "r.command";
  }
}

/* Whitelist, not user SQL -- this is what makes it safe to interpolate
 * group_by_column()'s return value directly into a query string below.
 * --group-by-option's value is deliberately NOT handled here -- it names an
 * arbitrary, front-end-invented --config-option key, not one of a fixed set
 * of columns, so it's always passed as a bound query parameter instead (see
 * summarize()), never through this interpolation path. */
static int parse_group_by(const char *s,enum group_by *out){
  if (!strcmp(s,"command")){ *out = GROUP_COMMAND; return 1; }
  if (!strcmp(s,"hostname")){ *out = GROUP_HOSTNAME; return 1; }
  if (!strcmp(s,"cpu_vendor")){ *out = GROUP_CPU_VENDOR; return 1; }
  if (!strcmp(s,"affinity_mode")){ *out = GROUP_AFFINITY_MODE; return 1; }
  if (!strcmp(s,"preset_name")){ *out = GROUP_PRESET_NAME; return 1; }
  if (!strcmp(s,"config_name")){ *out = GROUP_CONFIG_NAME; return 1; }
  if (!strcmp(s,"cpu_governor")){ *out = GROUP_CPU_GOVERNOR; return 1; }
  if (!strcmp(s,"virt_role")){ *out = GROUP_VIRT_ROLE; return 1; }
  return 0;
}

static int metric_wanted(const struct summary_opts *opts,const char *name){
  int i;
  if (opts->nmetrics == 0) return 1;
  for (i = 0; i < opts->nmetrics; i++) if (!strcmp(opts->metrics[i],name)) return 1;
  return 0;
}

static int cmp_double(const void *a,const void *b){
  double da = *(const double *)a,db = *(const double *)b;
  if (da < db) return -1;
  if (da > db) return 1;
  return 0;
}

/* Sample statistics over an unordered array of per-run values (n>=1).
 * stddev uses the sample (n-1) denominator; for n<2 there is nothing to
 * vary against, so stddev is reported as 0 rather than NaN/undefined, by
 * convention. outlier_flags (caller-allocated, length n) is set to 1 for
 * any value whose |z-score| exceeds outlier_z -- requires n>=3 and
 * stddev>0, since flagging with fewer samples has no real meaning (with
 * two points, one is trivially "further from the mean" than the other).
 * Returns the outlier count. */
static int compute_stats(const double *values,int n,double outlier_z,
                          double *min_out,double *max_out,double *mean_out,
                          double *median_out,double *stddev_out,int *outlier_flags){
  double *sorted;
  double sum = 0,mean,sumsq = 0,stddev;
  int i,outlier_count = 0;

  sorted = malloc(sizeof(double) * (size_t)n);
  memcpy(sorted,values,sizeof(double) * (size_t)n);
  qsort(sorted,(size_t)n,sizeof(double),cmp_double);

  for (i = 0; i < n; i++) sum += values[i];
  mean = sum / n;
  for (i = 0; i < n; i++) sumsq += (values[i] - mean) * (values[i] - mean);
  stddev = (n >= 2) ? sqrt(sumsq / (n - 1)) : 0.0;

  *min_out = sorted[0];
  *max_out = sorted[n-1];
  *mean_out = mean;
  *median_out = (n % 2) ? sorted[n/2] : (sorted[n/2 - 1] + sorted[n/2]) / 2.0;
  *stddev_out = stddev;

  for (i = 0; i < n; i++){
    int flagged = 0;
    if (n >= 3 && stddev > 0){
      double z = fabs(values[i] - mean) / stddev;
      flagged = (z > outlier_z);
    }
    outlier_flags[i] = flagged;
    if (flagged) outlier_count++;
  }
  free(sorted);
  return outlier_count;
}

/* Two-tailed 95% critical values of Student's t, indexed by df=n-1 for
 * df=1..T95_TABLE_MAX_DF; df beyond the table falls back to the normal
 * approximation z=1.96 rather than growing the table further -- t and z are
 * close enough there, and wspy-level repeat counts this high are rare in
 * practice. No stats library is linked (summary.c only pulls in sqlite3/
 * math.h), so this is a small hardcoded table, the same idiom as
 * validate.c's sanity_bounds[] or topdown.c's event tables, not a
 * general-purpose distribution routine. */
#define T95_TABLE_MAX_DF 30
static const double t95_table[T95_TABLE_MAX_DF] = {
  12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
   2.201, 2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
   2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042,
};

static double t_critical_95(int df){
  if (df < 1) return 0.0; /* compute_ci95() never reaches this for n<2 */
  if (df <= T95_TABLE_MAX_DF) return t95_table[df - 1];
  return 1.96;
}

/* 95% CI of the mean via mean +/- t_critical_95(n-1) * stddev/sqrt(n). n<2
 * (stddev==0 by compute_stats()'s own "nothing to vary against" convention)
 * degenerates to a zero-width interval without consulting the table, since
 * df=0 has no entry -- the formula would produce the same result anyway
 * once stddev is 0, this just avoids needing a t(df=0) lookup. Not
 * configurable (no --confidence-level) -- matches compute_stats()'s own
 * stddev being fixed at sample (n-1) with no user-facing knob. */
static void compute_ci95(double mean,double stddev,int n,double *ci_low_out,double *ci_high_out){
  double margin;

  if (n < 2){
    *ci_low_out = mean;
    *ci_high_out = mean;
    return;
  }
  margin = t_critical_95(n - 1) * stddev / sqrt((double)n);
  *ci_low_out = mean - margin;
  *ci_high_out = mean + margin;
}

/* Repeatability-policy verdict for an emitted bucket: "PASS", or "WARN:"
 * plus a comma-separated reason list. "thin" is n < VERDICT_MIN_RUNS_FOR_
 * CONFIDENCE; "noisy" is cv_percent exceeding the (single, global,
 * --max-cv-controlled) threshold -- not per-metric, see validate.c's
 * per-column sanity_bounds[] for the precedent a future per-metric override
 * table would follow, deliberately not built here without real data to
 * justify differentiated thresholds. Caveat (INVESTIGATION.md's
 * "Repeatability policy + confidence metadata deep-dive"): a workload that
 * wraps its own multi-trial benchmark harness (Phoronix's adaptive-N,
 * SPEC's fixed multi-run high/low/median) can trigger WARN:noisy from the
 * harness's own internal repeat-count variance as much as from real
 * measurement noise -- wspy has no visibility into what happens inside the
 * wrapped child process to tell the two apart. "mixed_pmu" is a distinct,
 * independently-combinable reason -- see this file's header comment on
 * "mixed-pmu" for what it means and why it's exact-match rather than a
 * numeric threshold like noisy's. "env_score"/"min_env_score" are the scored
 * counterpart (see ENV_FIELD_COUNT's comment): env_score == -1.0 (no field
 * was ever mutually comparable) never trips "mixed-env", regardless of
 * min_env_score -- absence of data is not evidence of a mismatch. Reasons
 * are joined in a fixed order (thin, noisy, mixed-pmu, mixed-env) regardless
 * of which combination fired -- new reasons append at the end so existing
 * verdict strings/assertions never need reordering -- so the verdict string
 * is deterministic rather than depending on evaluation order. */
static void compute_verdict(int n,double cv_percent,double max_cv,int mixed_pmu,
                             double env_score,double min_env_score,
                             char *verdict_out,size_t verdict_size){
  int thin = (n < VERDICT_MIN_RUNS_FOR_CONFIDENCE);
  int noisy = (cv_percent > max_cv);
  int mixed_env = (env_score >= 0.0 && env_score < min_env_score);
  size_t used;

  if (!thin && !noisy && !mixed_pmu && !mixed_env){ snprintf(verdict_out,verdict_size,"PASS"); return; }
  snprintf(verdict_out,verdict_size,"WARN:");
  used = strlen(verdict_out);
  if (thin) used += (size_t)snprintf(verdict_out+used,verdict_size-used,"thin");
  if (noisy) used += (size_t)snprintf(verdict_out+used,verdict_size-used,"%snoisy",
                                       used > 5 ? "," : "");
  if (mixed_pmu) used += (size_t)snprintf(verdict_out+used,verdict_size-used,"%smixed-pmu",
                                           used > 5 ? "," : "");
  if (mixed_env) used += (size_t)snprintf(verdict_out+used,verdict_size-used,"%smixed-env",
                                           used > 5 ? "," : "");
}

static void print_csv_field(FILE *out,const char *s){
  int needs_quote = (strchr(s,',') != NULL) || (strchr(s,'"') != NULL);
  const char *p;

  if (!needs_quote){ fputs(s,out); return; }
  fputc('"',out);
  for (p = s; *p; p++){
    if (*p == '"') fputc('"',out);
    fputc(*p,out);
  }
  fputc('"',out);
}

/* One (group, metric) bucket accumulated while streaming rows out of
 * summarize()'s query, which is already sorted by (group_val,metric_name)
 * -- so a bucket is always a contiguous run of query rows, never
 * revisited once closed. */
struct bucket {
  char group_val[160];
  char secondary_val[160]; /* --group-by-option's composed axis; unused (empty) unless opts->group_by_option is set */
  char metric_name[192];
  double *values;
  char **run_ids;
  char **hostnames;
  int n,cap;
  /* "mixed-pmu" verdict tracking (see this file's header comment): the
   * (cpu_vendor,counters_requested,counters_measured) signature of the
   * first contributing run, and whether any later run's signature
   * differed. pmu_signature_set guards the first add specifically (there's
   * no "no value yet" sentinel for an arbitrary vendor string the way 0
   * would work for the int fields alone). */
  char pmu_vendor[64];
  int pmu_requested,pmu_measured;
  int pmu_signature_set;
  int pmu_mixed;
  /* "mixed-env" verdict tracking (see ENV_FIELD_COUNT's comment above): the
   * first-contributing-run's value per tracked field becomes that field's
   * signature (env_sig[f]/env_sig_set[f]); every later run that has a
   * comparable value for that field grows env_compared[f] (the score's
   * per-field denominator) and, on disagreement, sets env_mismatched[f].
   * memory_total_kb's signature/comparison values are decimal strings,
   * atof()'d at comparison time -- same char[] style as pmu_vendor, no
   * separate double field needed. */
  char env_sig[ENV_FIELD_COUNT][80];
  int env_sig_set[ENV_FIELD_COUNT];
  int env_compared[ENV_FIELD_COUNT];
  int env_mismatched[ENV_FIELD_COUNT];
};

static void bucket_reset(struct bucket *b,const char *group_val,const char *secondary_val,
                          const char *metric_name){
  b->n = 0;
  b->cap = 0;
  b->values = NULL;
  b->run_ids = NULL;
  b->hostnames = NULL;
  snprintf(b->group_val,sizeof(b->group_val),"%s",group_val);
  snprintf(b->secondary_val,sizeof(b->secondary_val),"%s",secondary_val);
  snprintf(b->metric_name,sizeof(b->metric_name),"%s",metric_name);
  b->pmu_vendor[0] = '\0';
  b->pmu_requested = 0;
  b->pmu_measured = 0;
  b->pmu_signature_set = 0;
  b->pmu_mixed = 0;
  memset(b->env_sig,0,sizeof(b->env_sig));
  memset(b->env_sig_set,0,sizeof(b->env_sig_set));
  memset(b->env_compared,0,sizeof(b->env_compared));
  memset(b->env_mismatched,0,sizeof(b->env_mismatched));
}

/* Environment comparability score for a bucket: fraction of ENV_FIELD_COUNT
 * tracked fields that agreed across contributing runs, counting only fields
 * that were actually mutually comparable (env_compared[f] > 0 -- a field
 * unavailable on every run but the first never counts at all). Returns
 * -1.0 when NO field was ever mutually comparable -- distinct from a real
 * 0.0 ("compared >=1 field, all mismatched"), the same "absence of data is
 * not itself evidence of a mismatch" principle check_regression()'s own
 * no-baseline status already follows. Callers must never display -1.0 as a
 * measured 0%/0.0 -- see print_regression_row()/emit_bucket()'s env_score_buf
 * "-" handling. */
static double compute_env_score(const struct bucket *b){
  int f,denom = 0,agree = 0;
  for (f = 0; f < ENV_FIELD_COUNT; f++){
    if (b->env_compared[f] == 0) continue;
    denom++;
    if (!b->env_mismatched[f]) agree++;
  }
  return denom == 0 ? -1.0 : (double)agree / (double)denom;
}

static void bucket_add(struct bucket *b,double value,const char *run_id,const char *hostname,
                        const char *cpu_vendor,int counters_requested,int counters_measured,
                        const char *const env_values[ENV_FIELD_COUNT]){
  int f;

  if (b->n == b->cap){
    b->cap = b->cap ? b->cap * 2 : 8;
    b->values = realloc(b->values,sizeof(double) * (size_t)b->cap);
    b->run_ids = realloc(b->run_ids,sizeof(char *) * (size_t)b->cap);
    b->hostnames = realloc(b->hostnames,sizeof(char *) * (size_t)b->cap);
  }
  b->values[b->n] = value;
  b->run_ids[b->n] = strdup(run_id);
  b->hostnames[b->n] = strdup(hostname);
  b->n++;

  if (!b->pmu_signature_set){
    snprintf(b->pmu_vendor,sizeof(b->pmu_vendor),"%s",cpu_vendor);
    b->pmu_requested = counters_requested;
    b->pmu_measured = counters_measured;
    b->pmu_signature_set = 1;
  } else if (strcmp(b->pmu_vendor,cpu_vendor) != 0 ||
             b->pmu_requested != counters_requested ||
             b->pmu_measured != counters_measured){
    b->pmu_mixed = 1;
  }

  for (f = 0; f < ENV_FIELD_COUNT; f++){
    const char *v = env_values[f];
    if (!v) continue; /* unavailable on this run -- doesn't touch this field's signature/denominator */
    if (!b->env_sig_set[f]){
      snprintf(b->env_sig[f],sizeof(b->env_sig[f]),"%s",v);
      b->env_sig_set[f] = 1; /* first contributing value becomes the signature -- nothing to
                               * compare it against yet, same as pmu_vendor's own first-add */
      continue;
    }
    b->env_compared[f]++;
    if (f == ENV_MEMORY_TOTAL_KB){
      double sig_kb = atof(b->env_sig[f]),this_kb = atof(v);
      double tol = sig_kb * ENV_MEMORY_TOLERANCE_FRACTION;
      if (fabs(this_kb - sig_kb) > tol) b->env_mismatched[f] = 1;
    } else if (strcmp(b->env_sig[f],v) != 0){
      b->env_mismatched[f] = 1;
    }
  }
}

/* Reads ENV_FIELD_COUNT consecutive sqlite3_column_text() values starting
 * at first_col into env_values, in ENV_FIELD_COLUMN's order -- shared by
 * summarize() and load_baseline_buckets(), whose SELECT lists both append
 * the same 8 run_environment columns in that order right before calling
 * bucket_add(). NULL entries (SQL NULL) are preserved as NULL, not
 * "(unknown)" -- bucket_add() treats NULL as "unavailable on this run",
 * a real state distinct from any display placeholder. */
static void read_env_values(sqlite3_stmt *stmt,int first_col,const char *env_values[ENV_FIELD_COUNT]){
  int f;
  for (f = 0; f < ENV_FIELD_COUNT; f++)
    env_values[f] = (const char *)sqlite3_column_text(stmt,first_col + f);
}

static void bucket_free_contents(struct bucket *b){
  int i;
  for (i = 0; i < b->n; i++){ free(b->run_ids[i]); free(b->hostnames[i]); }
  free(b->run_ids);
  free(b->hostnames);
  free(b->values);
}

/* Appends entry to buf (already holding *used bytes, semicolon-separated),
 * declining to touch buf at all if entry (plus a leading separator, if any)
 * wouldn't fit -- shared by outlier_ids and format_contributing_runs below
 * so the bounds-check arithmetic exists in exactly one place. Returns 1 if
 * entry was actually appended, 0 if dropped for space. Checking the
 * separator+entry+NUL as one combined bound (rather than adding the
 * separator first and only then checking the entry) matters once a list
 * gets long enough to truncate: adding the separator unconditionally would
 * leave a dangling ';' with no entry after it, and the next call would do
 * the same, filling the rest of the buffer with bare separators instead of
 * stopping cleanly. */
static int append_list_entry(char *buf,size_t bufsize,size_t *used,const char *entry){
  size_t len = strlen(entry);
  size_t sep = (*used > 0) ? 1 : 0;

  if (*used + sep + len + 1 > bufsize) return 0;
  if (sep) buf[(*used)++] = ';';
  strcpy(buf + *used,entry);
  *used += len;
  return 1;
}

/* Appends every hostname:run_id in the bucket (in run order, not just
 * outliers -- that's outlier_ids' job) into buf, semicolon-separated, for
 * --show-runs. Unlike outlier_ids (normally a handful of flagged runs), a
 * --show-runs list is meant to be complete, so a "(+N more)" marker (itself
 * best-effort -- if even that doesn't fit, the list is silently short, same
 * as outlier_ids already accepts) makes a truncation visible rather than
 * silently dropping the tail. */
static void format_contributing_runs(const struct bucket *b,char *buf,size_t bufsize){
  size_t used = 0;
  int i,dropped = 0;

  buf[0] = '\0';
  for (i = 0; i < b->n; i++){
    char entry[256];
    snprintf(entry,sizeof(entry),"%s:%s",b->hostnames[i],b->run_ids[i]);
    if (!append_list_entry(buf,bufsize,&used,entry)) dropped++;
  }
  if (dropped > 0){
    char marker[64];
    snprintf(marker,sizeof(marker),"(+%d more)",dropped);
    append_list_entry(buf,bufsize,&used,marker);
  }
}

/* Computes stats for a closed bucket and prints one table row (human or
 * CSV). Buckets with fewer than opts->min_runs contributing runs are
 * skipped -- counted in totals, not fatal (degrade, don't fail: a thin
 * bucket just isn't trustworthy enough to report, the rest of the table
 * still is). */
static void emit_bucket(const struct bucket *b,const struct summary_opts *opts,FILE *out,
                         struct summary_totals *totals){
  double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high,env_score;
  int *outlier_flags;
  int outlier_count,i;
  char outlier_ids[512];
  char contributing_runs[4096];
  char env_score_buf[16];
  char verdict[40]; /* longest possible: "WARN:thin,noisy,mixed-pmu,mixed-env" (36 chars) + NUL */
  size_t used = 0;

  if (b->n < opts->min_runs){
    totals->groups_skipped_min_runs++;
    return;
  }

  outlier_flags = malloc(sizeof(int) * (size_t)b->n);
  outlier_count = compute_stats(b->values,b->n,opts->outlier_z,
                                 &min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);

  outlier_ids[0] = '\0';
  for (i = 0; i < b->n; i++){
    if (!outlier_flags[i]) continue;
    append_list_entry(outlier_ids,sizeof(outlier_ids),&used,b->run_ids[i]);
  }
  if (opts->show_runs) format_contributing_runs(b,contributing_runs,sizeof(contributing_runs));

  cv = (mean_v != 0.0) ? (stddev_v / fabs(mean_v)) * 100.0 : 0.0;
  compute_ci95(mean_v,stddev_v,b->n,&ci_low,&ci_high);
  env_score = compute_env_score(b);
  compute_verdict(b->n,cv,opts->max_cv,b->pmu_mixed,env_score,opts->min_env_score,verdict,sizeof(verdict));
  if (strcmp(verdict,"PASS") != 0) totals->groups_warned++;

  if (env_score >= 0.0) snprintf(env_score_buf,sizeof(env_score_buf),"%.4g",env_score);
  else snprintf(env_score_buf,sizeof(env_score_buf),"-");

  if (opts->csvflag){
    print_csv_field(out,b->group_val); fputc(',',out);
    if (opts->group_by_option){ print_csv_field(out,b->secondary_val); fputc(',',out); }
    print_csv_field(out,b->metric_name); fputc(',',out);
    fprintf(out,"%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.4g,",
            b->n,min_v,max_v,mean_v,median_v,stddev_v,cv);
    if (env_score >= 0.0) fprintf(out,"%.4g,",env_score); else fprintf(out,",");
    fprintf(out,"%.6g,%.6g,",ci_low,ci_high);
    print_csv_field(out,verdict); fputc(',',out);
    fprintf(out,"%d,",outlier_count);
    print_csv_field(out,outlier_ids);
    if (opts->show_runs){ fputc(',',out); print_csv_field(out,contributing_runs); }
    fputc('\n',out);
  } else {
    fprintf(out,"%-28.28s ",b->group_val);
    if (opts->group_by_option) fprintf(out,"%-20.20s ",b->secondary_val);
    fprintf(out,"%-24.24s %4d %10.6g %10.6g %10.6g %10.6g %10.6g %7.2f%% %9s %10.6g %10.6g %-16s %3d  %s",
            b->metric_name,b->n,min_v,max_v,mean_v,median_v,stddev_v,cv,env_score_buf,ci_low,ci_high,
            verdict,outlier_count,outlier_ids);
    if (opts->show_runs) fprintf(out,"  %s",contributing_runs);
    fputc('\n',out);
  }
  free(outlier_flags);
  totals->groups_reported++;
}

/* Streams (group,secondary,metric,per-run-average-value) rows out of the
 * store, already sorted by (group,secondary,metric,run start_time),
 * bucketing contiguous rows that share (group,secondary,metric) and
 * emitting each bucket as it closes. Per-run averaging (AVG(mv.value)
 * grouped by run id) is what collapses --interval's multiple ticks or
 * --per-core's multiple cores down to one number per run before the
 * across-run statistics ever see it -- see the file header comment.
 *
 * run_environment is always LEFT JOINed (cheap, at most one row per run)
 * so --group-by cpu_governor/virt_role work without a conditional query
 * shape; run_config_options is always LEFT JOINed on a bound option_name
 * too (?3, "" when --group-by-option wasn't given, which never matches a
 * real option_name -- store.c never stores an empty one -- so secondary_val
 * is uniformly NULL/"(unknown)" and effectively inert when the flag isn't
 * in use, rather than needing a second query shape). */
static int summarize(sqlite3 *db,const struct summary_opts *opts,FILE *out,struct summary_totals *totals){
  char sql[1600];
  sqlite3_stmt *stmt;
  struct bucket cur;
  int have_bucket = 0;

  snprintf(sql,sizeof(sql),
    "SELECT %s AS group_val, rco.option_value AS secondary_val, "
    "r.run_id AS run_id, r.hostname AS run_hostname, "
    "mv.metric_name AS metric_name, AVG(mv.value) AS avg_value, "
    "r.cpu_vendor AS run_cpu_vendor, r.counters_requested AS run_counters_requested, "
    "r.counters_measured AS run_counters_measured, "
    "e.virt_role, e.hypervisor_vendor, e.microcode_version, "
    "e.bios_vendor, e.bios_version, e.bios_date, e.cpu_governor, e.memory_total_kb "
    "FROM metric_values mv JOIN runs r ON r.id = mv.run_id "
    "LEFT JOIN run_environment e ON e.run_id = r.id "
    "LEFT JOIN run_config_options rco ON rco.run_id = r.id AND rco.option_name = ?3 "
    "WHERE mv.value IS NOT NULL "
    "AND (?1 = '' OR r.command LIKE '%%' || ?1 || '%%') "
    "AND (?2 = '' OR r.hostname = ?2) "
    "GROUP BY r.id, mv.metric_name "
    "ORDER BY group_val, secondary_val, mv.metric_name, r.start_time, r.id;",
    group_by_column(opts->group_by));

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,opts->command_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,opts->hostname_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,opts->group_by_option ? opts->group_by_option : "",-1,SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW){
    const unsigned char *group_raw = sqlite3_column_text(stmt,0);
    const unsigned char *secondary_raw = sqlite3_column_text(stmt,1);
    const unsigned char *run_id_raw = sqlite3_column_text(stmt,2);
    const unsigned char *hostname_raw = sqlite3_column_text(stmt,3);
    const unsigned char *metric_raw = sqlite3_column_text(stmt,4);
    double value = sqlite3_column_double(stmt,5);
    const unsigned char *cpu_vendor_raw = sqlite3_column_text(stmt,6);
    int counters_requested = sqlite3_column_int(stmt,7);
    int counters_measured = sqlite3_column_int(stmt,8);
    const char *env_values[ENV_FIELD_COUNT];
    const char *group_val = group_raw ? (const char *)group_raw : "(unknown)";
    const char *secondary_val = secondary_raw ? (const char *)secondary_raw : "(unknown)";
    const char *run_id = run_id_raw ? (const char *)run_id_raw : "?";
    const char *hostname = hostname_raw ? (const char *)hostname_raw : "?";
    const char *metric_name = metric_raw ? (const char *)metric_raw : "?";
    const char *cpu_vendor = cpu_vendor_raw ? (const char *)cpu_vendor_raw : "(unknown)";

    read_env_values(stmt,9,env_values);

    if (!metric_wanted(opts,metric_name)) continue;
    totals->rows_scanned++;

    if (!have_bucket || strcmp(cur.group_val,group_val) != 0 ||
        strcmp(cur.secondary_val,secondary_val) != 0 || strcmp(cur.metric_name,metric_name) != 0){
      if (have_bucket){ emit_bucket(&cur,opts,out,totals); bucket_free_contents(&cur); }
      bucket_reset(&cur,group_val,secondary_val,metric_name);
      have_bucket = 1;
    }
    bucket_add(&cur,value,run_id,hostname,cpu_vendor,counters_requested,counters_measured,env_values);
  }
  if (have_bucket){ emit_bucket(&cur,opts,out,totals); bucket_free_contents(&cur); }

  sqlite3_finalize(stmt);
  return 0;
}

static sqlite3 *open_summary_db(const char *path){
  sqlite3 *db = NULL;
  sqlite3_stmt *stmt;
  int user_version = 0;

  if (sqlite3_open_v2(path,&db,SQLITE_OPEN_READONLY,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: unable to open database %s: %s\n",path,sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db,30000);

  if (sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(stmt) == SQLITE_ROW) user_version = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }
  if (user_version < METRIC_VALUES_MIN_SCHEMA_VERSION){
    fprintf(stderr,"wspy-summary: %s: schema version %d predates metric_values (needs >= %d) "
                    "-- re-ingest with a current wspy-store build\n",
            path,user_version,METRIC_VALUES_MIN_SCHEMA_VERSION);
    sqlite3_close(db);
    return NULL;
  }
  return db;
}

/* Resolves a "hostname:run_id" identity (the same identity --show-runs
 * appends to a summary bucket) to the artifact chain this item exists to
 * close: command line, environment (via the manifest), raw CSV, tree
 * artifact, and plots. Output is a stable "key=value" line format rather
 * than --csv's table shape (a single resolved run isn't a table row) or a
 * bespoke JSON encoding (this codebase's hand-rolled JSON emitter lives in
 * json_util.c for manifest/run-index writing, not worth pulling into a
 * query tool for one record) -- easy for a script (web/server.py) or a
 * human to read either way. Every path field degrades independently
 * (exists=0 rather than aborting) matching validate.c/coverage.c's
 * "measured vs unavailable" idiom -- a manifest/CSV/tree file recorded at
 * ingest time may since have been moved, pruned, or never existed on this
 * machine at all (store.c's own doc note: paths from a different
 * originating host frequently don't resolve here). Returns 0 if the
 * (hostname,run_id) pair was found in the store (regardless of which
 * artifacts still exist), 1 if no such run is recorded at all. */
/* Prints "key=value\n", replacing any embedded '\n'/'\r' in value with a
 * space first -- the key=value format is one record per line, so a stray
 * newline inside a stored command/path (nothing upstream forbids one)
 * would otherwise start a bare, unkeyed line that a parser like
 * web/server.py's _discovery_trace() silently drops instead of corrupting
 * the field it belongs to. */
static void print_trace_field(FILE *out,const char *key,const char *value){
  const char *p;
  fprintf(out,"%s=",key);
  for (p = value; *p; p++) fputc((*p == '\n' || *p == '\r') ? ' ' : *p,out);
  fputc('\n',out);
}

static int trace_run(sqlite3 *db,const char *hostname,const char *run_id,FILE *out){
  sqlite3_stmt *stmt;
  int found = 0;
  const char *sql =
    "SELECT command,start_time,cpu_vendor,manifest_path,output_path,tree_output_path "
    "FROM runs WHERE hostname = ?1 AND run_id = ?2;";

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return 1;
  }
  sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) == SQLITE_ROW){
    const unsigned char *command = sqlite3_column_text(stmt,0);
    const unsigned char *start_time = sqlite3_column_text(stmt,1);
    const unsigned char *cpu_vendor = sqlite3_column_text(stmt,2);
    const char *output_path = (const char *)sqlite3_column_text(stmt,4);
    /* {path key, exists key, sqlite column} triples sharing one print+
     * stat() loop below -- a fourth artifact path (a future store column)
     * is one array entry, not a fourth copy-pasted block. Key names match
     * store.c's own (irregular: "tree_output_path", not "tree_path")
     * column names, since web/server.py's _discovery_trace() parses these
     * exact keys back out. */
    struct { const char *path_key,*exists_key; int column; } artifacts[] = {
      { "manifest_path", "manifest_exists", 3 },
      { "output_path", "output_exists", 4 },
      { "tree_output_path", "tree_exists", 5 },
    };
    size_t i;

    found = 1;
    print_trace_field(out,"hostname",hostname);
    print_trace_field(out,"run_id",run_id);
    print_trace_field(out,"command",command ? (const char *)command : "");
    print_trace_field(out,"start_time",start_time ? (const char *)start_time : "");
    print_trace_field(out,"cpu_vendor",cpu_vendor ? (const char *)cpu_vendor : "");

    for (i = 0; i < sizeof(artifacts)/sizeof(artifacts[0]); i++){
      const char *path = (const char *)sqlite3_column_text(stmt,artifacts[i].column);
      struct stat st;

      print_trace_field(out,artifacts[i].path_key,path ? path : "");
      fprintf(out,"%s=%d\n",artifacts[i].exists_key,(path && stat(path,&st) == 0) ? 1 : 0);
    }

    /* wspy-plot (plot.c) writes into <rundir>/plots, a sibling of whichever
     * CSV it charted from -- not a path store.c records anywhere, so derive
     * it from output_path's directory rather than leaving it unresolved.
     * output_path with no '/' at all (a bare relative filename) has no
     * derivable directory of its own -- degrade to "can't tell" (exists=0)
     * rather than guessing a "plots" path relative to wherever
     * wspy-summary's cwd happens to be, which could silently report an
     * unrelated directory's contents as this run's plots. */
    if (output_path && strchr(output_path,'/')){
      char plots_dir[4096];
      const char *slash = strrchr(output_path,'/');
      DIR *d;
      int png_count = 0;

      snprintf(plots_dir,sizeof(plots_dir),"%.*s/plots",
               (int)(slash - output_path),output_path);

      print_trace_field(out,"plots_dir",plots_dir);
      d = opendir(plots_dir);
      if (d){
        struct dirent *ent;
        while ((ent = readdir(d)) != NULL){
          size_t len = strlen(ent->d_name);
          if (len > 4 && !strcmp(ent->d_name + len - 4,".png")) png_count++;
        }
        closedir(d);
        fprintf(out,"plots_exist=1\n");
      } else {
        fprintf(out,"plots_exist=0\n");
      }
      fprintf(out,"plots_count=%d\n",png_count);
    } else {
      fprintf(out,"plots_dir=\n");
      fprintf(out,"plots_exist=0\n");
      fprintf(out,"plots_count=0\n");
    }
  }

  sqlite3_finalize(stmt);
  if (!found)
    fprintf(stderr,"wspy-summary: no run found for %s:%s\n",hostname,run_id);
  return found ? 0 : 1;
}

/* --check-regression's first step: resolve (hostname,run_id) to its runs.id/start_time and the
 * bucket key (group_val/secondary_val) it belongs to under the *current* --group-by/
 * --group-by-option settings -- reuses group_by_column() exactly as summarize()'s own query does
 * (same whitelisted-interpolation safety argument), so "which historical runs count as
 * comparable" is defined identically here and in the plain group-by summary table, not by a
 * second, separately-invented matching-key set. Returns 1 if found, 0 otherwise (caller prints
 * the not-found message, mirroring trace_run()'s own convention above). */
static int resolve_target_run(sqlite3 *db,const struct summary_opts *opts,
                               const char *hostname,const char *run_id,struct target_run *out){
  char sql[600];
  sqlite3_stmt *stmt;
  int found = 0;

  snprintf(sql,sizeof(sql),
    "SELECT r.id, r.start_time, %s AS group_val, rco.option_value AS secondary_val "
    "FROM runs r "
    "LEFT JOIN run_environment e ON e.run_id = r.id "
    "LEFT JOIN run_config_options rco ON rco.run_id = r.id AND rco.option_name = ?3 "
    "WHERE r.hostname = ?1 AND r.run_id = ?2;",
    group_by_column(opts->group_by));

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return 0;
  }
  sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,opts->group_by_option ? opts->group_by_option : "",-1,SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) == SQLITE_ROW){
    out->id = sqlite3_column_int64(stmt,0);
    snprintf(out->start_time,sizeof(out->start_time),"%s",
             (const char *)sqlite3_column_text(stmt,1));
    out->group_val_isnull = (sqlite3_column_type(stmt,2) == SQLITE_NULL);
    snprintf(out->group_val,sizeof(out->group_val),"%s",
             out->group_val_isnull ? "" : (const char *)sqlite3_column_text(stmt,2));
    out->secondary_val_isnull = (sqlite3_column_type(stmt,3) == SQLITE_NULL);
    snprintf(out->secondary_val,sizeof(out->secondary_val),"%s",
             out->secondary_val_isnull ? "" : (const char *)sqlite3_column_text(stmt,3));
    found = 1;
  }
  sqlite3_finalize(stmt);
  return found;
}

/* The target run's own per-metric values -- run_id here is the resolved integer runs.id
 * (struct target_run.id), not the string run-index run_id metric_values doesn't key on. */
static int load_target_metrics(sqlite3 *db,sqlite3_int64 run_id,struct metric_value_map *out){
  sqlite3_stmt *stmt;
  const char *sql =
    "SELECT metric_name, AVG(value) FROM metric_values "
    "WHERE run_id = ?1 AND value IS NOT NULL GROUP BY metric_name;";

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_int64(stmt,1,run_id);
  while (sqlite3_step(stmt) == SQLITE_ROW){
    metric_map_add(out,(const char *)sqlite3_column_text(stmt,0),sqlite3_column_double(stmt,1));
  }
  sqlite3_finalize(stmt);
  return 0;
}

/* Streams the baseline query (every strictly-earlier run sharing the target's bucket) into a
 * growable array of per-metric struct bucket -- same per-run AVG(mv.value) collapse and JOIN
 * shape as summarize()'s own query, but WHERE-scoped to one already-resolved bucket via
 * target->group_val/secondary_val (NULL-safely, via "IS ?" -- see struct target_run's comment),
 * so buckets here are keyed by metric_name alone (the group axis is already fixed). Caller owns
 * the returned array (bucket_free_contents() each entry, then free() the array itself). */
static int load_baseline_buckets(sqlite3 *db,const struct summary_opts *opts,
                                  const struct target_run *target,
                                  struct bucket **out_buckets,int *out_n){
  char sql[1200];
  sqlite3_stmt *stmt;
  struct bucket *buckets = NULL;
  int n = 0,cap = 0;
  int have_bucket = 0;
  struct bucket cur;

  snprintf(sql,sizeof(sql),
    "SELECT mv.metric_name AS metric_name, AVG(mv.value) AS avg_value, "
    "r.run_id AS run_id, r.hostname AS run_hostname, "
    "r.cpu_vendor AS run_cpu_vendor, r.counters_requested AS run_counters_requested, "
    "r.counters_measured AS run_counters_measured, "
    "e.virt_role, e.hypervisor_vendor, e.microcode_version, "
    "e.bios_vendor, e.bios_version, e.bios_date, e.cpu_governor, e.memory_total_kb "
    "FROM metric_values mv JOIN runs r ON r.id = mv.run_id "
    "LEFT JOIN run_environment e ON e.run_id = r.id "
    "LEFT JOIN run_config_options rco ON rco.run_id = r.id AND rco.option_name = ?5 "
    "WHERE mv.value IS NOT NULL AND r.start_time < ?1 AND r.id != ?2 "
    "AND %s IS ?3 AND rco.option_value IS ?4 "
    "GROUP BY r.id, mv.metric_name "
    "ORDER BY mv.metric_name, r.start_time, r.id;",
    group_by_column(opts->group_by));

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,target->start_time,-1,SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt,2,target->id);
  if (target->group_val_isnull) sqlite3_bind_null(stmt,3);
  else sqlite3_bind_text(stmt,3,target->group_val,-1,SQLITE_TRANSIENT);
  if (target->secondary_val_isnull) sqlite3_bind_null(stmt,4);
  else sqlite3_bind_text(stmt,4,target->secondary_val,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,5,opts->group_by_option ? opts->group_by_option : "",-1,SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW){
    const unsigned char *metric_raw = sqlite3_column_text(stmt,0);
    double value = sqlite3_column_double(stmt,1);
    const unsigned char *run_id_raw = sqlite3_column_text(stmt,2);
    const unsigned char *hostname_raw = sqlite3_column_text(stmt,3);
    const unsigned char *cpu_vendor_raw = sqlite3_column_text(stmt,4);
    int counters_requested = sqlite3_column_int(stmt,5);
    int counters_measured = sqlite3_column_int(stmt,6);
    const char *env_values[ENV_FIELD_COUNT];
    const char *metric_name = metric_raw ? (const char *)metric_raw : "?";
    const char *run_id = run_id_raw ? (const char *)run_id_raw : "?";
    const char *hostname = hostname_raw ? (const char *)hostname_raw : "?";
    const char *cpu_vendor = cpu_vendor_raw ? (const char *)cpu_vendor_raw : "(unknown)";

    read_env_values(stmt,7,env_values);

    if (!metric_wanted(opts,metric_name)) continue;

    if (!have_bucket || strcmp(cur.metric_name,metric_name) != 0){
      if (have_bucket){
        if (n == cap){ cap = cap ? cap * 2 : 8; buckets = realloc(buckets,sizeof(struct bucket) * (size_t)cap); }
        buckets[n++] = cur;
      }
      bucket_reset(&cur,"","",metric_name);
      have_bucket = 1;
    }
    bucket_add(&cur,value,run_id,hostname,cpu_vendor,counters_requested,counters_measured,env_values);
  }
  if (have_bucket){
    if (n == cap){ cap = cap ? cap * 2 : 8; buckets = realloc(buckets,sizeof(struct bucket) * (size_t)cap); }
    buckets[n++] = cur;
  }

  sqlite3_finalize(stmt);
  *out_buckets = buckets;
  *out_n = n;
  return 0;
}

static const struct bucket *find_baseline_bucket(const struct bucket *buckets,int n,const char *metric_name){
  int i;
  for (i = 0; i < n; i++) if (!strcmp(buckets[i].metric_name,metric_name)) return &buckets[i];
  return NULL;
}

/* One --check-regression output row: target run's own value for one metric versus that metric's
 * baseline (strictly-earlier, same-bucket runs). status is direction-neutral ("above"/"below"
 * baseline's 95% CI, or "within" it) rather than an asserted "regression" -- this codebase has no
 * per-metric higher-is-better/lower-is-better table anywhere to make that call correctly (see
 * this file's header comment for the header-comment-level version of this reasoning once added
 * there), so a human (or a future per-metric semantics table) decides whether a flagged
 * deviation is actually bad. "-" (not 0/empty) marks fields with no meaningful value for the
 * no-baseline/thin cases, so "no data" is never misread as "baseline mean is zero". */
static void print_regression_row(FILE *out,int csvflag,const char *metric_name,double target_value,
                                  const char *status,int n,double mean,double ci_low,double ci_high,
                                  double env_score,const char *verdict){
  if (csvflag){
    print_csv_field(out,metric_name); fputc(',',out);
    fprintf(out,"%.6g,",target_value);
    if (n >= 0){
      fprintf(out,"%d,%.6g,%.6g,%.6g,",n,mean,ci_low,ci_high);
      if (env_score >= 0.0) fprintf(out,"%.4g,",env_score); else fprintf(out,",");
      print_csv_field(out,verdict);
    } else {
      fprintf(out,",,,,,");
    }
    fputc(',',out);
    print_csv_field(out,status);
    fputc('\n',out);
  } else {
    if (n >= 0){
      char env_score_buf[16];
      if (env_score >= 0.0) snprintf(env_score_buf,sizeof(env_score_buf),"%.4g",env_score);
      else snprintf(env_score_buf,sizeof(env_score_buf),"-");
      fprintf(out,"%-32.32s %12.6g %4d %12.6g %12.6g %12.6g %8s %-16s %-12s\n",
              metric_name,target_value,n,mean,ci_low,ci_high,env_score_buf,verdict,status);
    } else {
      fprintf(out,"%-32.32s %12.6g %4s %12s %12s %12s %8s %-16s %-12s\n",
              metric_name,target_value,"-","-","-","-","-","-",status);
    }
  }
}

/* --check-regression driver: resolves the target run + its bucket key, loads the target run's
 * own per-metric values, loads the baseline (strictly-earlier same-bucket runs) per metric, and
 * compares. Returns 1 if the target run wasn't found (mirrors trace_run()'s unconditional
 * not-found return, not gated by --strict); otherwise (opts->strict && flagged>0) ? 1 : 0. */
static int check_regression(sqlite3 *db,const struct summary_opts *opts,
                             const char *hostname,const char *run_id,FILE *out){
  struct target_run target;
  struct metric_value_map target_metrics;
  struct bucket *baselines = NULL;
  int nbaselines = 0;
  int checked = 0,flagged = 0,no_baseline = 0;
  int i;

  if (!resolve_target_run(db,opts,hostname,run_id,&target)){
    fprintf(stderr,"wspy-summary: no run found for %s:%s\n",hostname,run_id);
    return 1;
  }

  memset(&target_metrics,0,sizeof(target_metrics));
  if (load_target_metrics(db,target.id,&target_metrics) != 0) return 1;
  if (load_baseline_buckets(db,opts,&target,&baselines,&nbaselines) != 0){
    metric_map_free(&target_metrics);
    return 1;
  }

  if (opts->csvflag){
    fprintf(out,"metric,target_value,baseline_n,baseline_mean,ci95_low,ci95_high,baseline_env_score,"
                "baseline_verdict,status\n");
  } else {
    fprintf(out,"%-32s %12s %4s %12s %12s %12s %8s %-16s %-12s\n",
            "metric","target","n","base_mean","ci95_low","ci95_high","env_score","base_verdict","status");
  }

  for (i = 0; i < target_metrics.n; i++){
    const char *metric_name = target_metrics.names[i];
    double target_value = target_metrics.values[i];
    const struct bucket *b;

    if (!metric_wanted(opts,metric_name)) continue;
    checked++;

    b = find_baseline_bucket(baselines,nbaselines,metric_name);
    if (!b){
      no_baseline++;
      print_regression_row(out,opts->csvflag,metric_name,target_value,"no-baseline",-1,0,0,0,-1.0,"");
    } else if (b->n < opts->min_runs){
      print_regression_row(out,opts->csvflag,metric_name,target_value,"thin",-1,0,0,0,-1.0,"");
    } else {
      double min_v,max_v,mean_v,median_v,stddev_v,cv,ci_low,ci_high;
      int *outlier_flags = malloc(sizeof(int) * (size_t)b->n);
      char verdict[40];
      double env_score;
      const char *status;

      compute_stats(b->values,b->n,opts->outlier_z,&min_v,&max_v,&mean_v,&median_v,&stddev_v,outlier_flags);
      free(outlier_flags);
      cv = (mean_v != 0.0) ? (stddev_v / fabs(mean_v)) * 100.0 : 0.0;
      compute_ci95(mean_v,stddev_v,b->n,&ci_low,&ci_high);
      env_score = compute_env_score(b);
      compute_verdict(b->n,cv,opts->max_cv,b->pmu_mixed,env_score,opts->min_env_score,verdict,sizeof(verdict));

      if (target_value < ci_low) status = "below";
      else if (target_value > ci_high) status = "above";
      else status = "within";
      if (!strcmp(status,"above") || !strcmp(status,"below")) flagged++;

      print_regression_row(out,opts->csvflag,metric_name,target_value,status,b->n,mean_v,ci_low,ci_high,
                            env_score,verdict);
    }
  }

  if (!opts->quiet)
    fprintf(out,"wspy-summary: %d metric(s) checked, %d flagged, %d with no baseline history\n",
            checked,flagged,no_baseline);

  for (i = 0; i < nbaselines; i++) bucket_free_contents(&baselines[i]);
  free(baselines);
  metric_map_free(&target_metrics);

  return (opts->strict && flagged > 0) ? 1 : 0;
}

#ifndef TEST_SUMMARY
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s --db <path> [options]\n"
    "\n"
    "Generates a summary table (min/max/mean/median/stddev/outlier flags)\n"
    "across repeated wspy runs recorded in a normalized store (wspy-store\n"
    "--db <path>), grouped by workload command (or --group-by) and metric\n"
    "name -- regenerable at any time straight from indexed data, no manual\n"
    "copy/paste.\n"
    "\n"
    "For each run, a metric's rows (ticks/cores/single aggregate value) are\n"
    "first averaged down to one number per run; statistics are then computed\n"
    "across those per-run values within each (group,metric) bucket.\n"
    "\n"
    "Options:\n"
    "  --db <path>            normalized store database (required)\n"
    "  --command <substr>     only include runs whose command matches this substring\n"
    "  --hostname <name>      only include runs from this host\n"
    "  --metric <name>        only include this metric; may be repeated (default: all)\n"
    "  --group-by <col>       command (default), hostname, cpu_vendor, affinity_mode,\n"
    "                         preset_name, config_name, cpu_governor, or virt_role\n"
    "  --group-by-option <name>  compose a second grouping axis from an arbitrary\n"
    "                         --config-option key (e.g. a comparison-matrix sweep's\n"
    "                         own tag) -- adds one column, doesn't replace --group-by\n"
    "  --outlier-stddev <n>   z-score threshold for flagging outliers (default 2.0)\n"
    "  --min-runs <n>         only report (group,metric) buckets with >= n runs (default 1)\n"
    "  --max-cv <percent>     CV%% above which a bucket's verdict is WARN:noisy (default 5.0)\n"
    "  --min-env-score <frac> env_score below which a bucket's verdict is WARN:mixed-env\n"
    "                         (default 0.8 -- at most 1 of 8 tracked provenance fields\n"
    "                         may disagree across the bucket's contributing runs)\n"
    "  --csv                  machine-readable CSV output instead of the human table\n"
    "  --show-runs            append every contributing run's hostname:run_id to each\n"
    "                         bucket (traceability: chase a surprising number to its runs)\n"
    "  -q, --quiet            suppress the trailing summary line\n"
    "  -s, --strict           exit non-zero if any bucket was skipped for --min-runs,\n"
    "                         if any reported bucket's verdict is WARN, or if nothing\n"
    "                         matched at all\n"
    "  -h, --help              show this help\n"
    "\n"
    "  --trace <host>:<run_id>  standalone mode: resolve one run (as named by\n"
    "                         --show-runs or a run-index record) to its manifest/\n"
    "                         raw CSV/tree/plots artifact paths, checking which\n"
    "                         still exist on disk. Ignores every other option\n"
    "                         above except --db. Prints key=value lines.\n"
    "\n"
    "  --check-regression <host>:<run_id>  standalone mode: compares this run's own\n"
    "                         per-metric values against a rolling baseline (every\n"
    "                         strictly-earlier run sharing the same --group-by/\n"
    "                         --group-by-option bucket). --group-by/--group-by-option/\n"
    "                         --metric/--min-runs/--max-cv/--min-env-score apply as\n"
    "                         modifiers; every other option above is ignored except --db/--csv/--quiet/\n"
    "                         --strict. A metric outside the baseline's 95%% CI is\n"
    "                         reported as \"above\" or \"below\" (direction-neutral --\n"
    "                         this tool doesn't know which direction is bad for a\n"
    "                         given metric), alongside \"within\"/\"no-baseline\"/\"thin\".\n"
    "                         Mutually exclusive with --trace.\n"
    "\n"
    "Every reported bucket carries a 95%% confidence interval of the mean (ci95_low/\n"
    "ci95_high, Student's t) and a repeatability verdict (PASS, or WARN:thin for\n"
    "n<3 runs, WARN:noisy for CV over --max-cv, WARN:mixed-pmu when the bucket's\n"
    "contributing runs don't all share the same cpu_vendor/counter coverage --\n"
    "same-named columns computed from genuinely different hardware or a different\n"
    "counter-collection setup, not just repeat-to-repeat noise -- and/or WARN:mixed-env\n"
    "when env_score falls below --min-env-score) alongside min/max/mean/median/\n"
    "stddev/cv_percent/env_score -- all default output, no flag needed. env_score is\n"
    "the fraction of 8 tracked environment fields (virt_role, hypervisor_vendor,\n"
    "microcode_version, bios_vendor/version/date, cpu_governor, memory_total_kb) that\n"
    "agreed across the bucket's contributing runs, or \"-\" when none of them were ever\n"
    "mutually comparable (never treated as a mismatch -- absence of data is not\n"
    "evidence of one).\n"
    "\n"
    "Exit status: 0 normally (1 with --strict if any bucket still needs more\n"
    "runs or carried a WARN verdict, or nothing matched; 1 with --trace or\n"
    "--check-regression if no such run is recorded, or with --check-regression\n"
    "--strict if any metric was flagged above/below baseline), 2 on a usage\n"
    "error or if the database could not be opened.\n",
    prog);
}

int main(int argc,char **argv){
  struct summary_opts opts;
  struct summary_totals totals;
  sqlite3 *db;
  int opt;
  const char *group_by_str = "command";

  static struct option long_options[] = {
    { "db",             required_argument, 0, 'd' },
    { "command",        required_argument, 0, 'c' },
    { "hostname",       required_argument, 0, 'H' },
    { "metric",         required_argument, 0, 'm' },
    { "group-by",       required_argument, 0, 'g' },
    { "group-by-option",required_argument, 0, 'O' },
    { "outlier-stddev", required_argument, 0, 'z' },
    { "min-runs",       required_argument, 0, 'n' },
    { "max-cv",         required_argument, 0, 'X' },
    { "min-env-score",  required_argument, 0, 'E' },
    { "csv",            no_argument,       0, 'C' },
    { "show-runs",      no_argument,       0, 'R' },
    { "trace",          required_argument, 0, 'T' },
    { "check-regression", required_argument, 0, 'K' },
    { "quiet",          no_argument,       0, 'q' },
    { "strict",         no_argument,       0, 's' },
    { "help",           no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  memset(&opts,0,sizeof(opts));
  opts.command_filter = "";
  opts.hostname_filter = "";
  opts.group_by = GROUP_COMMAND;
  opts.outlier_z = 2.0;
  opts.min_runs = 1;
  opts.max_cv = 5.0;
  opts.min_env_score = 0.8;

  while ((opt = getopt_long(argc,argv,"qsh",long_options,NULL)) != -1){
    switch (opt){
    case 'd': opts.db_path = optarg; break;
    case 'c': opts.command_filter = optarg; break;
    case 'H': opts.hostname_filter = optarg; break;
    case 'm':
      if (opts.nmetrics >= (int)(sizeof(opts.metrics)/sizeof(opts.metrics[0]))){
        fprintf(stderr,"wspy-summary: too many --metric filters\n");
        return 2;
      }
      opts.metrics[opts.nmetrics++] = optarg;
      break;
    case 'g': group_by_str = optarg; break;
    case 'O': opts.group_by_option = optarg; break;
    case 'z': opts.outlier_z = atof(optarg); break;
    case 'n': opts.min_runs = atoi(optarg); break;
    case 'X': opts.max_cv = atof(optarg); break;
    case 'E': opts.min_env_score = atof(optarg); break;
    case 'C': opts.csvflag = 1; break;
    case 'R': opts.show_runs = 1; break;
    case 'T': opts.trace_key = optarg; break;
    case 'K': opts.check_regression_key = optarg; break;
    case 'q': opts.quiet = 1; break;
    case 's': opts.strict = 1; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (!opts.db_path){
    fprintf(stderr,"wspy-summary: --db <path> is required\n\n");
    usage(argv[0]);
    return 2;
  }

  if (opts.trace_key && opts.check_regression_key){
    fprintf(stderr,"wspy-summary: --trace and --check-regression are mutually exclusive\n\n");
    usage(argv[0]);
    return 2;
  }

  if (opts.trace_key){
    const char *colon = strchr(opts.trace_key,':');
    char hostname_buf[256];
    int rc;

    if (!colon || colon == opts.trace_key || colon[1] == '\0'){
      fprintf(stderr,"wspy-summary: --trace expects <hostname>:<run_id>, got '%s'\n\n",opts.trace_key);
      usage(argv[0]);
      return 2;
    }
    snprintf(hostname_buf,sizeof(hostname_buf),"%.*s",
             (int)(colon - opts.trace_key),opts.trace_key);

    db = open_summary_db(opts.db_path);
    if (!db) return 2;
    rc = trace_run(db,hostname_buf,colon + 1,stdout);
    sqlite3_close(db);
    return rc;
  }

  if (!parse_group_by(group_by_str,&opts.group_by)){
    fprintf(stderr,"wspy-summary: unrecognized --group-by '%s' (expected command, hostname, "
                    "cpu_vendor, affinity_mode, preset_name, config_name, cpu_governor, "
                    "or virt_role)\n\n",group_by_str);
    usage(argv[0]);
    return 2;
  }
  if (opts.min_runs < 1) opts.min_runs = 1;

  if (opts.check_regression_key){
    const char *colon = strchr(opts.check_regression_key,':');
    char hostname_buf[256];
    int rc;

    if (!colon || colon == opts.check_regression_key || colon[1] == '\0'){
      fprintf(stderr,"wspy-summary: --check-regression expects <hostname>:<run_id>, got '%s'\n\n",
              opts.check_regression_key);
      usage(argv[0]);
      return 2;
    }
    snprintf(hostname_buf,sizeof(hostname_buf),"%.*s",
             (int)(colon - opts.check_regression_key),opts.check_regression_key);

    db = open_summary_db(opts.db_path);
    if (!db) return 2;
    rc = check_regression(db,&opts,hostname_buf,colon + 1,stdout);
    sqlite3_close(db);
    return rc;
  }

  db = open_summary_db(opts.db_path);
  if (!db) return 2;

  memset(&totals,0,sizeof(totals));

  if (opts.csvflag){
    printf("group,");
    if (opts.group_by_option) printf("group_by_option,");
    printf("metric,n,min,max,mean,median,stddev,cv_percent,env_score,ci95_low,ci95_high,verdict,"
           "outlier_count,outlier_run_ids");
    if (opts.show_runs) printf(",contributing_runs");
    printf("\n");
  } else {
    printf("%-28s ","group");
    if (opts.group_by_option) printf("%-20s ","group_by_option");
    printf("%-24s %4s %10s %10s %10s %10s %10s %8s %9s %10s %10s %-16s %3s  %s",
           "metric","n","min","max","mean","median","stddev","cv","env_score",
           "ci95_low","ci95_high","verdict","out","outlier run(s)");
    if (opts.show_runs) printf("  %s","contributing runs (host:run_id)");
    printf("\n");
  }

  if (summarize(db,&opts,stdout,&totals) != 0){
    sqlite3_close(db);
    return 2;
  }
  sqlite3_close(db);

  if (!opts.quiet)
    printf("wspy-summary: %d group(s) reported (%d with WARN), %d skipped (< %d run(s)), "
           "%d run-metric row(s) scanned\n",
           totals.groups_reported,totals.groups_warned,totals.groups_skipped_min_runs,
           opts.min_runs,totals.rows_scanned);

  if (opts.strict && (totals.groups_skipped_min_runs > 0 || totals.groups_reported == 0 ||
                       totals.groups_warned > 0)) return 1;
  return 0;
}
#endif /* TEST_SUMMARY */
