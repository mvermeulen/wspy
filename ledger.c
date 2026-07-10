/*
 * ledger.c - wspy-ledger: the "coverage ledger" idea from
 * INVESTIGATION_4.0.md ("Run artifact foundation" track): for a suite of
 * workloads (e.g. a SPEC CPU2017 or Phoronix benchmark list), report each
 * workload's status -- done / skipped / unsupported / needs-tool-support --
 * generated from a shared --run-index file instead of the hand-maintained
 * "what's still missing" text files this replaces (workload/phoronix's
 * phoronix.tests.txt, the "Intel not supported" early-exit in
 * workload/cpu2017/run_test.sh).
 *
 * Not to be confused with coverage.c/coverage.h, which is per-run hardware
 * *counter* coverage (perf_event_open successes vs failures within one
 * wspy invocation). This is suite-level *workload* coverage across many
 * runs, read back from the run index -- the "scanning a whole run index...
 * for a batch of runs" scope validate.c's own comment calls out as
 * deliberately out of scope for wspy-validate.
 *
 * Input is a workload list file: one workload name per line, optionally
 * followed by a tab-separated status ("unsupported" or "needs-tool-support")
 * and a free-text note -- an explicit annotation that overrides whatever
 * the run index would otherwise imply (e.g. a workload known to fail to
 * build, or one that needs a GPU-enabled wspy build that isn't available
 * here). Unannotated workloads get their status inferred by matching their
 * name as a substring against each run-index record's "command" array:
 * no match -> skipped, at least one successful matching run -> done, one or
 * more matching runs but none succeeded -> needs-tool-support.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <getopt.h>
#include "json_reader.h"

enum ledger_status { LEDGER_DONE, LEDGER_SKIPPED, LEDGER_UNSUPPORTED, LEDGER_NEEDS_TOOL_SUPPORT };

#define MAX_NAME 128
#define MAX_NOTE 256
#define MAX_RUN_ID 64
#define MAX_TIMESTAMP 40

struct ledger_entry {
  char name[MAX_NAME];
  int annotated;                     /* 1 if status came from the workload list, not inferred */
  enum ledger_status annotated_status; /* valid only if annotated */
  char note[MAX_NOTE];                /* free-text reason, may be empty */

  int runs_matched;
  int runs_succeeded;
  int last_succeeded;                 /* whether the most recent matching run succeeded */
  char last_run_id[MAX_RUN_ID];
  char last_start_time[MAX_TIMESTAMP]; /* ISO-8601, sorts correctly as a string */
};

static const char *status_label(enum ledger_status s){
  switch (s){
  case LEDGER_DONE: return "done";
  case LEDGER_SKIPPED: return "skipped";
  case LEDGER_UNSUPPORTED: return "unsupported";
  case LEDGER_NEEDS_TOOL_SUPPORT: return "needs-tool-support";
  }
  return "?";
}

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

/* Splits buf into a NULL-terminated-count array of mutable line strings
 * (newlines stripped, blank lines dropped). Caller frees the returned
 * array; the line pointers point into buf. */
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

static char *rtrim(char *s){
  size_t n = strlen(s);
  while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t')) s[--n] = '\0';
  return s;
}

static char *ltrim(char *s){
  while (*s == ' ' || *s == '\t') s++;
  return s;
}

/* Parses one workload-list line into up to three tab-separated fields:
 * name, status keyword, free-text note. Returns 0 for a blank or '#'
 * comment line, 1 otherwise (name/status/note point into line, status and
 * note are NULL if not present). */
static int parse_ledger_line(char *line,char **name,char **status,char **note){
  char *p = ltrim(line);
  char *tab;

  if (*p == '\0' || *p == '#') return 0;

  *name = rtrim(p);
  *status = NULL;
  *note = NULL;
  tab = strchr(p,'\t');
  if (!tab) return 1;
  *tab = '\0';
  rtrim(*name);

  p = ltrim(tab + 1);
  *status = p;
  tab = strchr(p,'\t');
  if (!tab){ rtrim(*status); return 1; }
  *tab = '\0';
  rtrim(*status);

  *note = ltrim(tab + 1);
  return 1;
}

static struct ledger_entry *load_workload_list(const char *path,int *count_out){
  long size;
  char *buf;
  char **lines;
  int nlines,i,cap = 0,n = 0;
  struct ledger_entry *entries = NULL;

  buf = read_whole_file(path,&size);
  if (!buf){
    fprintf(stderr,"wspy-ledger: unable to read workload list: %s\n",path);
    return NULL;
  }
  lines = split_lines(buf,&nlines);
  for (i = 0; i < nlines; i++){
    char *name,*status_str,*note;
    struct ledger_entry *e;

    if (!parse_ledger_line(lines[i],&name,&status_str,&note)) continue;
    if (!*name) continue;

    if (n == cap){ cap = cap ? cap * 2 : 64; entries = realloc(entries,cap * sizeof(*entries)); }
    e = &entries[n++];
    memset(e,0,sizeof(*e));
    snprintf(e->name,sizeof(e->name),"%s",name);
    if (status_str && *status_str){
      if (!strcasecmp(status_str,"unsupported")){
        e->annotated = 1;
        e->annotated_status = LEDGER_UNSUPPORTED;
      } else if (!strcasecmp(status_str,"needs-tool-support")){
        e->annotated = 1;
        e->annotated_status = LEDGER_NEEDS_TOOL_SUPPORT;
      } else {
        fprintf(stderr,"wspy-ledger: %s:%d: unrecognized status '%s', ignoring annotation\n",path,i+1,status_str);
      }
    }
    if (note && *note) snprintf(e->note,sizeof(e->note),"%s",note);
  }
  free(lines);
  free(buf);
  *count_out = n;
  return entries;
}

static int command_matches(const struct json_value *record,const char *name){
  const struct json_value *cmd = json_object_get(record,"command");
  size_t i,n;

  if (!cmd || cmd->type != JSON_ARRAY) return 0;
  n = json_array_len(cmd);
  for (i = 0; i < n; i++){
    const struct json_value *item = json_array_get(cmd,i);
    if (item && item->type == JSON_STRING && strstr(item->u.string,name)) return 1;
  }
  return 0;
}

static int record_succeeded(const struct json_value *record){
  const struct json_value *es = json_object_get(record,"exit_status");

  if (!es) return 0;
  if (!json_get_bool(es,"known",0)) return 0;
  if (json_get_bool(es,"signaled",0)) return 0;
  if (!json_get_bool(es,"exited",0)) return 0;
  return (int)json_get_number(es,"exit_code",-1) == 0;
}

/* Scans one run-index (JSONL) file, updating every ledger_entry whose name
 * matches a record's command line. Returns the number of records parsed,
 * or -1 if the file could not be read. Malformed lines are logged to
 * stderr and skipped rather than aborting the whole file, since a run
 * index is meant to tolerate being scanned mid-append. */
static int process_run_index_file(const char *path,struct ledger_entry *entries,int nentries){
  long size;
  char *buf;
  char **lines;
  int nlines,i,j,records_read = 0;

  buf = read_whole_file(path,&size);
  if (!buf){
    fprintf(stderr,"wspy-ledger: unable to read run index file: %s\n",path);
    return -1;
  }
  lines = split_lines(buf,&nlines);
  for (i = 0; i < nlines; i++){
    char errbuf[256];
    struct json_value *root = json_parse(lines[i],errbuf,sizeof(errbuf));
    const char *run_id,*start_time;

    if (!root){
      fprintf(stderr,"wspy-ledger: %s:%d: skipping malformed record: %s\n",path,i+1,errbuf);
      continue;
    }
    records_read++;
    run_id = json_get_string(root,"run_id","?");
    start_time = json_get_string(root,"start_time","");

    for (j = 0; j < nentries; j++){
      struct ledger_entry *e = &entries[j];

      if (!command_matches(root,e->name)) continue;
      e->runs_matched++;
      if (record_succeeded(root)) e->runs_succeeded++;

      /* Keep the fields for the most recent matching run by start_time
       * (ISO-8601 sorts correctly as a plain string comparison), so
       * multiple --run-index files don't need to be given in
       * chronological order. */
      if (strcmp(start_time,e->last_start_time) >= 0){
        e->last_succeeded = record_succeeded(root);
        snprintf(e->last_run_id,sizeof(e->last_run_id),"%s",run_id);
        snprintf(e->last_start_time,sizeof(e->last_start_time),"%s",start_time);
      }
    }
    json_free(root);
  }
  free(lines);
  free(buf);
  return records_read;
}

static enum ledger_status entry_status(const struct ledger_entry *e){
  if (e->annotated) return e->annotated_status;
  if (e->runs_matched == 0) return LEDGER_SKIPPED;
  if (e->runs_succeeded > 0) return LEDGER_DONE;
  return LEDGER_NEEDS_TOOL_SUPPORT;
}

static void format_detail(const struct ledger_entry *e,enum ledger_status status,char *buf,size_t bufsize){
  if (e->annotated && (status == LEDGER_UNSUPPORTED || status == LEDGER_NEEDS_TOOL_SUPPORT)){
    snprintf(buf,bufsize,"%s",e->note[0] ? e->note : "marked in workload list");
    return;
  }
  switch (status){
  case LEDGER_SKIPPED:
    snprintf(buf,bufsize,"no matching run found in run index");
    break;
  case LEDGER_DONE:
    snprintf(buf,bufsize,"%d/%d run(s) succeeded, most recent %s",
             e->runs_succeeded,e->runs_matched,e->last_run_id);
    break;
  case LEDGER_NEEDS_TOOL_SUPPORT:
    snprintf(buf,bufsize,"%d run(s) attempted, none succeeded, most recent %s",
             e->runs_matched,e->last_run_id);
    break;
  default:
    buf[0] = '\0';
  }
}

static void print_csv_field(const char *s){
  int needs_quote = (strchr(s,',') != NULL) || (strchr(s,'"') != NULL);
  const char *p;

  if (!needs_quote){ fputs(s,stdout); return; }
  fputc('"',stdout);
  for (p = s; *p; p++){
    if (*p == '"') fputc('"',stdout);
    fputc(*p,stdout);
  }
  fputc('"',stdout);
}

#ifndef TEST_LEDGER
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s [options] --run-index <file> [--run-index <file> ...] <workload-list>\n"
    "\n"
    "Generates a \"coverage ledger\" for a suite of workloads: for each\n"
    "workload named in <workload-list>, reports whether it is done (a\n"
    "successful matching run exists in the run index), skipped (no matching\n"
    "run at all), unsupported, or needs-tool-support -- the last two either\n"
    "inferred (matching runs exist but none succeeded) or explicitly\n"
    "annotated in <workload-list>.\n"
    "\n"
    "<workload-list> is one workload name per line, optionally followed by a\n"
    "tab-separated status (\"unsupported\" or \"needs-tool-support\") and a\n"
    "free-text note. Blank lines and lines starting with '#' are ignored.\n"
    "A workload's name is matched as a substring against each run-index\n"
    "record's command line.\n"
    "\n"
    "Options:\n"
    "  --run-index <file>  run-index (JSONL) file to scan; may be repeated\n"
    "  --csv               machine-readable CSV output instead of the human report\n"
    "  -q, --quiet         only print workloads that are not done\n"
    "  -s, --strict        exit non-zero if any workload is skipped or needs-tool-support\n"
    "  -h, --help          show this help\n"
    "\n"
    "Exit status: 0 normally (1 with --strict if any workload still needs\n"
    "attention), 2 on a usage error.\n",
    prog);
}

int main(int argc,char **argv){
  int csvflag = 0,quiet = 0,strict = 0;
  int opt,i;
  const char *run_index_paths[64];
  int nrun_index = 0;
  const char *workload_list_path;
  struct ledger_entry *entries;
  int nentries,counts[4] = {0,0,0,0};

  static struct option long_options[] = {
    { "run-index", required_argument, 0, 'r' },
    { "csv",       no_argument,       0, 'c' },
    { "quiet",     no_argument,       0, 'q' },
    { "strict",    no_argument,       0, 's' },
    { "help",      no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  while ((opt = getopt_long(argc,argv,"qsh",long_options,NULL)) != -1){
    switch (opt){
    case 'r':
      if (nrun_index >= (int)(sizeof(run_index_paths)/sizeof(run_index_paths[0]))){
        fprintf(stderr,"wspy-ledger: too many --run-index files\n");
        return 2;
      }
      run_index_paths[nrun_index++] = optarg;
      break;
    case 'c': csvflag = 1; break;
    case 'q': quiet = 1; break;
    case 's': strict = 1; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (optind >= argc){
    fprintf(stderr,"wspy-ledger: no workload list given\n\n");
    usage(argv[0]);
    return 2;
  }
  if (nrun_index == 0){
    fprintf(stderr,"wspy-ledger: at least one --run-index <file> is required\n\n");
    usage(argv[0]);
    return 2;
  }
  workload_list_path = argv[optind];

  entries = load_workload_list(workload_list_path,&nentries);
  if (!entries) return 2;

  for (i = 0; i < nrun_index; i++){
    if (process_run_index_file(run_index_paths[i],entries,nentries) < 0) return 2;
  }

  if (csvflag) printf("name,status,runs_matched,runs_succeeded,last_run_id,last_start_time,note\n");

  for (i = 0; i < nentries; i++){
    const struct ledger_entry *e = &entries[i];
    enum ledger_status status = entry_status(e);
    char detail[MAX_NOTE + 64];

    counts[status]++;
    if (quiet && status == LEDGER_DONE) continue;

    format_detail(e,status,detail,sizeof(detail));
    if (csvflag){
      print_csv_field(e->name); printf(",");
      printf("%s,%d,%d,",status_label(status),e->runs_matched,e->runs_succeeded);
      print_csv_field(e->last_run_id); printf(",");
      print_csv_field(e->last_start_time); printf(",");
      print_csv_field(detail);
      printf("\n");
    } else {
      printf("%-40s %-20s %s\n",e->name,status_label(status),detail);
    }
  }

  if (!csvflag)
    printf("%d workload(s): %d done, %d skipped, %d unsupported, %d needs-tool-support\n",
           nentries,counts[LEDGER_DONE],counts[LEDGER_SKIPPED],
           counts[LEDGER_UNSUPPORTED],counts[LEDGER_NEEDS_TOOL_SUPPORT]);

  free(entries);

  if (strict && (counts[LEDGER_SKIPPED] || counts[LEDGER_NEEDS_TOOL_SUPPORT])) return 1;
  return 0;
}
#endif /* TEST_LEDGER */
