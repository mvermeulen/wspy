/*
 * summary.c - wspy-summary: generates a per-metric summary table (min/max/
 * mean/median/stddev/outlier flags) across repeated wspy runs recorded in
 * a normalized store (wspy-store --db <path>) -- INVESTIGATION_4.0.md's
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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sqlite3.h>

/* metric_values didn't exist before store.c's schema version 2
 * (MIGRATION_V1_TO_V2 in store.c) -- a database older than that has
 * nothing for this tool to summarize. */
#define METRIC_VALUES_MIN_SCHEMA_VERSION 2

enum group_by { GROUP_COMMAND, GROUP_HOSTNAME, GROUP_CPU_VENDOR };

struct summary_opts {
  const char *db_path;
  const char *command_filter;   /* substring match against runs.command, "" = no filter */
  const char *hostname_filter;  /* exact match against runs.hostname, "" = no filter */
  const char *metrics[64];
  int nmetrics;                 /* 0 = all metrics */
  enum group_by group_by;
  double outlier_z;
  int min_runs;
  int csvflag;
  int quiet;
  int strict;
};

struct summary_totals {
  int groups_reported;
  int groups_skipped_min_runs;
  int rows_scanned;
};

static const char *group_by_column(enum group_by g){
  switch (g){
  case GROUP_HOSTNAME: return "hostname";
  case GROUP_CPU_VENDOR: return "cpu_vendor";
  case GROUP_COMMAND: default: return "command";
  }
}

/* Whitelist, not user SQL -- this is what makes it safe to interpolate
 * group_by_column()'s return value directly into a query string below. */
static int parse_group_by(const char *s,enum group_by *out){
  if (!strcmp(s,"command")){ *out = GROUP_COMMAND; return 1; }
  if (!strcmp(s,"hostname")){ *out = GROUP_HOSTNAME; return 1; }
  if (!strcmp(s,"cpu_vendor")){ *out = GROUP_CPU_VENDOR; return 1; }
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
  char metric_name[192];
  double *values;
  char **run_ids;
  int n,cap;
};

static void bucket_reset(struct bucket *b,const char *group_val,const char *metric_name){
  b->n = 0;
  b->cap = 0;
  b->values = NULL;
  b->run_ids = NULL;
  snprintf(b->group_val,sizeof(b->group_val),"%s",group_val);
  snprintf(b->metric_name,sizeof(b->metric_name),"%s",metric_name);
}

static void bucket_add(struct bucket *b,double value,const char *run_id){
  if (b->n == b->cap){
    b->cap = b->cap ? b->cap * 2 : 8;
    b->values = realloc(b->values,sizeof(double) * (size_t)b->cap);
    b->run_ids = realloc(b->run_ids,sizeof(char *) * (size_t)b->cap);
  }
  b->values[b->n] = value;
  b->run_ids[b->n] = strdup(run_id);
  b->n++;
}

static void bucket_free_contents(struct bucket *b){
  int i;
  for (i = 0; i < b->n; i++) free(b->run_ids[i]);
  free(b->run_ids);
  free(b->values);
}

/* Computes stats for a closed bucket and prints one table row (human or
 * CSV). Buckets with fewer than opts->min_runs contributing runs are
 * skipped -- counted in totals, not fatal (degrade, don't fail: a thin
 * bucket just isn't trustworthy enough to report, the rest of the table
 * still is). */
static void emit_bucket(const struct bucket *b,const struct summary_opts *opts,FILE *out,
                         struct summary_totals *totals){
  double min_v,max_v,mean_v,median_v,stddev_v,cv;
  int *outlier_flags;
  int outlier_count,i;
  char outlier_ids[512];
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
    size_t len;
    if (!outlier_flags[i]) continue;
    len = strlen(b->run_ids[i]);
    if (used > 0 && used + 1 < sizeof(outlier_ids)){ outlier_ids[used++] = ';'; outlier_ids[used] = '\0'; }
    if (used + len < sizeof(outlier_ids)){ strcpy(outlier_ids + used,b->run_ids[i]); used += len; }
  }

  cv = (mean_v != 0.0) ? (stddev_v / fabs(mean_v)) * 100.0 : 0.0;

  if (opts->csvflag){
    print_csv_field(out,b->group_val); fputc(',',out);
    print_csv_field(out,b->metric_name); fputc(',',out);
    fprintf(out,"%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.4g,%d,",
            b->n,min_v,max_v,mean_v,median_v,stddev_v,cv,outlier_count);
    print_csv_field(out,outlier_ids);
    fputc('\n',out);
  } else {
    fprintf(out,"%-28.28s %-24.24s %4d %10.6g %10.6g %10.6g %10.6g %10.6g %7.2f%% %3d  %s\n",
            b->group_val,b->metric_name,b->n,min_v,max_v,mean_v,median_v,stddev_v,cv,outlier_count,
            outlier_ids);
  }
  free(outlier_flags);
  totals->groups_reported++;
}

/* Streams (group,metric,per-run-average-value) rows out of the store,
 * already sorted by (group,metric,run start_time), bucketing contiguous
 * rows that share (group,metric) and emitting each bucket as it closes.
 * Per-run averaging (AVG(mv.value) grouped by run id) is what collapses
 * --interval's multiple ticks or --per-core's multiple cores down to one
 * number per run before the across-run statistics ever see it -- see the
 * file header comment. */
static int summarize(sqlite3 *db,const struct summary_opts *opts,FILE *out,struct summary_totals *totals){
  char sql[1024];
  sqlite3_stmt *stmt;
  struct bucket cur;
  int have_bucket = 0;

  snprintf(sql,sizeof(sql),
    "SELECT r.%s AS group_val, r.run_id AS run_id, mv.metric_name AS metric_name, "
    "AVG(mv.value) AS avg_value "
    "FROM metric_values mv JOIN runs r ON r.id = mv.run_id "
    "WHERE mv.value IS NOT NULL "
    "AND (?1 = '' OR r.command LIKE '%%' || ?1 || '%%') "
    "AND (?2 = '' OR r.hostname = ?2) "
    "GROUP BY r.id, mv.metric_name "
    "ORDER BY group_val, mv.metric_name, r.start_time, r.id;",
    group_by_column(opts->group_by));

  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK){
    fprintf(stderr,"wspy-summary: query failed: %s\n",sqlite3_errmsg(db));
    return -1;
  }
  sqlite3_bind_text(stmt,1,opts->command_filter,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,opts->hostname_filter,-1,SQLITE_TRANSIENT);

  while (sqlite3_step(stmt) == SQLITE_ROW){
    const unsigned char *group_raw = sqlite3_column_text(stmt,0);
    const unsigned char *run_id_raw = sqlite3_column_text(stmt,1);
    const unsigned char *metric_raw = sqlite3_column_text(stmt,2);
    double value = sqlite3_column_double(stmt,3);
    const char *group_val = group_raw ? (const char *)group_raw : "(unknown)";
    const char *run_id = run_id_raw ? (const char *)run_id_raw : "?";
    const char *metric_name = metric_raw ? (const char *)metric_raw : "?";

    if (!metric_wanted(opts,metric_name)) continue;
    totals->rows_scanned++;

    if (!have_bucket || strcmp(cur.group_val,group_val) != 0 || strcmp(cur.metric_name,metric_name) != 0){
      if (have_bucket){ emit_bucket(&cur,opts,out,totals); bucket_free_contents(&cur); }
      bucket_reset(&cur,group_val,metric_name);
      have_bucket = 1;
    }
    bucket_add(&cur,value,run_id);
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
    "  --group-by <col>       command (default), hostname, or cpu_vendor\n"
    "  --outlier-stddev <n>   z-score threshold for flagging outliers (default 2.0)\n"
    "  --min-runs <n>         only report (group,metric) buckets with >= n runs (default 1)\n"
    "  --csv                  machine-readable CSV output instead of the human table\n"
    "  -q, --quiet            suppress the trailing summary line\n"
    "  -s, --strict           exit non-zero if any bucket was skipped for --min-runs,\n"
    "                         or if nothing matched at all\n"
    "  -h, --help              show this help\n"
    "\n"
    "Exit status: 0 normally (1 with --strict if any bucket still needs more\n"
    "runs, or nothing matched), 2 on a usage error or if the database could\n"
    "not be opened.\n",
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
    { "outlier-stddev", required_argument, 0, 'z' },
    { "min-runs",       required_argument, 0, 'n' },
    { "csv",            no_argument,       0, 'C' },
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
    case 'z': opts.outlier_z = atof(optarg); break;
    case 'n': opts.min_runs = atoi(optarg); break;
    case 'C': opts.csvflag = 1; break;
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
  if (!parse_group_by(group_by_str,&opts.group_by)){
    fprintf(stderr,"wspy-summary: unrecognized --group-by '%s' (expected command, hostname, "
                    "or cpu_vendor)\n\n",group_by_str);
    usage(argv[0]);
    return 2;
  }
  if (opts.min_runs < 1) opts.min_runs = 1;

  db = open_summary_db(opts.db_path);
  if (!db) return 2;

  memset(&totals,0,sizeof(totals));

  if (opts.csvflag)
    printf("group,metric,n,min,max,mean,median,stddev,cv_percent,outlier_count,outlier_run_ids\n");
  else
    printf("%-28s %-24s %4s %10s %10s %10s %10s %10s %8s %3s  %s\n",
           "group","metric","n","min","max","mean","median","stddev","cv","out","outlier run(s)");

  if (summarize(db,&opts,stdout,&totals) != 0){
    sqlite3_close(db);
    return 2;
  }
  sqlite3_close(db);

  if (!opts.quiet)
    printf("wspy-summary: %d group(s) reported, %d skipped (< %d run(s)), %d run-metric row(s) scanned\n",
           totals.groups_reported,totals.groups_skipped_min_runs,opts.min_runs,totals.rows_scanned);

  if (opts.strict && (totals.groups_skipped_min_runs > 0 || totals.groups_reported == 0)) return 1;
  return 0;
}
#endif /* TEST_SUMMARY */
