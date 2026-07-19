/*
 * ledger.c - wspy-ledger: the "coverage ledger" idea from
 * INVESTIGATION.md ("Run artifact foundation" track): for a suite of
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
 *
 * --add <name> is a separate mode: rather than reporting coverage, it just
 * appends a workload name to a workload-list file (creating it if needed),
 * defaulting to workload/phoronix/backlog.txt -- a spot to jot down
 * candidate workloads as they come up, in the same file format this tool
 * already reads, without hand-editing the file or needing --run-index.
 *
 * --unavailable-deps <file> is the other kind of "known in advance" input,
 * alongside the workload list's own explicit unsupported/needs-tool-support
 * annotations, but for a whole *class* of workload rather than one at a
 * time: a file listing Phoronix Test Suite ExternalDependencies tags (one
 * per line, '#' comments like the workload list) known to be unavailable
 * on this host -- e.g. "steam" for a machine that can't install Steam.
 * When given, every unannotated workload is cross-checked against each
 * matching test-definition.xml's own <ExternalDependencies> list (scanned
 * from --phoronix-profiles-dir, default $HOME/.phoronix-test-suite/
 * test-profiles) and, on a match, treated as "unsupported" with a note
 * naming the tag and the profile it came from -- without needing to
 * discover or hand-annotate that workload individually. This only ever
 * *adds* an inferred "unsupported" where the workload would otherwise have
 * shown as skipped or needs-tool-support; a workload with at least one
 * successful matching run is left as "done" regardless (proof it actually
 * works trumps a dependency list saying it shouldn't), and an explicit
 * workload-list annotation still always wins over both.
 *
 * A matching run-index record whose own recorded output/tree/manifest files
 * have all been deleted (the common case: the whole run directory was
 * removed after a run failed for an environment reason -- tools not
 * installed, permissions, etc -- rather than a real workload problem) is
 * excluded from runs_matched/runs_succeeded rather than left to masquerade
 * as evidence of a real attempt; deleting a bad run's directory now
 * degrades that workload back toward "skipped" instead of permanently
 * pinning it at "needs-tool-support". It's still counted separately
 * (runs_stale) and, unless -q/--quiet is given, called out in the human
 * report's detail column and counted in --csv output, so the exclusion
 * stays auditable rather than silent. This is a read-time check against
 * the run index as it stands each run -- nothing here rewrites or prunes
 * the run-index file itself.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <dirent.h>
#include "json_reader.h"
#include "run_index.h"

#define DEFAULT_LIST_PATH "workload/phoronix/backlog.txt"
#define MAX_DEP_TAG 64
#define MAX_UNAVAILABLE_TAGS 256

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

  int runs_stale;                     /* matching runs excluded: their own output files are gone */
  char last_stale_run_id[MAX_RUN_ID];
  char last_stale_start_time[MAX_TIMESTAMP];

  int dep_unavailable;                /* 1 if a --unavailable-deps scan matched this workload */
  char dep_note[MAX_NOTE];            /* which tag/profile matched, valid only if dep_unavailable */
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
 * name, status keyword, free-text note. A '#' anywhere on the line starts
 * a comment that runs to end of line (whole-line or trailing inline);
 * it and everything after it are discarded before parsing. Returns 0 for
 * a blank or comment-only line, 1 otherwise (name/status/note point into
 * line, status and note are NULL if not present). */
static int parse_ledger_line(char *line,char **name,char **status,char **note){
  char *p = ltrim(line);
  char *tab;
  char *hash = strchr(p,'#');

  if (hash) *hash = '\0';
  if (*p == '\0') return 0;

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

/* Checks whether name already appears (exact match on the name field,
 * i.e. ignoring any status/note columns) in the workload list at path.
 * A missing file just means "not present yet", not an error. */
static int list_contains_name(const char *path,const char *name){
  long size;
  char *buf;
  char **lines;
  int nlines,i,found = 0;

  buf = read_whole_file(path,&size);
  if (!buf) return 0;
  lines = split_lines(buf,&nlines);
  for (i = 0; i < nlines && !found; i++){
    char *n,*status_str,*note;

    if (!parse_ledger_line(lines[i],&n,&status_str,&note)) continue;
    if (!strcmp(n,name)) found = 1;
  }
  free(lines);
  free(buf);
  return found;
}

/* Appends name as a new line to the workload list at path (creating the
 * file if it doesn't exist yet), unless it's already present. Returns a
 * process exit code (0 on success, including the already-present case;
 * 2 on a usage/IO error), matching main()'s other error paths. */
static int add_to_list(const char *path,const char *name){
  FILE *fp;

  if (!*name){
    fprintf(stderr,"wspy-ledger: --add requires a non-empty workload name\n");
    return 2;
  }
  if (strchr(name,'\t') || strchr(name,'\n') || strchr(name,'#')){
    fprintf(stderr,"wspy-ledger: workload name must not contain tabs, newlines, or '#'\n");
    return 2;
  }
  if (list_contains_name(path,name)){
    printf("wspy-ledger: '%s' is already in %s, not added\n",name,path);
    return 0;
  }
  fp = fopen(path,"a");
  if (!fp){
    fprintf(stderr,"wspy-ledger: unable to open %s for writing: %s\n",path,strerror(errno));
    return 2;
  }
  fprintf(fp,"%s\n",name);
  fclose(fp);
  printf("wspy-ledger: added '%s' to %s\n",name,path);
  return 0;
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

/* Parses a --unavailable-deps file: one Phoronix ExternalDependencies tag
 * per line ('#' comments and blank lines ignored, same convention as the
 * workload list). Returns the number of tags loaded, or -1 on a read
 * error; truncates silently past max_tags (there's no realistic list this
 * long, but this keeps the caller's buffer a fixed size like the rest of
 * this file's arrays). */
static int load_unavailable_deps(const char *path,char tags[][MAX_DEP_TAG],int max_tags){
  long size;
  char *buf;
  char **lines;
  int nlines,i,n = 0;

  buf = read_whole_file(path,&size);
  if (!buf){
    fprintf(stderr,"wspy-ledger: unable to read --unavailable-deps file: %s\n",path);
    return -1;
  }
  lines = split_lines(buf,&nlines);
  for (i = 0; i < nlines && n < max_tags; i++){
    char *p = ltrim(lines[i]);
    char *hash = strchr(p,'#');

    if (hash) *hash = '\0';
    rtrim(p);
    if (!*p) continue;
    snprintf(tags[n++],MAX_DEP_TAG,"%s",p);
  }
  free(lines);
  free(buf);
  return n;
}

static int dep_set_contains(char tags[][MAX_DEP_TAG],int ntags,const char *tag){
  int i;
  for (i = 0; i < ntags; i++) if (!strcasecmp(tags[i],tag)) return 1;
  return 0;
}

/* A Phoronix test profile directory is named "<test-id>-<version>"; this
 * strips a trailing version suffix (the last '-' whose remainder is only
 * digits/dots) to recover the bare test id a workload-list entry's name
 * is expected to match, e.g. "dirt-rally2-1.0.2" -> "dirt-rally2". A
 * directory name with no version-looking suffix is returned unchanged. */
static int looks_like_version(const char *s){
  int has_digit = 0;

  for (; *s; s++){
    if (*s == '.') continue;
    if (*s < '0' || *s > '9') return 0;
    has_digit = 1;
  }
  return has_digit;
}

static void strip_version_suffix(const char *dirname,char *out,size_t outsize){
  const char *last_dash = strrchr(dirname,'-');

  if (last_dash && looks_like_version(last_dash + 1)){
    size_t n = (size_t)(last_dash - dirname);
    if (n >= outsize) n = outsize - 1;
    memcpy(out,dirname,n);
    out[n] = '\0';
  } else {
    snprintf(out,outsize,"%s",dirname);
  }
}

/* Extracts the raw text between <ExternalDependencies>...</...> in a
 * test-definition.xml's contents. Deliberately not a real XML parser --
 * this is the one field this tool needs, and a full parser is more
 * dependency than that's worth (see json_reader.h's own comment on why
 * that reader exists only because wspy-validate actually needed one). */
static int extract_external_dependencies(const char *xml,char *out,size_t outsize){
  static const char *const open_tag = "<ExternalDependencies>";
  static const char *const close_tag = "</ExternalDependencies>";
  const char *start = strstr(xml,open_tag);
  const char *end;
  size_t len;

  if (!start) return 0;
  start += strlen(open_tag);
  end = strstr(start,close_tag);
  if (!end) return 0;
  len = (size_t)(end - start);
  if (len >= outsize) len = outsize - 1;
  memcpy(out,start,len);
  out[len] = '\0';
  return 1;
}

/* Scans every "<profiles_dir>/<suite>/<test-id>-<version>/test-definition.xml"
 * for ExternalDependencies tags in unavail_tags, marking any matching
 * (by bare test id, case-insensitively) and not-already-annotated entry as
 * dep_unavailable. A profile directory whose test-definition.xml can't be
 * read, or that has no ExternalDependencies field, is skipped, not an
 * error -- most profiles have neither a dependency this tool cares about
 * nor a matching entry in the workload list at all. Missing/unreadable
 * profiles_dir itself is reported but not fatal, since --unavailable-deps
 * is meant to enrich a report, not gate it. */
static void scan_phoronix_dependencies(const char *profiles_dir,char unavail_tags[][MAX_DEP_TAG],int nunavail,
                                        struct ledger_entry *entries,int nentries){
  DIR *suites_dir;
  struct dirent *suite_ent;

  suites_dir = opendir(profiles_dir);
  if (!suites_dir){
    fprintf(stderr,"wspy-ledger: --phoronix-profiles-dir: unable to open %s: %s\n",profiles_dir,strerror(errno));
    return;
  }
  while ((suite_ent = readdir(suites_dir)) != NULL){
    char suite_path[512];
    struct stat st;
    DIR *suite_dir;
    struct dirent *test_ent;

    if (suite_ent->d_name[0] == '.') continue;
    snprintf(suite_path,sizeof(suite_path),"%s/%s",profiles_dir,suite_ent->d_name);
    if (stat(suite_path,&st) != 0 || !S_ISDIR(st.st_mode)) continue;

    suite_dir = opendir(suite_path);
    if (!suite_dir) continue;
    while ((test_ent = readdir(suite_dir)) != NULL){
      char base_name[MAX_NAME];
      char xml_path[1024];
      char deps[1024];
      char *xml;
      long xml_size;
      char *tag;
      int i,any_entry_match = 0;

      if (test_ent->d_name[0] == '.') continue;
      strip_version_suffix(test_ent->d_name,base_name,sizeof(base_name));

      for (i = 0; i < nentries; i++){
        if (entries[i].annotated || entries[i].dep_unavailable) continue;
        if (!strcasecmp(entries[i].name,base_name)){ any_entry_match = 1; break; }
      }
      if (!any_entry_match) continue;

      snprintf(xml_path,sizeof(xml_path),"%s/%s/test-definition.xml",suite_path,test_ent->d_name);
      xml = read_whole_file(xml_path,&xml_size);
      if (!xml) continue;
      if (!extract_external_dependencies(xml,deps,sizeof(deps))){ free(xml); continue; }

      tag = strtok(deps,",");
      while (tag){
        char *trimmed = rtrim(ltrim(tag));

        if (dep_set_contains(unavail_tags,nunavail,trimmed)){
          for (i = 0; i < nentries; i++){
            if (entries[i].annotated || entries[i].dep_unavailable) continue;
            if (strcasecmp(entries[i].name,base_name)) continue;
            entries[i].dep_unavailable = 1;
            snprintf(entries[i].dep_note,sizeof(entries[i].dep_note),
                     "external dependency '%.40s' not available (%.40s/%.80s)",
                     trimmed,suite_ent->d_name,test_ent->d_name);
          }
        }
        tag = strtok(NULL,",");
      }
      free(xml);
    }
    closedir(suite_dir);
  }
  closedir(suites_dir);
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

/* Checks whether a run-index record's own output_files paths still exist
 * on disk. Returns 1 if at least one of output_path/tree_output_path/
 * manifest_path exists, 0 if the record names at least one path and none
 * of them exist (the run's artifacts were deleted -- most commonly the
 * whole run directory was removed), or -1 if the record names no paths at
 * all to check (an older schema, or a run that used neither --csv/-o,
 * --tree, nor --manifest output) -- treated as "can't tell", not stale,
 * so a record with nothing to check never gets wrongly excluded. */
static int record_paths_exist(const struct json_value *record){
  static const char *const fields[] = { "output_path","tree_output_path","manifest_path" };
  const struct json_value *of = json_object_get(record,"output_files");
  struct stat st;
  int any_path = 0,i;

  if (!of) return -1;
  for (i = 0; i < (int)(sizeof(fields)/sizeof(fields[0])); i++){
    const char *path = json_get_string(of,fields[i],NULL);
    if (!path || !*path) continue;
    any_path = 1;
    if (stat(path,&st) == 0) return 1;
  }
  return any_path ? 0 : -1;
}

static int record_succeeded(const struct json_value *record){
  const struct json_value *es = json_object_get(record,"exit_status");

  if (!es) return 0;
  if (!json_get_bool(es,"known",0)) return 0;
  if (json_get_bool(es,"signaled",0)) return 0;
  if (!json_get_bool(es,"exited",0)) return 0;
  return (int)json_get_number(es,"exit_code",-1) == 0;
}

/* Checks a run-index record's "schema_version" against RUN_INDEX_SCHEMA_VERSION
 * (INVESTIGATION.md's "run-index schema validation on ingest" item --
 * the write side has been versioned since run_index.c shipped, but nothing
 * read it back and noticed a mismatch until now). Only warns on a MAJOR
 * version difference, mirroring validate.c's check_schema_version() for
 * manifests: a MINOR/PATCH bump only adds fields, which this reader already
 * tolerates via json_reader.h's default-on-missing-key accessors. Each
 * distinct mismatched (or missing) version is warned about once per file,
 * not once per record, since a file's records typically share one version. */
#define MAX_WARNED_SCHEMA_VERSIONS 16
static void check_record_schema_version(const struct json_value *record,const char *path,
                                         char warned_versions[][16],int *nwarned_versions,int *warned_missing){
  const char *version = json_get_string(record,"schema_version",NULL);
  int j;

  if (!version){
    if (!*warned_missing){
      fprintf(stderr,"wspy-ledger: %s: run index records have no schema_version field "
                      "-- may predate run-index versioning or not be a wspy run index\n",path);
      *warned_missing = 1;
    }
    return;
  }
  if (atoi(version) == atoi(RUN_INDEX_SCHEMA_VERSION)) return;
  for (j = 0; j < *nwarned_versions; j++) if (!strcmp(warned_versions[j],version)) return;
  fprintf(stderr,"wspy-ledger: %s: run index records use schema version %s, which has a "
                  "different major version than this tool understands (%s)\n",
          path,version,RUN_INDEX_SCHEMA_VERSION);
  if (*nwarned_versions < MAX_WARNED_SCHEMA_VERSIONS) snprintf(warned_versions[(*nwarned_versions)++],16,"%s",version);
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
  char warned_versions[MAX_WARNED_SCHEMA_VERSIONS][16];
  int nwarned_versions = 0,warned_missing = 0;

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
    int paths_exist;

    if (!root){
      fprintf(stderr,"wspy-ledger: %s:%d: skipping malformed record: %s\n",path,i+1,errbuf);
      continue;
    }
    records_read++;
    check_record_schema_version(root,path,warned_versions,&nwarned_versions,&warned_missing);
    run_id = json_get_string(root,"run_id","?");
    start_time = json_get_string(root,"start_time","");
    paths_exist = record_paths_exist(root);

    for (j = 0; j < nentries; j++){
      struct ledger_entry *e = &entries[j];

      if (!command_matches(root,e->name)) continue;

      if (paths_exist == 0){
        /* This run's own output files are gone -- don't let a deleted run
         * keep counting as evidence of anything, but keep it visible. */
        e->runs_stale++;
        if (strcmp(start_time,e->last_stale_start_time) >= 0){
          snprintf(e->last_stale_run_id,sizeof(e->last_stale_run_id),"%s",run_id);
          snprintf(e->last_stale_start_time,sizeof(e->last_stale_start_time),"%s",start_time);
        }
        continue;
      }

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
  if (e->runs_succeeded > 0) return LEDGER_DONE;
  if (e->dep_unavailable) return LEDGER_UNSUPPORTED;
  if (e->runs_matched == 0) return LEDGER_SKIPPED;
  return LEDGER_NEEDS_TOOL_SUPPORT;
}

static void format_detail(const struct ledger_entry *e,enum ledger_status status,char *buf,size_t bufsize){
  if (e->annotated && (status == LEDGER_UNSUPPORTED || status == LEDGER_NEEDS_TOOL_SUPPORT)){
    snprintf(buf,bufsize,"%s",e->note[0] ? e->note : "marked in workload list");
    return;
  }
  if (!e->annotated && status == LEDGER_UNSUPPORTED && e->dep_unavailable){
    snprintf(buf,bufsize,"%s",e->dep_note);
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
    "Usage: %s [options] --run-index <file> [--run-index <file> ...] [<workload-list>]\n"
    "       %s --add <name> [--list <file>]\n"
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
    "free-text note. Blank lines are ignored, and '#' starts a comment that\n"
    "runs to end of line (whole-line or trailing after content).\n"
    "A workload's name is matched as a substring against each run-index\n"
    "record's command line. If omitted, defaults to %s\n"
    "(overridable with --list, same as --add below).\n"
    "\n"
    "A matching run whose own output/tree/manifest files have all been\n"
    "deleted (e.g. its whole run directory was removed after a bad,\n"
    "environment-caused run) does not count as an attempt -- it's excluded\n"
    "from scoring so the workload can fall back to \"skipped\" rather than\n"
    "stay pinned at \"needs-tool-support\" -- but it's still counted as a\n"
    "stale run and noted in the report/CSV so the exclusion stays visible;\n"
    "-q/--quiet also suppresses that note (not just done workloads).\n"
    "\n"
    "--add <name> instead appends <name> as a new line to the workload list\n"
    "(creating it if needed) rather than generating a report -- a spot to\n"
    "note candidate workloads as they come up. Does nothing (but still exits\n"
    "0) if <name> is already in the list.\n"
    "\n"
    "--unavailable-deps <file> lists Phoronix Test Suite ExternalDependencies\n"
    "tags (one per line, '#' comments allowed) known to be unavailable on\n"
    "this host, e.g. \"steam\". Every unannotated workload is then checked\n"
    "against its matching test-definition.xml(s) under\n"
    "--phoronix-profiles-dir (default $HOME/.phoronix-test-suite/test-profiles)\n"
    "and, on a match, reported as unsupported with the matched tag/profile\n"
    "noted -- without needing to discover or annotate that workload by hand.\n"
    "Does not override a workload that already has at least one successful\n"
    "matching run, or an explicit workload-list annotation.\n"
    "\n"
    "Options:\n"
    "  --run-index <file>  run-index (JSONL) file to scan; may be repeated\n"
    "  --csv               machine-readable CSV output instead of the human report\n"
    "  -q, --quiet         only print workloads that are not done, and omit\n"
    "                      the stale-run detail note from the ones printed\n"
    "  -s, --strict        exit non-zero if any workload is skipped or needs-tool-support\n"
    "  --add <name>        append <name> to the workload list instead of reporting\n"
    "  --list <file>       workload list to use/append to (default: %s)\n"
    "  --unavailable-deps <file>\n"
    "                      tags known unavailable on this host (see above)\n"
    "  --phoronix-profiles-dir <dir>\n"
    "                      Phoronix test-profiles dir to scan (default:\n"
    "                      $HOME/.phoronix-test-suite/test-profiles)\n"
    "  -h, --help          show this help\n"
    "\n"
    "Exit status: 0 normally (1 with --strict if any workload still needs\n"
    "attention), 2 on a usage error.\n",
    prog,prog,DEFAULT_LIST_PATH,DEFAULT_LIST_PATH);
}

int main(int argc,char **argv){
  int csvflag = 0,quiet = 0,strict = 0;
  int opt,i;
  const char *run_index_paths[64];
  int nrun_index = 0;
  const char *workload_list_path;
  const char *add_name = NULL;
  const char *list_path = DEFAULT_LIST_PATH;
  const char *unavailable_deps_path = NULL;
  const char *phoronix_profiles_dir = NULL;
  struct ledger_entry *entries;
  int nentries,counts[4] = {0,0,0,0};

  static struct option long_options[] = {
    { "run-index",             required_argument, 0, 'r' },
    { "csv",                   no_argument,       0, 'c' },
    { "quiet",                 no_argument,       0, 'q' },
    { "strict",                no_argument,       0, 's' },
    { "add",                   required_argument, 0, 'a' },
    { "list",                  required_argument, 0, 'l' },
    { "unavailable-deps",      required_argument, 0, 'u' },
    { "phoronix-profiles-dir", required_argument, 0, 'p' },
    { "help",                  no_argument,       0, 'h' },
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
    case 'a': add_name = optarg; break;
    case 'l': list_path = optarg; break;
    case 'u': unavailable_deps_path = optarg; break;
    case 'p': phoronix_profiles_dir = optarg; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (add_name) return add_to_list(list_path,add_name);
  if (nrun_index == 0){
    fprintf(stderr,"wspy-ledger: at least one --run-index <file> is required\n\n");
    usage(argv[0]);
    return 2;
  }
  workload_list_path = (optind < argc) ? argv[optind] : list_path;

  entries = load_workload_list(workload_list_path,&nentries);
  if (!entries) return 2;

  for (i = 0; i < nrun_index; i++){
    if (process_run_index_file(run_index_paths[i],entries,nentries) < 0) return 2;
  }

  if (!unavailable_deps_path && phoronix_profiles_dir){
    fprintf(stderr,"wspy-ledger: --phoronix-profiles-dir given without --unavailable-deps, ignoring\n");
  }
  if (unavailable_deps_path){
    char tags[MAX_UNAVAILABLE_TAGS][MAX_DEP_TAG];
    int ntags = load_unavailable_deps(unavailable_deps_path,tags,MAX_UNAVAILABLE_TAGS);
    const char *profiles_dir = phoronix_profiles_dir;
    char default_profiles_dir[512];

    if (ntags < 0) return 2;
    if (!profiles_dir){
      const char *home = getenv("HOME");
      if (home){
        snprintf(default_profiles_dir,sizeof(default_profiles_dir),"%s/.phoronix-test-suite/test-profiles",home);
        profiles_dir = default_profiles_dir;
      }
    }
    if (!profiles_dir){
      fprintf(stderr,"wspy-ledger: --unavailable-deps given but no --phoronix-profiles-dir and "
                      "$HOME is unset; skipping dependency scan\n");
    } else {
      scan_phoronix_dependencies(profiles_dir,tags,ntags,entries,nentries);
    }
  }

  if (csvflag) printf("name,status,runs_matched,runs_succeeded,runs_stale,last_run_id,last_start_time,note\n");

  for (i = 0; i < nentries; i++){
    const struct ledger_entry *e = &entries[i];
    enum ledger_status status = entry_status(e);
    char detail[MAX_NOTE + 160];

    counts[status]++;
    if (quiet && status == LEDGER_DONE) continue;

    format_detail(e,status,detail,sizeof(detail));
    if (!quiet && e->runs_stale > 0){
      size_t len = strlen(detail);
      snprintf(detail+len,sizeof(detail)-len," (%d stale run(s) excluded, output deleted, most recent %s)",
               e->runs_stale,e->last_stale_run_id);
    }
    if (csvflag){
      print_csv_field(e->name); printf(",");
      printf("%s,%d,%d,%d,",status_label(status),e->runs_matched,e->runs_succeeded,e->runs_stale);
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
