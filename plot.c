/*
 * plot.c - wspy-plot: shared plotting templates over wspy CSV output
 * (INVESTIGATION_4.0.md 4.1 Tier 2, item 12 "Shared plotting templates").
 *
 * Replaces workload/phoronix/gnuplot.sh's single hardcoded script, which
 * only knew how to plot two literal filenames (amdtopdown.csv,
 * systemtime.csv) at fixed column positions, with a small, reusable
 * template table matched against a CSV's *header* -- the same
 * normalized-schema convention store.c's ingest_csv_metrics() already
 * established for the normalized store (a column named exactly "time"/
 * "core"/"phase" is a dimension, every other non-empty-named column is a
 * metric; column *identity*, not position or which flags were passed,
 * decides what a CSV holds). A template fires against a given CSV once
 * enough of its candidate metric columns are present in that CSV's header
 * (position within the file doesn't matter, and extra columns -- e.g. a
 * newer wspy adding "confidence,sanity," after the topdown group's core
 * four -- don't stop it matching); any metric columns left unclaimed by a
 * firing template still get one generic "metrics" plot, so an unfamiliar
 * or future counter-group combination still produces *something* rather
 * than nothing.
 *
 * Only CSVs with a "time" column (i.e. produced with --interval) are
 * plottable time series -- an aggregate (non---interval) CSV has one row
 * and nothing to chart, so it's skipped without being treated as an error.
 *
 * Rendering itself still delegates to gnuplot, exactly as gnuplot.sh
 * always did -- this only generalizes *what* gets asked of it and *how
 * many* different CSV shapes it can be asked about, not the rendering
 * engine underneath.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <getopt.h>

#define MAX_CSV_FIELDS 128
#define MAX_TEMPLATE_METRICS 8
#define MAX_PATH_LEN 4096
#define MAX_NAME_LEN 256

struct plot_template {
  const char *name;                          /* filesystem-safe; becomes part of the output filename */
  const char *title;
  const char *ylabel;
  const char *metrics[MAX_TEMPLATE_METRICS]; /* candidate metric column names, preferred plot order */
  int min_match;                              /* how many of metrics[] must be present to fire */
};

/* Column names below are the literal wspy CSV header text for each counter
 * group (topdown.c's print_*() CSV-header cases) -- see CLAUDE.md's "CSV vs.
 * human output" section for why header text, not position, is load-bearing
 * here. Kept intentionally small: one template per counter group that's
 * meaningful as a time series, not an exhaustive catalog of every column. */
static const struct plot_template templates[] = {
  { "topdown", "Topdown Breakdown", "% of pipeline slots",
    { "retire","frontend","backend","speculate" }, 2 },
  { "memory-bound", "Memory Boundedness", "% of cycles",
    { "l1_bound","l2_bound","l3_bound","dram_bound","store_bound" }, 2 },
  { "cache-miss", "Cache Miss Rates", "miss rate (%)",
    { "L1-dcache miss","L1-icache miss","iTLB miss","dTLB miss","opcache miss" }, 1 },
  { "system-cpu", "System CPU Time", "% of interval",
    { "cpu","idle","iowait","irq" }, 2 },
  { "ipc", "Instructions per Cycle", "IPC",
    { "ipc" }, 1 },
  { "branch-miss", "Branch Miss Rate", "miss rate (%)",
    { "branch miss" }, 1 },
  { "float", "Float Instruction Mix", "% of instructions",
    { "float" }, 1 },
};
static const int ntemplates = sizeof(templates) / sizeof(templates[0]);

/* One template's match against one CSV's header: which header columns
 * (0-based) satisfy it, in metrics[] order. Also used, with tmpl_name ==
 * NULL, for the generic fallback plot of whatever metric columns no
 * specific template claimed. */
struct plot_match {
  char name[64];
  char title[128];
  char ylabel[64];
  int time_col;
  int cols[MAX_CSV_FIELDS];
  int ncols;
};

/* Mirrored verbatim from validate.c/store.c's own split_csv_line() -- this
 * codebase's established precedent for a tiny CSV-splitting helper is to
 * duplicate it per standalone tool rather than share a module across them
 * (see store.c's own comment on this). line is modified in place. */
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

static int find_col(char * const *header_fields,int header_n,const char *name){
  int i;
  for (i = 0; i < header_n; i++) if (!strcmp(header_fields[i],name)) return i;
  return -1;
}

/* Matches every built-in template against one CSV header, appending a
 * struct plot_match per template that reaches its min_match threshold.
 * Matched header columns are marked in claimed[] (size >= header_n) so a
 * following fallback pass can find what's left over. Returns the number of
 * matches appended (<= ntemplates). */
static int match_templates(char * const *header_fields,int header_n,int time_col,
                            struct plot_match *matches,int max_matches,int *claimed){
  int t,n = 0;

  for (t = 0; t < ntemplates && n < max_matches; t++){
    const struct plot_template *tmpl = &templates[t];
    int found[MAX_TEMPLATE_METRICS];
    int nfound = 0,m,i;
    struct plot_match *pm;

    for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->metrics[m]; m++){
      int idx = find_col(header_fields,header_n,tmpl->metrics[m]);
      if (idx >= 0) found[nfound++] = idx;
    }
    if (nfound < tmpl->min_match) continue;

    pm = &matches[n++];
    snprintf(pm->name,sizeof(pm->name),"%s",tmpl->name);
    snprintf(pm->title,sizeof(pm->title),"%s",tmpl->title);
    snprintf(pm->ylabel,sizeof(pm->ylabel),"%s",tmpl->ylabel);
    pm->time_col = time_col;
    pm->ncols = nfound;
    for (i = 0; i < nfound; i++){
      pm->cols[i] = found[i];
      claimed[found[i]] = 1;
    }
  }
  return n;
}

/* Per-run counter-coverage bookkeeping (coverage.c's print_counter_coverage(),
 * present on every CSV row regardless of which counter groups were
 * requested) is a numeric, non-dimension column by the same rule everything
 * else here uses, but it's not a per-tick *workload* metric -- plotting it
 * alongside real metrics only adds a constant flat line and a legend entry.
 * Excluded from every match (templates and fallbacks alike) rather than
 * just the generic fallback, in case a future template's candidate list
 * ever collided with these names. */
static int is_excluded_metric_column(const char *name){
  return !strcmp(name,"counters_measured") || !strcmp(name,"counters_requested");
}

/* Appends one "network-io" plot covering every column whose name starts
 * with "net " (system.c's per-interface byte counters, e.g. "net eth0" --
 * one column per interface discovered on this host, so the exact names
 * can't be a fixed template entry the way a counter group's fixed column
 * set can). Grouped separately from add_fallback_match()'s generic sweep
 * because network byte counters run orders of magnitude larger than the
 * load-average/percentage columns --system also emits; plotted on the same
 * linear axis, the byte counters flatten everything else in the same plot
 * to an indistinguishable line at the bottom. Returns the new total match
 * count (unchanged if no "net " column is present). */
static int add_network_fallback_match(char * const *header_fields,int header_n,
                                       int time_col,int *claimed,
                                       struct plot_match *matches,int max_matches,int n){
  struct plot_match *pm;
  int i;

  if (n >= max_matches) return n;
  pm = &matches[n];
  pm->ncols = 0;
  for (i = 0; i < header_n; i++){
    if (claimed[i] || !*header_fields[i]) continue;
    if (strncmp(header_fields[i],"net ",4)) continue;
    if (pm->ncols < MAX_CSV_FIELDS) pm->cols[pm->ncols++] = i;
  }
  if (pm->ncols == 0) return n;
  for (i = 0; i < pm->ncols; i++) claimed[pm->cols[i]] = 1;
  snprintf(pm->name,sizeof(pm->name),"network-io");
  snprintf(pm->title,sizeof(pm->title),"Network I/O");
  snprintf(pm->ylabel,sizeof(pm->ylabel),"bytes");
  pm->time_col = time_col;
  return n + 1;
}

/* Appends one generic "metrics" plot covering every metric column (i.e.
 * not "time"/"core"/"phase", not empty-named, not excluded bookkeeping,
 * not already claimed by a firing template or the network-io bucket above)
 * still left in the header, so a counter-group combination with no
 * dedicated template still produces something. Returns the new total match
 * count (unchanged if there was nothing left over). */
static int add_fallback_match(char * const *header_fields,int header_n,
                               int time_col,int core_col,int phase_col,
                               const int *claimed,struct plot_match *matches,
                               int max_matches,int n){
  struct plot_match *pm;
  int i;

  if (n >= max_matches) return n;
  pm = &matches[n];
  pm->ncols = 0;
  for (i = 0; i < header_n; i++){
    if (i == time_col || i == core_col || i == phase_col) continue;
    if (!*header_fields[i]) continue; /* trailing empty header cell from wspy's own trailing comma */
    if (claimed[i]) continue;
    if (is_excluded_metric_column(header_fields[i])) continue;
    if (pm->ncols < MAX_CSV_FIELDS) pm->cols[pm->ncols++] = i;
  }
  if (pm->ncols == 0) return n;
  snprintf(pm->name,sizeof(pm->name),"metrics");
  snprintf(pm->title,sizeof(pm->title),"Other Metrics");
  snprintf(pm->ylabel,sizeof(pm->ylabel),"value");
  pm->time_col = time_col;
  return n + 1;
}

static void basename_without_ext(const char *path,char *out,size_t outsize){
  const char *slash = strrchr(path,'/');
  const char *base = slash ? slash + 1 : path;
  const char *dot = strrchr(base,'.');
  size_t len = dot ? (size_t)(dot - base) : strlen(base);

  if (len >= outsize) len = outsize - 1;
  memcpy(out,base,len);
  out[len] = '\0';
}

/* Renders one match via gnuplot -- a generated script piped to gnuplot's
 * stdin (functionally the same idiom gnuplot.sh's heredocs used), reusing
 * this process's own stdout/stderr for gnuplot's diagnostics rather than
 * capturing them, so a caller that pipes wspy-plot's own output (e.g.
 * web/server.py's subprocess.Popen with stderr=STDOUT) sees them too. */
static int render_match(const char *csv_path,const struct plot_match *pm,const char *out_png){
  FILE *gp;
  int i;
  int rc;

  gp = popen("gnuplot","w");
  if (!gp){
    fprintf(stderr,"wspy-plot: unable to launch gnuplot: %s\n",strerror(errno));
    return -1;
  }
  /* noenhanced: gnuplot's default "enhanced" text mode treats "_"/"^" as
   * subscript/superscript markup, which mangles a legend autotitled from a
   * literal column name like "counters_measured" (renders as "counters"
   * with a subscripted "measured") or "L1-dcache miss" (harmless here, but
   * "_" appears in several real wspy column names). Column names are data,
   * not markup, so render them literally. */
  fprintf(gp,"set terminal png size 1280,960 noenhanced\n");
  fprintf(gp,"set output '%s'\n",out_png);
  fprintf(gp,"set title '%s'\n",pm->title);
  fprintf(gp,"set datafile separator ','\n");
  fprintf(gp,"set key autotitle columnhead\n");
  fprintf(gp,"set xlabel 'time (s)'\n");
  fprintf(gp,"set ylabel '%s'\n",pm->ylabel);
  fprintf(gp,"plot ");
  for (i = 0; i < pm->ncols; i++){
    if (i) fprintf(gp,", ");
    fprintf(gp,"'%s' using %d:%d with lines",csv_path,pm->time_col + 1,pm->cols[i] + 1);
  }
  fprintf(gp,"\n");
  rc = pclose(gp);
  return (rc == 0) ? 0 : -1;
}

/* Parses one CSV's header line, matches it against every built-in template
 * plus the generic fallback, and renders whatever matched into out_dir.
 * Returns the number of plots rendered (0 if the CSV has no "time" column,
 * or has one but nothing matched), or -1 if the CSV couldn't be read at
 * all or a gnuplot invocation failed (any_failed is also set in that
 * case) -- matching the rest of this codebase's "measured vs unavailable"
 * degrade-don't-fail idiom: a CSV this tool doesn't know how to plot is
 * not an error, only a gnuplot invocation that was attempted and failed is. */
static int process_csv(const char *csv_path,const char *out_dir,int quiet,int *any_failed){
  FILE *fp;
  char line[8192];
  char *header_fields[MAX_CSV_FIELDS];
  int header_n;
  int time_col,core_col,phase_col;
  int claimed[MAX_CSV_FIELDS];
  struct plot_match matches[MAX_CSV_FIELDS];
  int nmatches,i,rendered = 0;
  char stem[MAX_NAME_LEN];

  fp = fopen(csv_path,"r");
  if (!fp){
    fprintf(stderr,"wspy-plot: unable to read %s: %s\n",csv_path,strerror(errno));
    return -1;
  }
  if (!fgets(line,sizeof(line),fp)){
    fclose(fp);
    return 0; /* empty file -- nothing to plot, not an error */
  }
  fclose(fp);
  line[strcspn(line,"\r\n")] = '\0';

  header_n = split_csv_line(line,header_fields,MAX_CSV_FIELDS);
  time_col = find_col(header_fields,header_n,"time");
  if (time_col < 0){
    if (!quiet)
      fprintf(stderr,"wspy-plot: %s: no 'time' column (not produced with --interval), skipping\n",csv_path);
    return 0;
  }
  core_col = find_col(header_fields,header_n,"core");
  phase_col = find_col(header_fields,header_n,"phase");

  memset(claimed,0,sizeof(claimed));
  nmatches = match_templates(header_fields,header_n,time_col,matches,MAX_CSV_FIELDS,claimed);
  nmatches = add_network_fallback_match(header_fields,header_n,time_col,claimed,
                                         matches,MAX_CSV_FIELDS,nmatches);
  nmatches = add_fallback_match(header_fields,header_n,time_col,core_col,phase_col,
                                 claimed,matches,MAX_CSV_FIELDS,nmatches);

  basename_without_ext(csv_path,stem,sizeof(stem));
  for (i = 0; i < nmatches; i++){
    char out_png[MAX_PATH_LEN];

    snprintf(out_png,sizeof(out_png),"%s/%s.%s.png",out_dir,stem,matches[i].name);
    if (render_match(csv_path,&matches[i],out_png) != 0){
      fprintf(stderr,"wspy-plot: %s: gnuplot failed rendering '%s' template\n",csv_path,matches[i].name);
      *any_failed = 1;
      continue;
    }
    rendered++;
    if (!quiet) printf("wspy-plot: %s -> %s (%s)\n",csv_path,out_png,matches[i].title);
  }
  return rendered;
}

static int is_csv_name(const char *name){
  size_t len = strlen(name);
  return len > 4 && !strcmp(name + len - 4,".csv");
}

static int csv_filter(const struct dirent *de){
  return is_csv_name(de->d_name);
}

static void print_template_catalog(void){
  int t,m;

  printf("Built-in plot templates:\n");
  for (t = 0; t < ntemplates; t++){
    const struct plot_template *tmpl = &templates[t];
    printf("  %-14s %-28s (needs %d of: ",tmpl->name,tmpl->title,tmpl->min_match);
    for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->metrics[m]; m++)
      printf("%s%s",m ? ", " : "",tmpl->metrics[m]);
    printf(")\n");
  }
  printf("  %-14s %-28s (any metric column left unclaimed by the above)\n","metrics","Other Metrics (fallback)");
}

#ifndef TEST_PLOT
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s [options] --rundir <dir>\n"
    "   or: %s [options] --csv <file> [--csv <file> ...]\n"
    "\n"
    "Scans one or more wspy CSV output files -- every *.csv directly inside\n"
    "--rundir (wspy-run's own unified-output-layout convention), plus any\n"
    "--csv given explicitly -- and renders any matching shared plot template\n"
    "(see --list-templates) into --out-dir via gnuplot, one PNG per matching\n"
    "template per CSV.\n"
    "\n"
    "A CSV with no \"time\" column (not produced with --interval) has no\n"
    "time series to chart and is silently skipped, as is one whose header\n"
    "matches no template at all.\n"
    "\n"
    "Options:\n"
    "  --rundir <dir>      scan <dir> for *.csv files (not recursive)\n"
    "  --csv <file>        a specific CSV file to plot (repeatable)\n"
    "  --out-dir <dir>     where to write *.png (default: <rundir>/plots,\n"
    "                      or \".\" if --rundir was not given)\n"
    "  --list-templates    print the built-in template catalog and exit\n"
    "  -q, --quiet         suppress per-plot progress lines\n"
    "  -h, --help          show this help\n"
    "\n"
    "Exit status: 0 if nothing that was attempted failed (a CSV with no\n"
    "time column or no template match is not a failure); 1 if any gnuplot\n"
    "invocation failed; 2 on a usage error.\n",
    prog,prog);
}

int main(int argc,char **argv){
  const char *rundir = NULL;
  const char *csv_paths[64];
  int ncsv = 0;
  const char *out_dir = NULL;
  char out_dir_buf[MAX_PATH_LEN];
  int quiet = 0;
  int opt,i;
  int any_failed = 0,total_rendered = 0,total_csvs = 0;

  static struct option long_options[] = {
    { "rundir",         required_argument, 0, 'r' },
    { "csv",            required_argument, 0, 'c' },
    { "out-dir",        required_argument, 0, 'o' },
    { "list-templates", no_argument,       0, 'l' },
    { "quiet",          no_argument,       0, 'q' },
    { "help",           no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"qh",long_options,NULL)) != -1){
    switch (opt){
    case 'r': rundir = optarg; break;
    case 'c':
      if (ncsv >= (int)(sizeof(csv_paths)/sizeof(csv_paths[0]))){
        fprintf(stderr,"wspy-plot: too many --csv files\n");
        return 2;
      }
      csv_paths[ncsv++] = optarg;
      break;
    case 'o': out_dir = optarg; break;
    case 'l': print_template_catalog(); return 0;
    case 'q': quiet = 1; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }

  if (!rundir && ncsv == 0){
    fprintf(stderr,"wspy-plot: nothing to do -- give --rundir <dir> or --csv <file>\n\n");
    usage(argv[0]);
    return 2;
  }

  if (!out_dir){
    if (rundir){
      snprintf(out_dir_buf,sizeof(out_dir_buf),"%s/plots",rundir);
      out_dir = out_dir_buf;
    } else {
      out_dir = ".";
    }
  }
  if (mkdir(out_dir,0755) != 0 && errno != EEXIST){
    fprintf(stderr,"wspy-plot: unable to create output directory %s: %s\n",out_dir,strerror(errno));
    return 2;
  }

  if (rundir){
    struct dirent **namelist;
    int n = scandir(rundir,&namelist,csv_filter,alphasort);

    if (n < 0){
      fprintf(stderr,"wspy-plot: unable to scan %s: %s\n",rundir,strerror(errno));
      return 2;
    }
    for (i = 0; i < n; i++){
      char full[MAX_PATH_LEN];
      int r;

      snprintf(full,sizeof(full),"%s/%s",rundir,namelist[i]->d_name);
      r = process_csv(full,out_dir,quiet,&any_failed);
      if (r > 0){ total_rendered += r; total_csvs++; }
      free(namelist[i]);
    }
    free(namelist);
  }
  for (i = 0; i < ncsv; i++){
    int r = process_csv(csv_paths[i],out_dir,quiet,&any_failed);
    if (r > 0){ total_rendered += r; total_csvs++; }
  }

  if (!quiet)
    printf("wspy-plot: %d plot(s) generated from %d CSV file(s)\n",total_rendered,total_csvs);

  return any_failed ? 1 : 0;
}
#endif /* TEST_PLOT */
