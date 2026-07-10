/*
 * validate.c - wspy-validate: basic pre-publish quality checks against a
 * wspy run manifest (see manifest.h) and the output files it references.
 *
 * This is the "basic validation/quality checks... required files present,
 * non-empty CSV, exit status, sanity ranges" item from INVESTIGATION_4.0.md
 * ("Run artifact foundation" track) -- the manifest and coverage work it
 * consumes already ships (manifest.c, coverage.c); this is the first reader
 * in the tree that parses a manifest.json back rather than just writing one.
 *
 * Scope is deliberately per-run, not per-suite: given one manifest.json,
 * check that what it claims happened is plausible before the run's output
 * gets folded into a published result set. Scanning a whole run index or
 * output tree for a batch of runs is the separate "coverage ledger" idea.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <unistd.h>
#include <getopt.h>
#include "json_reader.h"
#include "manifest.h"

enum severity { SEV_PASS, SEV_WARN, SEV_FAIL };

struct check {
  enum severity sev;
  char msg[320];
};

#define MAX_CHECKS 256
struct report {
  struct check checks[MAX_CHECKS];
  int count;
  int fail_count;
  int warn_count;
};

static void add_check(struct report *r,enum severity sev,const char *fmt,...){
  va_list ap;
  struct check *c;

  if (sev == SEV_FAIL) r->fail_count++;
  else if (sev == SEV_WARN) r->warn_count++;
  if (r->count >= MAX_CHECKS) return;
  c = &r->checks[r->count++];
  c->sev = sev;
  va_start(ap,fmt);
  vsnprintf(c->msg,sizeof(c->msg),fmt,ap);
  va_end(ap);
}

static const char *sev_label(enum severity sev){
  switch (sev){
  case SEV_PASS: return "PASS";
  case SEV_WARN: return "WARN";
  case SEV_FAIL: return "FAIL";
  }
  return "?";
}

static enum severity overall_severity(const struct report *r){
  if (r->fail_count) return SEV_FAIL;
  if (r->warn_count) return SEV_WARN;
  return SEV_PASS;
}

/* Known numeric CSV columns with a tighter-than-generic plausible range,
 * beyond the "non-negative and finite" default every numeric column gets.
 * Extend this table when a new counter is worth a specific bound -- see
 * CLAUDE.md's "Common edits" for how new CSV columns get added. */
struct sanity_bound { const char *column; double min,max; };
static const struct sanity_bound sanity_bounds[] = {
  { "ipc", 0.0, 32.0 },
};
#define NUM_SANITY_BOUNDS (sizeof(sanity_bounds) / sizeof(sanity_bounds[0]))

/* Percentage-valued cells (e.g. "26.61%") are printed as plain text with a
 * trailing '%', not a normalized 0-1 fraction, and system.c's load/cpu
 * columns are allowed to exceed 100% when aggregated across cores -- so the
 * bound here is a loose "not garbage" check, not a strict percentage range. */
#define PERCENT_SANITY_MAX 1000.0

static char *read_whole_file(const char *path,long *size_out){
  FILE *fp;
  long size;
  char *buf;

  fp = fopen(path,"rb");
  if (!fp) return NULL;
  fseek(fp,0,SEEK_END);
  size = ftell(fp);
  fseek(fp,0,SEEK_SET);
  if (size < 0){ fclose(fp); return NULL; }
  buf = malloc((size_t)size + 1);
  if (size > 0 && fread(buf,1,(size_t)size,fp) != (size_t)size){
    free(buf);
    fclose(fp);
    return NULL;
  }
  buf[size] = '\0';
  fclose(fp);
  *size_out = size;
  return buf;
}

/* Splits a CSV line in place on ',' (mutating line), preserving empty
 * fields -- including the trailing empty field wspy's own CSV writers emit
 * after their last real column (a trailing comma before the newline). */
#define MAX_CSV_FIELDS 1024
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

/* Splits buf (the full contents of a file) into a NULL-terminated array of
 * mutable line strings (newlines stripped, blank lines dropped). Caller
 * frees the returned array with free() -- the line pointers point into buf,
 * which the caller must keep alive at least as long as the array. */
static char **split_lines(char *buf,int *count_out){
  char **lines = NULL;
  int cap = 0,n = 0;
  char *p = buf;

  while (*p){
    char *line_start = p;
    while (*p && *p != '\n') p++;
    if (*p == '\n'){ *p = '\0'; p++; }
    if (*line_start && line_start[strlen(line_start)-1] == '\r') line_start[strlen(line_start)-1] = '\0';
    if (*line_start){
      if (n == cap){ cap = cap ? cap * 2 : 16; lines = realloc(lines,cap * sizeof(*lines)); }
      lines[n++] = line_start;
    }
  }
  *count_out = n;
  return lines;
}

static int parse_numeric_field(const char *s,double *out,int *is_percent){
  char *end;

  if (!s || !*s) return 0;
  *out = strtod(s,&end);
  if (end == s) return 0;
  if (*end == '\0'){ *is_percent = 0; return 1; }
  if (*end == '%' && end[1] == '\0'){ *is_percent = 1; return 1; }
  return 0;
}

static void check_csv_row_sanity(struct report *r,char **header,int header_n,
                                  char **fields,int field_n,int row_number,int *values_checked){
  int j;

  if (field_n != header_n) return; /* already flagged by the caller as a malformed row */
  for (j = 0; j < header_n; j++){
    const char *name = header[j];
    const char *val = fields[j];
    double num;
    int is_pct,k;

    if (!parse_numeric_field(val,&num,&is_pct)) continue;
    (*values_checked)++;

    // Must run before any bounds comparison, named or generic: NaN/Inf
    // compare false against every "<"/">" bound (a NaN column value would
    // otherwise silently pass a named sanity_bounds[] entry, e.g. "ipc",
    // since "NaN < min || NaN > max" is always false in C -- only the
    // generic fallback below used to check isnan()/isinf(), so a column
    // with its own named bound skipped that catch-all entirely).
    if (isnan(num) || isinf(num)){
      add_check(r,SEV_FAIL,"sanity: row %d column '%s' = %s is not a finite number",row_number,name,val);
      continue;
    }

    if (is_pct){
      if (num < 0 || num > PERCENT_SANITY_MAX)
        add_check(r,SEV_FAIL,"sanity: row %d column '%s' = %s%% outside plausible range [0,%.0f]",
                  row_number,name,val,PERCENT_SANITY_MAX);
      continue;
    }

    for (k = 0; k < (int)NUM_SANITY_BOUNDS; k++){
      if (!strcmp(name,sanity_bounds[k].column)){
        if (num < sanity_bounds[k].min || num > sanity_bounds[k].max)
          add_check(r,SEV_FAIL,"sanity: row %d column '%s' = %s outside plausible range [%.1f,%.1f]",
                    row_number,name,val,sanity_bounds[k].min,sanity_bounds[k].max);
        break;
      }
    }
    if (k < (int)NUM_SANITY_BOUNDS) continue;

    if (num < 0)
      add_check(r,SEV_FAIL,"sanity: row %d column '%s' = %s is negative",row_number,name,val);
    else if (num > 1e12)
      add_check(r,SEV_FAIL,"sanity: row %d column '%s' = %s is implausibly large",row_number,name,val);
  }
}

/* Checks the CSV file at path: non-empty (header + at least one data row),
 * every data row has the same column count as the header, and per-cell
 * sanity bounds via check_csv_row_sanity(). */
static void validate_csv_file(struct report *r,const char *path){
  long size;
  char *buf;
  char **lines;
  int nlines,i;
  char *header_fields[MAX_CSV_FIELDS];
  int header_n;
  int mismatches = 0,values_checked = 0,data_rows = 0;

  buf = read_whole_file(path,&size);
  if (!buf){
    add_check(r,SEV_FAIL,"unable to read output file: %s",path);
    return;
  }
  if (size == 0){
    add_check(r,SEV_FAIL,"output CSV is empty: %s",path);
    free(buf);
    return;
  }
  lines = split_lines(buf,&nlines);
  if (nlines == 0){
    add_check(r,SEV_FAIL,"output CSV has no header row: %s",path);
    free(lines);
    free(buf);
    return;
  }

  header_n = split_csv_line(lines[0],header_fields,MAX_CSV_FIELDS);

  if (nlines < 2){
    add_check(r,SEV_FAIL,"output CSV has a header but no data rows: %s",path);
    free(lines);
    free(buf);
    return;
  }

  for (i = 1; i < nlines; i++){
    char *row_fields[MAX_CSV_FIELDS];
    int row_n = split_csv_line(lines[i],row_fields,MAX_CSV_FIELDS);

    data_rows++;
    if (row_n != header_n){
      mismatches++;
      if (mismatches <= 5)
        add_check(r,SEV_FAIL,"row %d has %d column(s), expected %d (from header)",i,row_n,header_n);
      continue;
    }
    check_csv_row_sanity(r,header_fields,header_n,row_fields,row_n,i,&values_checked);
  }
  if (mismatches > 5)
    add_check(r,SEV_FAIL,"%d more row(s) with a column-count mismatch (not shown)",mismatches - 5);

  if (!mismatches)
    add_check(r,SEV_PASS,"output CSV well-formed: %d column(s), %d data row(s)",header_n,data_rows);
  if (values_checked && r->fail_count == 0)
    add_check(r,SEV_PASS,"sanity checks passed on %d numeric value(s) across %d row(s)",values_checked,data_rows);

  free(lines);
  free(buf);
}

static void validate_nonempty_file(struct report *r,const char *path,const char *what){
  long size;
  char *buf = read_whole_file(path,&size);

  if (!buf){
    add_check(r,SEV_FAIL,"unable to read %s: %s",what,path);
    return;
  }
  if (size == 0) add_check(r,SEV_FAIL,"%s is empty: %s",what,path);
  else add_check(r,SEV_PASS,"%s non-empty (%ld bytes): %s",what,size,path);
  free(buf);
}

static void check_schema_version(struct report *r,const struct json_value *root){
  const char *version = json_get_string(root,"schema_version",NULL);
  int major,expected_major;

  if (!version){
    add_check(r,SEV_FAIL,"no schema_version field -- doesn't look like a wspy manifest");
    return;
  }
  major = atoi(version);
  expected_major = atoi(MANIFEST_SCHEMA_VERSION);
  if (major != expected_major)
    add_check(r,SEV_WARN,"manifest schema version %s has a different major version than this tool understands (%s)",
              version,MANIFEST_SCHEMA_VERSION);
  else
    add_check(r,SEV_PASS,"schema version %s recognized",version);
}

static void check_exit_status(struct report *r,const struct json_value *root){
  const struct json_value *es = json_object_get(root,"exit_status");

  if (!es){
    add_check(r,SEV_WARN,"no exit_status recorded in manifest");
    return;
  }
  if (!json_get_bool(es,"known",0)){
    add_check(r,SEV_WARN,"workload exit status was not recorded this run");
    return;
  }
  if (json_get_bool(es,"signaled",0)){
    add_check(r,SEV_FAIL,"workload was terminated by signal %d",(int)json_get_number(es,"term_signal",-1));
    return;
  }
  if (json_get_bool(es,"exited",0)){
    int code = (int)json_get_number(es,"exit_code",-1);
    if (code == 0) add_check(r,SEV_PASS,"workload exited normally (status 0)");
    else add_check(r,SEV_FAIL,"workload exited with nonzero status %d",code);
    return;
  }
  add_check(r,SEV_WARN,"exit_status recorded but neither exited nor signaled is set");
}

static void check_counter_coverage(struct report *r,const struct json_value *root){
  const struct json_value *cc = json_object_get(root,"counter_coverage");
  int requested,measured;

  if (!cc) return; /* older/foreign manifest without this field -- not an error */
  requested = (int)json_get_number(cc,"requested",0);
  measured = (int)json_get_number(cc,"measured",0);
  if (requested == 0) return; /* no counters requested this run (e.g. --system only) */
  if (measured < requested)
    add_check(r,SEV_WARN,"partial counter coverage: %d/%d counters measured",measured,requested);
  else
    add_check(r,SEV_PASS,"full counter coverage: %d/%d counters measured",measured,requested);
}

static void check_elapsed_time(struct report *r,const struct json_value *root){
  const struct json_value *timing = json_object_get(root,"timing");
  double elapsed;

  if (!timing){
    add_check(r,SEV_WARN,"no timing block in manifest");
    return;
  }
  elapsed = json_get_number(timing,"elapsed_seconds",-1);
  if (elapsed <= 0) add_check(r,SEV_FAIL,"non-positive elapsed_seconds (%.3f) recorded",elapsed);
  else add_check(r,SEV_PASS,"elapsed time %.3fs",elapsed);
}

/* Checks output_files: every referenced path must exist. Also runs the
 * CSV/non-empty checks on the "output" and "tree" entries, since whether a
 * file should look like CSV depends on options.csv from the same manifest. */
static void check_output_files(struct report *r,const struct json_value *root){
  const struct json_value *files = json_object_get(root,"output_files");
  int is_csv = json_get_bool(json_object_get(root,"options"),"csv",0);
  size_t i,n,present = 0;

  if (!files || json_array_len(files) == 0){
    add_check(r,SEV_WARN,"no output files recorded in manifest");
    return;
  }
  n = json_array_len(files);
  for (i = 0; i < n; i++){
    const struct json_value *entry = json_array_get(files,i);
    const char *kind = json_get_string(entry,"kind","?");
    const char *path = json_get_string(entry,"path",NULL);

    if (!path){
      add_check(r,SEV_FAIL,"output_files[%zu] has no path",i);
      continue;
    }
    if (access(path,F_OK) != 0){
      add_check(r,SEV_FAIL,"required file missing: %s (kind=%s)",path,kind);
      continue;
    }
    present++;

    if (!strcmp(kind,"output")){
      if (is_csv) validate_csv_file(r,path);
      else validate_nonempty_file(r,path,"output file");
    } else if (!strcmp(kind,"tree")){
      validate_nonempty_file(r,path,"tree file");
    }
    /* kind "manifest" is the file we're reading -- its existence is already
     * proven by having parsed it, nothing further to check. */
  }
  if (present == n) add_check(r,SEV_PASS,"required files present (%zu/%zu)",present,n);
}

static enum severity validate_manifest(const char *path,int quiet){
  char errbuf[256];
  struct json_value *root;
  struct report r;
  int i;
  enum severity overall;

  memset(&r,0,sizeof(r));

  root = json_parse_file(path,errbuf,sizeof(errbuf));
  if (!root){
    printf("%s: FAIL\n  [FAIL] %s\n",path,errbuf);
    return SEV_FAIL;
  }
  if (root->type != JSON_OBJECT){
    printf("%s: FAIL\n  [FAIL] top-level JSON value is not an object\n",path);
    json_free(root);
    return SEV_FAIL;
  }

  check_schema_version(&r,root);
  check_output_files(&r,root);
  check_exit_status(&r,root);
  check_counter_coverage(&r,root);
  check_elapsed_time(&r,root);

  json_free(root);

  overall = overall_severity(&r);
  if (quiet && overall != SEV_FAIL) return overall;

  printf("%s: %s\n",path,sev_label(overall));
  for (i = 0; i < r.count; i++)
    printf("  [%s] %s\n",sev_label(r.checks[i].sev),r.checks[i].msg);
  if (r.count > MAX_CHECKS)
    printf("  (%d additional check result(s) not shown)\n",r.count - MAX_CHECKS);

  return overall;
}

#ifndef TEST_VALIDATE
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s [options] <manifest.json> [<manifest.json> ...]\n"
    "\n"
    "Runs basic pre-publish quality checks against one or more wspy run\n"
    "manifests: required output files present, output CSV well-formed and\n"
    "non-empty, workload exit status, counter coverage, and sanity ranges\n"
    "on numeric CSV columns.\n"
    "\n"
    "Options:\n"
    "  -q, --quiet    only print manifests with at least one failed check\n"
    "  -s, --strict   treat warnings as failures for the exit status\n"
    "  -h, --help     show this help\n"
    "\n"
    "Exit status: 0 if every manifest passed (warnings allowed unless\n"
    "--strict), 1 if any manifest had a failed check, 2 on a usage error.\n",
    prog);
}

int main(int argc,char **argv){
  int quiet = 0,strict = 0;
  int opt,i;
  int total = 0,failed = 0,warned = 0,passed = 0;

  static struct option long_options[] = {
    { "quiet",  no_argument, 0, 'q' },
    { "strict", no_argument, 0, 's' },
    { "help",   no_argument, 0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"qsh",long_options,NULL)) != -1){
    switch (opt){
    case 'q': quiet = 1; break;
    case 's': strict = 1; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (optind >= argc){
    fprintf(stderr,"%s: no manifest file(s) given\n\n",argv[0]);
    usage(argv[0]);
    return 2;
  }

  for (i = optind; i < argc; i++){
    enum severity sev = validate_manifest(argv[i],quiet);
    total++;
    if (sev == SEV_FAIL) failed++;
    else if (sev == SEV_WARN) warned++;
    else passed++;
  }

  printf("%d manifest(s) checked: %d passed, %d warned, %d failed\n",total,passed,warned,failed);

  if (failed || (strict && warned)) return 1;
  return 0;
}
#endif /* TEST_VALIDATE */
