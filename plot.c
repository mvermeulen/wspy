/*
 * plot.c - wspy-plot: shared plotting templates over wspy CSV output
 * (INVESTIGATION.md's "What shipped in 4.1", "Shared plotting templates").
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
  const char *style;                          /* gnuplot "with" style, e.g. "points" or "lines" */
  /* Optional secondary (right-hand) y axis -- for pairing two metrics that
   * don't share a natural scale (e.g. pkg_watts alongside a 0-100 busy% or
   * a 0-4 IPC ratio) onto one time series instead of flattening one of them
   * on a shared linear axis. y2min_match == 0 (the default for every
   * existing template, via C's zero-init of unspecified trailing fields)
   * means "no secondary axis" -- the whole template behaves exactly as
   * before. When y2min_match > 0, the *entire* template requires both the
   * primary min_match and this secondary one to be met before it fires --
   * a "power-vs-frequency" plot with no frequency data isn't a lesser
   * version of itself, it's not the plot that was asked for. */
  const char *y2_metrics[MAX_TEMPLATE_METRICS];
  const char *y2label;
  int y2min_match;
};

/* Column names below are the literal wspy CSV header text for each counter
 * group (topdown.c's print_*() CSV-header cases) -- see CLAUDE.md's "CSV vs.
 * human output" section for why header text, not position, is load-bearing
 * here. Kept intentionally small: one template per counter group that's
 * meaningful as a time series, not an exhaustive catalog of every column.
 *
 * style is "points" for every hardware-performance-counter template,
 * matching gnuplot.sh's own topdown plot -- it never gave a "with" clause,
 * so gnuplot's default data style (points) applied, and per-tick counter
 * values read better as discrete samples than as an implied-continuous
 * line. "system-cpu" stays "lines", matching gnuplot.sh's systemtime.csv
 * plot (which did say "with lines" explicitly) -- it's a /proc/stat system
 * metric, not a hardware performance counter. The remaining generic/
 * network-io/custom matches (built below, not in this table) also default
 * to "lines" since they're not counter-specific either. */
static const struct plot_template templates[] = {
  { "topdown", "Topdown Breakdown", "% of pipeline slots",
    { "retire","frontend","backend","speculate" }, 2, "points" },
  { "memory-bound", "Memory Boundedness", "% of cycles",
    { "l1_bound","l2_bound","l3_bound","dram_bound","store_bound" }, 2, "points" },
  { "cache-miss", "Cache Miss Rates", "miss rate (%)",
    { "L1-dcache miss","L1-icache miss","iTLB miss","dTLB miss","opcache miss" }, 1, "points" },
  { "system-cpu", "System CPU Time", "% of interval",
    { "cpu","idle","iowait","irq" }, 2, "lines" },
  { "ipc", "Instructions per Cycle", "IPC",
    { "ipc" }, 1, "points" },
  { "branch-miss", "Branch Miss Rate", "miss rate (%)",
    { "branch miss" }, 1, "points" },
  { "float", "Float Instruction Mix", "% of instructions",
    { "float" }, 1, "points" },
  { "ibs", "AMD IBS Sample Counts", "samples per interval",
    { "ibs_fetch","ibs_op","ibs_op_unfiltered" }, 1, "points" },
  { "ibs-accepted-ratio", "IBS Op Accepted Ratio (filtered/unfiltered)", "ratio",
    { "ibs_op_accepted_ratio" }, 1, "points" },
  /* GPU busy/activity percentages (AMD sysfs's gpu_busy/gpu_activity, NVIDIA
   * NVML's nv_gpu_busy) on their own dedicated chart -- without this, these
   * were falling into the generic fallback plot alongside everything else
   * no template claimed, making "how much is actually happening on the GPU"
   * something you had to go dig for rather than see at a glance. */
  { "gpu-utilization", "GPU Utilization", "% busy",
    { "gpu_busy","gpu_activity","nv_gpu_busy" }, 1, "lines" },
  /* VRAM usage (NVIDIA NVML's nv_vram_used_mb/nv_vram_total_mb, AMD SMI's
   * gpu_smi_vram_used/gpu_smi_vram_total) is in MB -- thousands, the same
   * "shares no useful scale with a 0-100 percentage" problem power.c's
   * pkg_watts had, except worse: sharing an axis with 0-100-ish columns in
   * the generic fallback flattened everything else to a line along the
   * bottom (confirmed on a real run: an ~8151 MB total-VRAM column forced
   * every other metric in that plot down near zero). Its own chart, not a
   * secondary axis -- there's no single natural pairing partner for VRAM
   * the way power has utilization/frequency/IPC/topdown. */
  { "gpu-vram", "GPU VRAM Usage", "MB",
    { "nv_vram_used_mb","nv_vram_total_mb","gpu_smi_vram_used","gpu_smi_vram_total" }, 1, "lines" },
  /* power.c's pkg_watts paired against a second variable on its own
   * (right-hand) axis -- watts (tens-hundreds) shares no useful scale with
   * a busy percentage, a MHz frequency, or a 0-4 IPC ratio. Each requires
   * --power's own column *and* the paired column(s) to be present, so a
   * --power-only CSV still falls through to the generic fallback plot
   * exactly as before rather than firing a half-empty pairing. */
  { "power-vs-utilization", "Package Power vs. CPU/GPU Utilization", "Watts",
    { "pkg_watts" }, 1, "lines",
    { "cpu","gpu_busy" }, "% busy", 1 },
  { "power-vs-frequency", "Package Power vs. CPU Frequency", "Watts",
    { "pkg_watts" }, 1, "lines",
    { "freq" }, "MHz", 1 },
  { "power-vs-ipc", "Package Power vs. IPC", "Watts",
    { "pkg_watts" }, 1, "lines",
    { "ipc" }, "IPC", 1 },
  { "power-vs-topdown", "Package Power vs. Topdown Breakdown", "Watts",
    { "pkg_watts" }, 1, "lines",
    { "retire","frontend","backend","speculate" }, "% of pipeline slots", 2 },
  /* cpu_temp (system.c, hwmon-sourced) paired against the columns that tell
   * an actual thermal story -- same secondary-axis reasoning as the power-vs-*
   * templates above, since deg-C shares no useful scale with MHz, Watts, or
   * a 0-100 busy percentage. Each requires --system's own cpu_temp column
   * *and* the paired column(s), so a cpu_temp-only CSV (SYSTEM_TEMP with no
   * --power/other system columns collected) still falls through to the
   * generic fallback plot exactly as before rather than firing half-empty. */
  { "temp-vs-frequency", "CPU Temperature vs. Frequency", "deg C",
    { "cpu_temp" }, 1, "lines",
    { "freq" }, "MHz", 1 },
  { "temp-vs-power", "CPU Temperature vs. Package Power", "deg C",
    { "cpu_temp" }, 1, "lines",
    { "pkg_watts" }, "Watts", 1 },
  { "temp-vs-utilization", "CPU Temperature vs. Utilization", "deg C",
    { "cpu_temp" }, 1, "lines",
    { "cpu","gpu_busy" }, "% busy", 1 },
  /* AMD sysfs's own gpu_temp/gpu_freq -- same temp-vs-frequency pairing as
   * the CPU templates above, since gpu_freq (hundreds-thousands of MHz)
   * dominates a shared axis with anything percentage-scale exactly the way
   * cpu_temp's own frequency column would. Confirmed on the same real run
   * that surfaced the VRAM issue above: once VRAM had its own chart,
   * gpu_freq became the next thing flattening the generic fallback plot. */
  { "gpu-thermal", "GPU Temperature vs. Frequency", "deg C",
    { "gpu_temp" }, 1, "lines",
    { "gpu_freq" }, "MHz", 1 },
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
  const char *style;   /* gnuplot "with" style; always "lines" outside match_templates() */
  int time_col;
  int cols[MAX_CSV_FIELDS];
  int col_axis[MAX_CSV_FIELDS]; /* parallel to cols[]: 0 = primary (y1), 1 = secondary (y2) --
                                  * only meaningful (and only ever read) when y2label[0] != '\0' */
  int ncols;
  char y2label[64];     /* empty (the default every builder must set explicitly) means no
                          * secondary axis -- render_match() never consults col_axis[] otherwise */
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
 * struct plot_match per template that reaches its min_match threshold,
 * starting at index n (so a caller can run this after --plot's custom
 * specs and have both land in the same matches[] array). Matched header
 * columns are marked in claimed[] (size >= header_n) so a following
 * fallback pass can find what's left over. Returns the new total match
 * count. */
static int match_templates(char * const *header_fields,int header_n,int time_col,
                            struct plot_match *matches,int max_matches,int *claimed,int n){
  int t;

  for (t = 0; t < ntemplates && n < max_matches; t++){
    const struct plot_template *tmpl = &templates[t];
    int found[MAX_TEMPLATE_METRICS];
    int nfound = 0,m,i;
    int y2found[MAX_TEMPLATE_METRICS];
    int ny2found = 0;
    struct plot_match *pm;

    for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->metrics[m]; m++){
      int idx = find_col(header_fields,header_n,tmpl->metrics[m]);
      if (idx >= 0) found[nfound++] = idx;
    }
    if (nfound < tmpl->min_match) continue;

    if (tmpl->y2min_match > 0){
      for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->y2_metrics[m]; m++){
        int idx = find_col(header_fields,header_n,tmpl->y2_metrics[m]);
        if (idx >= 0) y2found[ny2found++] = idx;
      }
      if (ny2found < tmpl->y2min_match) continue; /* secondary-axis pairing not satisfied */
    }

    pm = &matches[n++];
    snprintf(pm->name,sizeof(pm->name),"%s",tmpl->name);
    snprintf(pm->title,sizeof(pm->title),"%s",tmpl->title);
    snprintf(pm->ylabel,sizeof(pm->ylabel),"%s",tmpl->ylabel);
    pm->y2label[0] = '\0';
    pm->style = tmpl->style;
    pm->time_col = time_col;
    pm->ncols = 0;
    for (i = 0; i < nfound; i++){
      pm->cols[pm->ncols] = found[i];
      pm->col_axis[pm->ncols] = 0;
      claimed[found[i]] = 1;
      pm->ncols++;
    }
    if (ny2found > 0){
      snprintf(pm->y2label,sizeof(pm->y2label),"%s",tmpl->y2label);
      for (i = 0; i < ny2found; i++){
        pm->cols[pm->ncols] = y2found[i];
        pm->col_axis[pm->ncols] = 1;
        claimed[y2found[i]] = 1;
        pm->ncols++;
      }
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
  return !strcmp(name,"counters_measured") || !strcmp(name,"counters_requested") ||
         /* ibs_l3missonly/ibs_ldlat_threshold/ibs_fetchlat_threshold (topdown.c's
          * print_ibs()) are per-run filter configuration, constant on every row --
          * not a per-tick workload measurement, same reasoning as the coverage
          * columns above. */
         !strcmp(name,"ibs_l3missonly") || !strcmp(name,"ibs_ldlat_threshold") ||
         !strcmp(name,"ibs_fetchlat_threshold");
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
  pm->y2label[0] = '\0';
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
  pm->style = "lines";
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
  pm->y2label[0] = '\0';
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
  pm->style = "lines";
  pm->time_col = time_col;
  return n + 1;
}

#define MAX_CUSTOM_PLOTS 32
#define MAX_CUSTOM_COLUMNS 16

/* One user-specified --plot NAME=col1,col2,... spec (parsed once at
 * startup, applied against every CSV scanned) -- the "configure specific
 * counters onto one plot yourself" escape hatch the built-in templates
 * above don't offer: they're a fixed, curated catalog of known counter
 * groups, not a general column-picker. Most useful when several counters
 * are individually meaningful as a time series but don't share a natural
 * scale with any built-in template's grouping (e.g. mixing one counter
 * group's percentage columns with another's raw counts) -- put exactly the
 * ones that belong together in one --plot spec instead. */
struct custom_plot_spec {
  char name[64];
  char columns[MAX_CUSTOM_COLUMNS][128];
  int ncolumns;
  /* Columns after an optional ';' in the --plot argument -- rendered on a
   * secondary (right-hand) y axis, for pairing columns that don't share a
   * scale with the primary group (e.g. pkg_watts;freq). Empty (ny2columns
   * == 0) is the common case and behaves exactly as before. */
  char y2columns[MAX_CUSTOM_COLUMNS][128];
  int ny2columns;
};

/* Parses "NAME=col1,col2,...[;col3,col4,...]" (the --plot argument text)
 * into spec -- columns before an optional ';' go on the primary axis,
 * columns after it (if any) go on the secondary axis. Returns 1 on
 * success, 0 if the argument has no '=', an empty name, or no column names
 * at all on either side -- a usage error the caller should reject up
 * front, not something to discover per-CSV later. */
static int parse_custom_plot_spec(const char *arg,struct custom_plot_spec *spec){
  const char *eq = strchr(arg,'=');
  char cols_buf[1024];
  char *secondary,*tok,*saveptr;
  size_t name_len;

  if (!eq || eq == arg) return 0;
  name_len = (size_t)(eq - arg);
  if (name_len >= sizeof(spec->name)) name_len = sizeof(spec->name) - 1;
  memcpy(spec->name,arg,name_len);
  spec->name[name_len] = '\0';

  snprintf(cols_buf,sizeof(cols_buf),"%s",eq + 1);
  spec->ncolumns = 0;
  spec->ny2columns = 0;

  secondary = strchr(cols_buf,';');
  if (secondary){
    *secondary = '\0';
    secondary++;
  }

  tok = strtok_r(cols_buf,",",&saveptr);
  while (tok && spec->ncolumns < MAX_CUSTOM_COLUMNS){
    snprintf(spec->columns[spec->ncolumns],sizeof(spec->columns[0]),"%s",tok);
    spec->ncolumns++;
    tok = strtok_r(NULL,",",&saveptr);
  }
  if (secondary){
    tok = strtok_r(secondary,",",&saveptr);
    while (tok && spec->ny2columns < MAX_CUSTOM_COLUMNS){
      snprintf(spec->y2columns[spec->ny2columns],sizeof(spec->y2columns[0]),"%s",tok);
      spec->ny2columns++;
      tok = strtok_r(NULL,",",&saveptr);
    }
  }
  return *spec->name && (spec->ncolumns > 0 || spec->ny2columns > 0);
}

/* Matches one --plot spec against a CSV header: any of its named columns
 * that's actually present in *this* CSV is included, one present column is
 * enough to fire (the user asked for these specific columns, so there's no
 * "not enough of them" threshold the way a built-in template has) --
 * unlike a built-in template, a --plot spec doesn't check claimed[] before
 * matching (the whole point is to let the same column also appear in a
 * plot it would otherwise have been grouped into) but does mark claimed[]
 * afterward, so the generic/network fallbacks don't also duplicate it. A
 * named column absent from this particular CSV is warned about once (not
 * silently dropped) since hand-picking columns is meant to be precise; if
 * every named column is absent, no plot is produced for this spec at all. */
static int add_custom_plot_match(char * const *header_fields,int header_n,int time_col,
                                  const struct custom_plot_spec *spec,int *claimed,
                                  struct plot_match *matches,int max_matches,int n,
                                  const char *csv_path,int quiet){
  struct plot_match *pm;
  int i,ny2added = 0;

  if (n >= max_matches) return n;
  pm = &matches[n];
  pm->ncols = 0;
  pm->y2label[0] = '\0';
  for (i = 0; i < spec->ncolumns; i++){
    int idx = find_col(header_fields,header_n,spec->columns[i]);

    if (idx < 0){
      if (!quiet)
        fprintf(stderr,"wspy-plot: %s: --plot %s: column '%s' not in this CSV's header, skipping it\n",
                csv_path,spec->name,spec->columns[i]);
      continue;
    }
    if (pm->ncols < MAX_CSV_FIELDS){
      pm->cols[pm->ncols] = idx;
      pm->col_axis[pm->ncols] = 0;
      pm->ncols++;
    }
  }
  for (i = 0; i < spec->ny2columns; i++){
    int idx = find_col(header_fields,header_n,spec->y2columns[i]);

    if (idx < 0){
      if (!quiet)
        fprintf(stderr,"wspy-plot: %s: --plot %s: column '%s' (secondary axis) not in this CSV's "
                       "header, skipping it\n",csv_path,spec->name,spec->y2columns[i]);
      continue;
    }
    if (pm->ncols < MAX_CSV_FIELDS){
      pm->cols[pm->ncols] = idx;
      pm->col_axis[pm->ncols] = 1;
      pm->ncols++;
      ny2added++;
    }
  }
  if (pm->ncols == 0) return n;
  for (i = 0; i < pm->ncols; i++) claimed[pm->cols[i]] = 1;
  snprintf(pm->name,sizeof(pm->name),"%s",spec->name);
  snprintf(pm->title,sizeof(pm->title),"%s",spec->name);
  snprintf(pm->ylabel,sizeof(pm->ylabel),"value");
  if (ny2added > 0) snprintf(pm->y2label,sizeof(pm->y2label),"value (secondary)");
  pm->style = "lines";
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

/* Stable per-metric-name line color -- gnuplot's own automatic color
 * cycling assigns colors by column *position* within one plot invocation,
 * so the same metric (e.g. "backend") can render in a different color on
 * different charts depending on what else happens to share that particular
 * chart, making it harder to visually track one metric across several PNGs
 * from the same run. This instead keys off the column name itself: a small
 * curated table for metrics that recur across more than one template
 * (topdown's four, the columns power/temp pairings share), and a stable
 * hash-based fallback -- not gnuplot's own cycling -- for everything else
 * (per-core columns, host-specific network interface names), so the same
 * column always renders the same color, run to run and chart to chart. */
struct metric_color { const char *name; const char *rgb; };
static const struct metric_color metric_colors[] = {
  { "retire",             "#d62728" }, /* red */
  { "frontend",           "#1f77b4" }, /* blue */
  { "backend",            "#2ca02c" }, /* green */
  { "speculate",          "#9467bd" }, /* purple */
  { "ipc",                "#1f77b4" },
  { "cpu",                "#1f77b4" },
  { "idle",               "#7f7f7f" }, /* gray */
  { "iowait",             "#ff7f0e" }, /* orange */
  { "irq",                "#9467bd" },
  { "freq",               "#2ca02c" },
  { "cpu_temp",           "#d62728" },
  { "pkg_watts",          "#ff7f0e" },
  { "pkg_joules",         "#bcbd22" }, /* olive */
  { "gpu_busy",           "#2ca02c" },
  { "gpu_activity",       "#17becf" }, /* cyan */
  { "nv_gpu_busy",        "#1f77b4" },
  { "gpu_temp",           "#d62728" },
  { "gpu_power",          "#ff7f0e" },
  { "gpu_freq",           "#2ca02c" },
  { "nv_vram_used_mb",    "#1f77b4" },
  { "nv_vram_total_mb",   "#7f7f7f" },
  { "gpu_smi_vram_used",  "#1f77b4" },
  { "gpu_smi_vram_total", "#7f7f7f" },
  { NULL, NULL },
};
/* Palette for names not in the curated table above -- a fixed, visually
 * distinct set, indexed by a stable hash of the column name rather than
 * gnuplot's own position-based cycling. */
static const char *fallback_palette[] = {
  "#1f77b4","#ff7f0e","#2ca02c","#d62728","#9467bd",
  "#8c564b","#e377c2","#7f7f7f","#bcbd22","#17becf",
};
#define FALLBACK_PALETTE_SIZE (sizeof(fallback_palette)/sizeof(fallback_palette[0]))

static const char *metric_line_color(const char *name){
  unsigned int hash = 2166136261u; /* FNV-1a */
  const char *p;
  int i;

  for (i = 0; metric_colors[i].name; i++){
    if (!strcmp(name,metric_colors[i].name)) return metric_colors[i].rgb;
  }
  for (p = name; *p; p++){
    hash ^= (unsigned char) *p;
    hash *= 16777619u;
  }
  return fallback_palette[hash % FALLBACK_PALETTE_SIZE];
}

/* Renders one match via gnuplot -- a generated script piped to gnuplot's
 * stdin (functionally the same idiom gnuplot.sh's heredocs used), reusing
 * this process's own stdout/stderr for gnuplot's diagnostics rather than
 * capturing them, so a caller that pipes wspy-plot's own output (e.g.
 * web/server.py's subprocess.Popen with stderr=STDOUT) sees them too.
 * header_fields lets it look up each plotted column's own name (for
 * metric_line_color() above) -- pm->cols[] only carries column indices. */
static int render_match(const char *csv_path,const struct plot_match *pm,const char *out_png,
                         char * const *header_fields){
  FILE *gp;
  int i;
  int rc;
  int has_y2 = pm->y2label[0] != '\0';

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
  if (has_y2){
    /* nomirror: without it gnuplot draws y1's tics on the right edge too,
     * which then collide visually with y2's own tics there. */
    fprintf(gp,"set ytics nomirror\n");
    fprintf(gp,"set y2tics\n");
    fprintf(gp,"set y2label '%s'\n",pm->y2label);
  }
  fprintf(gp,"plot ");
  for (i = 0; i < pm->ncols; i++){
    if (i) fprintf(gp,", ");
    fprintf(gp,"'%s' using %d:%d with %s linecolor rgb '%s'",
            csv_path,pm->time_col + 1,pm->cols[i] + 1,pm->style,
            metric_line_color(header_fields[pm->cols[i]]));
    if (has_y2 && pm->col_axis[i] == 1) fprintf(gp," axes x1y2");
  }
  fprintf(gp,"\n");
  rc = pclose(gp);
  if (rc != 0){
    /* gnuplot's "set output" already created (and possibly partially
     * wrote) out_png before the plot command itself failed -- e.g. every
     * data point undefined because the requested counters had no perf
     * access this run. Remove the stale/empty file rather than leaving a
     * broken-looking 0-byte PNG sitting in plots/ for a report to link to. */
    remove(out_png);
    return -1;
  }
  return 0;
}

/* Parses one CSV's header line, matches it against every built-in template
 * plus the generic fallback, and renders whatever matched into out_dir.
 * Returns the number of plots rendered (0 if the CSV has no "time" column,
 * or has one but nothing matched), or -1 if the CSV couldn't be read at
 * all or a gnuplot invocation failed (any_failed is also set in that
 * case) -- matching the rest of this codebase's "measured vs unavailable"
 * degrade-don't-fail idiom: a CSV this tool doesn't know how to plot is
 * not an error, only a gnuplot invocation that was attempted and failed is. */
/* Decides *what* to plot for one CSV header -- custom --plot specs, then
 * (unless only_custom) the built-in templates and both fallback buckets --
 * without touching gnuplot or the filesystem, so this half of process_csv()
 * is unit-testable on its own (test_plot.c does exactly that) without
 * needing gnuplot installed. Returns the number of matches appended to
 * matches[] (0 if the header has no "time" column or nothing matched). */
static int build_plot_matches(char * const *header_fields,int header_n,int time_col,
                               const struct custom_plot_spec *custom_plots,int ncustom_plots,
                               int only_custom,struct plot_match *matches,int max_matches,
                               const char *csv_path,int quiet){
  int core_col,phase_col;
  int claimed[MAX_CSV_FIELDS];
  int nmatches,i;

  core_col = find_col(header_fields,header_n,"core");
  phase_col = find_col(header_fields,header_n,"phase");

  memset(claimed,0,sizeof(claimed));
  nmatches = 0;
  for (i = 0; i < ncustom_plots; i++)
    nmatches = add_custom_plot_match(header_fields,header_n,time_col,&custom_plots[i],claimed,
                                      matches,max_matches,nmatches,csv_path,quiet);
  if (!only_custom){
    nmatches = match_templates(header_fields,header_n,time_col,matches,max_matches,claimed,nmatches);
    nmatches = add_network_fallback_match(header_fields,header_n,time_col,claimed,
                                           matches,max_matches,nmatches);
    nmatches = add_fallback_match(header_fields,header_n,time_col,core_col,phase_col,
                                   claimed,matches,max_matches,nmatches);
  }
  return nmatches;
}

static int process_csv(const char *csv_path,const char *out_dir,int quiet,int *any_failed,
                        const struct custom_plot_spec *custom_plots,int ncustom_plots,
                        int only_custom){
  FILE *fp;
  char line[8192];
  char *header_fields[MAX_CSV_FIELDS];
  int header_n;
  int time_col;
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

  nmatches = build_plot_matches(header_fields,header_n,time_col,custom_plots,ncustom_plots,
                                 only_custom,matches,MAX_CSV_FIELDS,csv_path,quiet);

  basename_without_ext(csv_path,stem,sizeof(stem));
  for (i = 0; i < nmatches; i++){
    char out_png[MAX_PATH_LEN];

    snprintf(out_png,sizeof(out_png),"%s/%s.%s.png",out_dir,stem,matches[i].name);
    if (render_match(csv_path,&matches[i],out_png,header_fields) != 0){
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
    printf("  %-22s %-40s (needs %d of: ",tmpl->name,tmpl->title,tmpl->min_match);
    for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->metrics[m]; m++)
      printf("%s%s",m ? ", " : "",tmpl->metrics[m]);
    printf(")");
    if (tmpl->y2min_match > 0){
      printf(" + %d of secondary axis: ",tmpl->y2min_match);
      for (m = 0; m < MAX_TEMPLATE_METRICS && tmpl->y2_metrics[m]; m++)
        printf("%s%s",m ? ", " : "",tmpl->y2_metrics[m]);
    }
    printf("\n");
  }
  printf("  %-14s %-28s (any \"net <iface>\" column, e.g. --system's per-interface byte counters)\n",
         "network-io","Network I/O (fallback)");
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
    "  --plot NAME=col1,col2,...[;col3,col4,...]\n"
    "                      define a custom plot grouping exactly these CSV\n"
    "                      column names together (repeatable) -- useful when\n"
    "                      counters that matter to you don't share a scale\n"
    "                      with any built-in template's grouping, so you'd\n"
    "                      rather chart them on a plot of their own. A named\n"
    "                      column missing from a given CSV is skipped with a\n"
    "                      warning, not fatal; if none of NAME's columns are\n"
    "                      present in a CSV, no plot is produced for it there.\n"
    "                      Columns after an optional ';' are plotted on a\n"
    "                      secondary (right-hand) y axis, for pairing columns\n"
    "                      that don't share a scale with the primary group,\n"
    "                      e.g. --plot power=pkg_watts;freq pairs Watts\n"
    "                      against MHz instead of flattening one of them.\n"
    "  --only-custom       skip every built-in template and fallback plot --\n"
    "                      render only the --plot spec(s) given. Requires at\n"
    "                      least one --plot.\n"
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
  int only_custom = 0;
  struct custom_plot_spec custom_plots[MAX_CUSTOM_PLOTS];
  int ncustom_plots = 0;
  int opt,i;
  int any_failed = 0,total_rendered = 0,total_csvs = 0;

  static struct option long_options[] = {
    { "rundir",         required_argument, 0, 'r' },
    { "csv",            required_argument, 0, 'c' },
    { "out-dir",        required_argument, 0, 'o' },
    { "plot",           required_argument, 0, 'p' },
    { "only-custom",    no_argument,       0, 'y' },
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
    case 'p':
      if (ncustom_plots >= MAX_CUSTOM_PLOTS){
        fprintf(stderr,"wspy-plot: too many --plot specs (max %d)\n",MAX_CUSTOM_PLOTS);
        return 2;
      }
      if (!parse_custom_plot_spec(optarg,&custom_plots[ncustom_plots])){
        fprintf(stderr,"wspy-plot: --plot '%s': expected NAME=col1,col2,... "
                       "(non-empty name, at least one column)\n",optarg);
        return 2;
      }
      ncustom_plots++;
      break;
    case 'y': only_custom = 1; break;
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
  if (only_custom && ncustom_plots == 0){
    fprintf(stderr,"wspy-plot: --only-custom requires at least one --plot\n\n");
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
      r = process_csv(full,out_dir,quiet,&any_failed,custom_plots,ncustom_plots,only_custom);
      if (r > 0){ total_rendered += r; total_csvs++; }
      free(namelist[i]);
    }
    free(namelist);
  }
  for (i = 0; i < ncsv; i++){
    int r = process_csv(csv_paths[i],out_dir,quiet,&any_failed,custom_plots,ncustom_plots,only_custom);
    if (r > 0){ total_rendered += r; total_csvs++; }
  }

  if (!quiet)
    printf("wspy-plot: %d plot(s) generated from %d CSV file(s)\n",total_rendered,total_csvs);

  return any_failed ? 1 : 0;
}
#endif /* TEST_PLOT */
