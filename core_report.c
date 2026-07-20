/*
 * core_report.c - wspy-core-report: cross-core imbalance/hot-core/core-class
 * diagnostics from an existing --per-core --csv wspy output file.
 * INVESTIGATION.md's 4.2 Tier 1 "per-core imbalance/hot-core diagnostics,
 * core-class summaries" item.
 *
 * A post-hoc report over an already-collected CSV file, matching
 * wspy-validate/wspy-plot's own "analyze an existing artifact" pattern
 * rather than a live collection-time feature -- every non-dimension numeric
 * column in a --per-core CSV (column identity, not which flags produced it,
 * decides what's a metric, the same convention store.c/plot.c already use)
 * gets cross-core min/max/mean/stddev, with the "hot"/"cold" core (max/min
 * value) called out by index. When this host's cores aren't all the same
 * type (cpu_info.c's per-core vendor field -- ARM big.LITTLE, Intel
 * Atom+Core hybrid), an additional breakdown groups the same stats by core
 * class instead of lumping every core together.
 *
 * Must be run on the same host that collected the CSV (or one with
 * identical topology): inventory_cpu() re-detects *this* host's per-core
 * classes fresh at report time -- there's no per-core class column in the
 * CSV itself, matching e.g. wspy-summary --trace's own host-relative-path
 * caveat rather than inventing a new one.
 *
 * AMD Zen5c/Zen5-dense heterogeneous parts aren't yet differentiated at the
 * per-core level by cpu_info.c (every core reports CORE_AMD_ZEN5 uniformly
 * regardless of which physical core type it actually is) -- a pre-existing
 * gap, not something this tool works around; core-class summaries simply
 * won't show a split on such a host yet.
 *
 * Process/thread migration diagnostics (did a process actually move between
 * cores during the run) is a different, harder capability, split out into
 * its own 4.4 backlog item -- nothing here samples scheduling history, only
 * the static end-of-run per-core counter values --per-core already collects.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <getopt.h>
#include "cpu_info.h"
#include "error.h"

#define MAX_CSV_FIELDS 256
#define MAX_CORES 1024
#define MAX_METRICS 128
#define MAX_FILTER_METRICS 64

/* One CSV metric column's per-core accumulated values -- sum/count[i] holds
 * everything seen for core i so far, collapsed to that core's own mean
 * before cross-core comparison (a no-op for the common one-row-per-core
 * shape, but also correctly handles a file with more than one row per core,
 * mirroring summary.c's own per-run AVG() collapse, just per-core here). */
struct metric_accum {
  char name[64];
  double sum[MAX_CORES];
  int count[MAX_CORES];
};

static struct metric_accum metrics[MAX_METRICS];
static int nmetrics = 0;

static int core_seen[MAX_CORES]; /* which core indices actually appeared in the file */
static int max_core_seen = -1;

/* Mirrored verbatim from validate.c/store.c/plot.c's own split_csv_line() --
 * this codebase's established precedent for a tiny CSV-splitting helper is
 * to duplicate it per standalone tool rather than share a module across
 * them (see store.c's own comment on this). line is modified in place. */
static int split_csv_line(char *line,char **fields,int max_fields){
  int n = 0;
  char *p = line;

  if (*p == '\0') return 0;
  fields[n++] = p;
  while (*p && n < max_fields){
    if (*p == ','){
      *p = '\0';
      fields[n++] = p + 1;
    }
    p++;
  }
  return n;
}

/* Mirrored from validate.c's own parse_numeric_field() -- accepts a bare
 * number or one with a trailing '%' (some CSV columns print percentages
 * with the sign included); the percent sign is decoration for this tool's
 * purposes, the numeric part is treated like any other value. */
static int parse_numeric_field(const char *s,double *out){
  char *end;

  if (!s || !*s) return 0;
  *out = strtod(s,&end);
  if (end == s) return 0;
  if (*end == '\0') return 1;
  if (*end == '%' && end[1] == '\0') return 1;
  return 0;
}

static void strip_newline(char *s){
  size_t len = strlen(s);
  while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')){
    s[--len] = '\0';
  }
}

/* Same non-metric exclusion list as plot.c's is_excluded_metric_column():
 * per-run coverage bookkeeping and IBS filter configuration are constant
 * across every row of a run, not a genuine per-core measurement worth
 * cross-core comparison. "core"/"time"/"phase" are dimension columns
 * (store.c/plot.c's own convention), handled separately, not metrics. An
 * empty name is wspy's own trailing-comma artifact (every CSV row here
 * ends with a trailing comma before the newline) -- see store.c's matching
 * comment on the same trailing empty header cell. */
static int is_dimension_or_excluded_column(const char *name){
  return !*name ||
         !strcmp(name,"core") || !strcmp(name,"time") || !strcmp(name,"phase") ||
         !strcmp(name,"counters_measured") || !strcmp(name,"counters_requested") ||
         !strcmp(name,"ibs_l3missonly") || !strcmp(name,"ibs_ldlat_threshold") ||
         !strcmp(name,"ibs_fetchlat_threshold");
}

static struct metric_accum *find_or_add_metric(const char *name){
  int i;
  for (i = 0; i < nmetrics; i++) if (!strcmp(metrics[i].name,name)) return &metrics[i];
  if (nmetrics >= MAX_METRICS){
    fatal("too many metric columns in this CSV (max %d)\n",MAX_METRICS);
  }
  snprintf(metrics[nmetrics].name,sizeof(metrics[nmetrics].name),"%s",name);
  return &metrics[nmetrics++];
}

static int metric_wanted(const char *name,char **filters,int nfilters){
  int i;
  if (nfilters == 0) return 1;
  for (i = 0; i < nfilters; i++) if (!strcmp(filters[i],name)) return 1;
  return 0;
}

/* Cross-core (or cross-class-member) sample statistics over an unordered
 * array of per-core mean values (n>=1). stddev uses the sample (n-1)
 * denominator; for n<2 there is nothing to vary against, so stddev/cv are
 * reported as 0 rather than NaN/undefined, the same convention summary.c's
 * own compute_stats() uses. hot_idx/cold_idx index into values[]/core_ids[]
 * (the max/min value's position), not a core number directly -- the caller
 * maps that back to the real core index. */
struct core_stats {
  int n;
  double min,max,mean,stddev,cv_percent;
  int hot_idx,cold_idx;
};

static void compute_core_stats(const double *values,int n,struct core_stats *out){
  double sum = 0,mean,sumsq = 0;
  int i;

  out->n = n;
  out->hot_idx = 0;
  out->cold_idx = 0;
  out->min = values[0];
  out->max = values[0];
  for (i = 0; i < n; i++){
    sum += values[i];
    if (values[i] > out->max){ out->max = values[i]; out->hot_idx = i; }
    if (values[i] < out->min){ out->min = values[i]; out->cold_idx = i; }
  }
  mean = sum / n;
  for (i = 0; i < n; i++) sumsq += (values[i] - mean) * (values[i] - mean);
  out->mean = mean;
  out->stddev = (n >= 2) ? sqrt(sumsq / (n - 1)) : 0.0;
  out->cv_percent = (mean != 0.0) ? (out->stddev / fabs(mean)) * 100.0 : 0.0;
}

/* Collects one metric's per-core mean values into values[]/core_ids[],
 * optionally restricted to cores of one class. Pure over its arguments (no
 * cpu_info/hardware dependency) except for the file-scope core_seen[]/
 * max_core_seen populated by read_percore_csv() -- core_class[]/class_known[]
 * are passed in rather than read from cpu_info directly so this (and the
 * class-membership logic it implements) can be driven by a synthetic class
 * assignment in test_core_report.c, without needing real or fake hardware
 * topology. Returns the count filled into values[]/core_ids[]. */
static int gather_core_values(const struct metric_accum *m,
                               const enum cpu_core_type *core_class,const int *class_known,
                               int filter_by_class,enum cpu_core_type want_class,
                               double *values,int *core_ids){
  int n = 0,c;

  for (c = 0; c <= max_core_seen; c++){
    if (!core_seen[c] || m->count[c] == 0) continue;
    if (filter_by_class){
      if (!class_known[c] || core_class[c] != want_class) continue;
    }
    core_ids[n] = c;
    values[n] = m->sum[c] / m->count[c];
    n++;
  }
  return n;
}

/* Which distinct core classes are present among the seen cores this CSV
 * covers (not every class a host might have -- a CSV from a restricted
 * --affinity run may only cover one class even on a heterogeneous host).
 * class_known[c] false means core c's class couldn't be determined
 * (typically: this CSV was collected on a different host than the one
 * running this report) -- such a core still counts toward the overall
 * cross-core stats, just never toward a class bucket here. Pure/testable,
 * same reasoning as gather_core_values() above. */
static int distinct_classes_present(const enum cpu_core_type *core_class,const int *class_known,
                                     enum cpu_core_type *out,int max_classes){
  int i,j,n = 0;

  for (i = 0; i <= max_core_seen; i++){
    int already;
    if (!core_seen[i] || !class_known[i]) continue;
    already = 0;
    for (j = 0; j < n; j++) if (out[j] == core_class[i]) { already = 1; break; }
    if (!already && n < max_classes) out[n++] = core_class[i];
  }
  return n;
}

static const char *core_class_name(enum cpu_core_type t){
  switch (t){
    case CORE_UNKNOWN: return "unknown";
    case CORE_ARM_GENERIC: return "arm_generic";
    case CORE_ARM_CORTEX_A53: return "arm_cortex_a53";
    case CORE_ARM_CORTEX_A57: return "arm_cortex_a57";
    case CORE_ARM_CORTEX_A72: return "arm_cortex_a72";
    case CORE_ARM_NEOVERSE_N1: return "arm_neoverse_n1";
    case CORE_ARM_NEOVERSE_V1: return "arm_neoverse_v1";
    case CORE_ARM_NEOVERSE_N2: return "arm_neoverse_n2";
    case CORE_ARM_NEOVERSE_V2: return "arm_neoverse_v2";
    case CORE_ARM_CORTEX_A78: return "arm_cortex_a78";
    case CORE_ARM_CORTEX_X1: return "arm_cortex_x1";
    case CORE_ARM_CORTEX_A710: return "arm_cortex_a710";
    case CORE_ARM_CORTEX_X2: return "arm_cortex_x2";
    case CORE_ARM_CORTEX_A510: return "arm_cortex_a510";
    case CORE_ARM_CORTEX_A520: return "arm_cortex_a520";
    case CORE_ARM_CORTEX_A720: return "arm_cortex_a720";
    case CORE_ARM_CORTEX_X4: return "arm_cortex_x4";
    case CORE_AMD_UNKNOWN: return "amd_unknown";
    case CORE_AMD_ZEN: return "amd_zen";
    case CORE_AMD_ZEN5: return "amd_zen5";
    case CORE_INTEL_UNKNOWN: return "intel_unknown";
    case CORE_INTEL_ATOM: return "intel_atom";
    case CORE_INTEL_CORE: return "intel_core";
  }
  return "unknown";
}

/* Reads the CSV at path, populating metrics[]/core_seen[]. Returns 0 on
 * success, -1 if the file couldn't be opened, or if it has no "core" column
 * at all (not a --per-core CSV -- this tool has exactly one job and can't
 * do it without that column). Malformed rows (field count mismatch) and a
 * non-numeric/out-of-range "core" cell are skipped and counted, not fatal,
 * matching this codebase's degrade-don't-fail idiom elsewhere. */
static int read_percore_csv(const char *path,int *nrows_skipped){
  FILE *fp;
  char line[8192];
  char *raw_header_fields[MAX_CSV_FIELDS];
  char *header_fields[MAX_CSV_FIELDS]; /* owned copies -- raw_header_fields[]
    points into line[], which the loop below reuses for every data row, so
    the header's own field strings must be duplicated before that happens */
  int header_n;
  int core_col = -1;
  int i;

  fp = fopen(path,"r");
  if (!fp){
    error("unable to open %s: %s\n",path,strerror(errno));
    return -1;
  }
  if (!fgets(line,sizeof(line),fp)){
    error("%s: empty file\n",path);
    fclose(fp);
    return -1;
  }
  strip_newline(line);
  header_n = split_csv_line(line,raw_header_fields,MAX_CSV_FIELDS);
  for (i = 0; i < header_n; i++){
    header_fields[i] = strdup(raw_header_fields[i]);
  }
  for (i = 0; i < header_n; i++){
    if (!strcmp(header_fields[i],"core")){ core_col = i; break; }
  }
  if (core_col < 0){
    error("%s: no \"core\" column -- this isn't a --per-core CSV output\n",path);
    fclose(fp);
    return -1;
  }

  /* Second pass over the header: register every non-dimension/non-excluded
   * column as a metric up front, so a metric that happens to be all-NaN or
   * missing from some rows still shows up as a known (if empty) column
   * rather than silently vanishing depending on row order. */
  for (i = 0; i < header_n; i++){
    if (is_dimension_or_excluded_column(header_fields[i])) continue;
    find_or_add_metric(header_fields[i]);
  }

  while (fgets(line,sizeof(line),fp)){
    char *fields[MAX_CSV_FIELDS];
    int field_n,core_idx;
    double core_val;

    strip_newline(line);
    if (line[0] == '\0') continue;
    field_n = split_csv_line(line,fields,MAX_CSV_FIELDS);
    if (field_n != header_n){
      (*nrows_skipped)++;
      continue;
    }
    if (!parse_numeric_field(fields[core_col],&core_val)){
      (*nrows_skipped)++;
      continue;
    }
    core_idx = (int)core_val;
    if (core_idx < 0 || core_idx >= MAX_CORES){
      (*nrows_skipped)++;
      continue;
    }
    if (!core_seen[core_idx]){
      core_seen[core_idx] = 1;
      if (core_idx > max_core_seen) max_core_seen = core_idx;
    }

    for (i = 0; i < header_n; i++){
      double val;
      struct metric_accum *m;

      if (is_dimension_or_excluded_column(header_fields[i])) continue;
      if (!parse_numeric_field(fields[i],&val)) continue;
      if (isnan(val) || isinf(val)) continue;
      m = find_or_add_metric(header_fields[i]);
      m->sum[core_idx] += val;
      m->count[core_idx] += 1;
    }
  }

  fclose(fp);
  return 0;
}

static void print_stats_human(const char *label,const struct core_stats *st,
                               const int *core_ids){
  printf("  %-24s n=%-3d mean=%-12.4f stddev=%-10.4f cv=%6.2f%%  hot=core%d(%.4f)  cold=core%d(%.4f)\n",
         label,st->n,st->mean,st->stddev,st->cv_percent,
         core_ids[st->hot_idx],st->max,core_ids[st->cold_idx],st->min);
}

static void print_stats_csv(const char *metric,const char *scope,const char *scope_value,
                             const struct core_stats *st,const int *core_ids){
  printf("%s,%s,%s,%d,%.6f,%d,%.6f,%d,%.6f,%.6f,%.4f\n",
         metric,scope,scope_value,st->n,
         st->min,core_ids[st->cold_idx],
         st->max,core_ids[st->hot_idx],
         st->mean,st->stddev,st->cv_percent);
}

#ifndef TEST_CORE_REPORT
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s [options] <percore.csv>\n"
    "\n"
    "Cross-core imbalance/hot-core diagnostics from an existing --per-core\n"
    "--csv wspy output file: for every metric column, reports min/max/mean/\n"
    "stddev/coefficient-of-variation across cores, naming the \"hot\" (max)\n"
    "and \"cold\" (min) core. When this host's cores aren't all the same\n"
    "type (ARM big.LITTLE, Intel Atom+Core hybrid), also breaks the same\n"
    "stats down by core class.\n"
    "\n"
    "Must be run on the same host that collected the CSV (or one with\n"
    "identical topology) -- core classes are re-detected fresh from this\n"
    "host, not read from the CSV.\n"
    "\n"
    "Options:\n"
    "  --csv               output as CSV (metric,scope,scope_value,n,min,\n"
    "                      min_core,max,max_core,mean,stddev,cv_percent)\n"
    "                      instead of the default human-readable report\n"
    "  --metric <name>     only report this metric column (repeatable)\n"
    "  -h, --help          show this help\n"
    "\n"
    "Exit status: 0 on success; 2 on a usage error (missing/unreadable\n"
    "file, or a CSV with no \"core\" column).\n",
    prog);
}

int main(int argc,char **argv){
  const char *csv_path;
  int csv_output = 0;
  char *filters[MAX_FILTER_METRICS];
  int nfilters = 0;
  int nrows_skipped = 0;
  int opt,i,j;
  int nclasses_seen;
  enum cpu_core_type classes_seen[32];

  static struct option long_options[] = {
    { "csv",    no_argument,       0, 'c' },
    { "metric", required_argument, 0, 'm' },
    { "help",   no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"h",long_options,NULL)) != -1){
    switch (opt){
    case 'c': csv_output = 1; break;
    case 'm':
      if (nfilters >= MAX_FILTER_METRICS){
        fprintf(stderr,"wspy-core-report: too many --metric filters (max %d)\n",MAX_FILTER_METRICS);
        return 2;
      }
      filters[nfilters++] = optarg;
      break;
    case 'h':
      usage(argv[0]);
      return 0;
    default:
      usage(argv[0]);
      return 2;
    }
  }

  if (optind >= argc){
    usage(argv[0]);
    return 2;
  }
  csv_path = argv[optind];

  if (read_percore_csv(csv_path,&nrows_skipped) != 0){
    return 2;
  }
  if (nrows_skipped){
    warning("%s: skipped %d malformed/unparseable row(s)\n",csv_path,nrows_skipped);
  }
  if (max_core_seen < 0){
    error("%s: no usable per-core data rows found\n",csv_path);
    return 2;
  }

  inventory_cpu();

  /* Per-core class + known-ness arrays, built from this host's own fresh
   * inventory_cpu() (a core index beyond what this host found can't be
   * classified -- different host/topology than the one that collected the
   * CSV -- and is excluded from every class bucket, though it still counts
   * toward the overall cross-core stats above). Passed into
   * distinct_classes_present()/gather_core_values() rather than read from
   * cpu_info inline, so those two stay testable against a synthetic
   * assignment with no real hardware involved. */
  {
    static enum cpu_core_type core_class[MAX_CORES];
    static int class_known[MAX_CORES];

    for (i = 0; i <= max_core_seen; i++){
      if (!core_seen[i] || (unsigned int)i >= cpu_info->num_cores){
        class_known[i] = 0;
        continue;
      }
      core_class[i] = cpu_info->coreinfo[i].vendor;
      class_known[i] = 1;
    }
    nclasses_seen = distinct_classes_present(core_class,class_known,classes_seen,
                                              (int)(sizeof(classes_seen)/sizeof(classes_seen[0])));

    if (csv_output){
      printf("metric,scope,scope_value,n,min,min_core,max,max_core,mean,stddev,cv_percent\n");
    } else {
      printf("core report: %s (%d core(s))\n\n",csv_path,max_core_seen + 1);
      printf("Cross-core stats:\n");
    }

    for (i = 0; i < nmetrics; i++){
      struct metric_accum *m = &metrics[i];
      double values[MAX_CORES];
      int core_ids[MAX_CORES];
      int n;
      struct core_stats st;

      if (!metric_wanted(m->name,filters,nfilters)) continue;

      n = gather_core_values(m,core_class,class_known,0,CORE_UNKNOWN,values,core_ids);
      if (n == 0) continue;

      compute_core_stats(values,n,&st);
      if (csv_output) print_stats_csv(m->name,"all","",&st,core_ids);
      else print_stats_human(m->name,&st,core_ids);
    }

    if (nclasses_seen > 1){
      if (!csv_output) printf("\nCore-class summary (%d classes present):\n",nclasses_seen);

      for (i = 0; i < nmetrics; i++){
        struct metric_accum *m = &metrics[i];

        if (!metric_wanted(m->name,filters,nfilters)) continue;

        for (j = 0; j < nclasses_seen; j++){
          double values[MAX_CORES];
          int core_ids[MAX_CORES];
          int n;
          struct core_stats st;
          const char *class_name = core_class_name(classes_seen[j]);

          n = gather_core_values(m,core_class,class_known,1,classes_seen[j],values,core_ids);
          if (n == 0) continue;

          compute_core_stats(values,n,&st);
          if (csv_output){
            print_stats_csv(m->name,"core_type",class_name,&st,core_ids);
          } else {
            char label[96];
            snprintf(label,sizeof(label),"%s [%s]",m->name,class_name);
            print_stats_human(label,&st,core_ids);
          }
        }
      }
    }
  }

  return 0;
}
#endif /* TEST_CORE_REPORT */
