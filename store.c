/*
 * store.c - wspy-store: ingests --run-index (JSONL) files, and best-effort
 * enriches each record from the manifest.json/CSV output it points at,
 * into a normalized SQLite "run catalog" plus a per-run metric-value
 * table -- INVESTIGATION.md's "What shipped in 4.1", "Canonical metrics
 * schema + normalized store" (both halves: run metadata, and the CSV metric
 * values that make the run metadata actually useful for stats/comparison
 * work -- see doc/ARTIFACT_CONTRACT.md's "Normalized store" section).
 *
 * Like ledger.c/validate.c, this reads JSON via json_reader.h's
 * tolerant-default accessors and never aborts a whole file over one
 * malformed record. Unlike them, it's a persistent store rather than a
 * stateless report: re-running it against the same or a grown run-index
 * file must not duplicate rows (idempotent upsert keyed on
 * (hostname,run_id), which run_index.c's own comment documents as unique
 * only per host) and should not re-parse bytes it already ingested (the
 * ingest_sources table tracks a per-file byte offset).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include <sys/stat.h>
#include <time.h>
#include <sqlite3.h>
#include "json_reader.h"
#include "run_index.h"
#include "manifest.h"

#define STORE_SCHEMA_VERSION 2

static int now_iso8601(char *buf,size_t bufsize);

static const char *SCHEMA_DDL =
  "CREATE TABLE IF NOT EXISTS store_meta ("
  "  key TEXT PRIMARY KEY,"
  "  value TEXT NOT NULL"
  ");"
  "CREATE TABLE IF NOT EXISTS ingest_sources ("
  "  path TEXT PRIMARY KEY,"
  "  last_byte_offset INTEGER NOT NULL DEFAULT 0,"
  "  last_size INTEGER NOT NULL DEFAULT 0,"
  "  last_ingested_at TEXT"
  ");"
  "CREATE TABLE IF NOT EXISTS runs ("
  "  id INTEGER PRIMARY KEY,"
  "  run_id TEXT NOT NULL,"
  "  hostname TEXT NOT NULL,"
  "  run_index_schema_version TEXT NOT NULL,"
  "  collector TEXT NOT NULL,"
  "  wspy_version TEXT,"
  "  cpu_vendor TEXT,"
  "  cpu_family INTEGER,"
  "  cpu_model INTEGER,"
  "  start_time TEXT NOT NULL,"
  "  finish_time TEXT,"
  "  elapsed_seconds REAL,"
  "  command TEXT NOT NULL,"
  "  exit_known INTEGER,"
  "  exit_exited INTEGER,"
  "  exit_code INTEGER,"
  "  exit_signaled INTEGER,"
  "  exit_term_signal INTEGER,"
  "  per_core INTEGER,"
  "  system_flag INTEGER,"
  "  csv_flag INTEGER,"
  "  tree_flag INTEGER,"
  "  interval_seconds INTEGER,"
  "  counter_mask TEXT,"
  "  counter_mask_int INTEGER,"
  "  counters_requested INTEGER,"
  "  counters_measured INTEGER,"
  "  output_path TEXT,"
  "  tree_output_path TEXT,"
  "  manifest_path TEXT,"
  "  manifest_ingested INTEGER NOT NULL DEFAULT 0,"
  "  kernel_release TEXT,"
  "  num_cores INTEGER,"
  "  num_cores_available INTEGER,"
  "  is_hybrid INTEGER,"
  "  metrics_ingested INTEGER NOT NULL DEFAULT 0,"
  "  metrics_row_count INTEGER,"
  "  source_run_index_path TEXT NOT NULL,"
  "  ingested_at TEXT NOT NULL,"
  "  UNIQUE(hostname, run_id)"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_runs_start_time ON runs(start_time);"
  "CREATE TABLE IF NOT EXISTS run_command_args ("
  "  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,"
  "  arg_index INTEGER NOT NULL,"
  "  arg_value TEXT NOT NULL,"
  "  PRIMARY KEY (run_id, arg_index)"
  ");"
  "CREATE TABLE IF NOT EXISTS run_environment ("
  "  run_id INTEGER PRIMARY KEY REFERENCES runs(id) ON DELETE CASCADE,"
  "  virt_role TEXT,"
  "  hypervisor_vendor TEXT,"
  "  microcode_version TEXT,"
  "  bios_vendor TEXT,"
  "  bios_version TEXT,"
  "  bios_date TEXT,"
  "  cpu_governor TEXT,"
  "  cpu_scaling_driver TEXT,"
  "  cpu_governor_uniform INTEGER,"
  "  memory_total_kb INTEGER,"
  "  compiler_version TEXT,"
  "  libc_version TEXT,"
  "  captured_count INTEGER,"
  "  probed_count INTEGER"
  ");"
  "CREATE TABLE IF NOT EXISTS metric_values ("
  "  id INTEGER PRIMARY KEY,"
  "  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,"
  "  row_index INTEGER NOT NULL,"
  "  tick_time REAL,"
  "  core INTEGER,"
  "  phase TEXT,"
  "  metric_name TEXT NOT NULL,"
  "  value REAL,"
  "  is_percent INTEGER NOT NULL DEFAULT 0,"
  "  raw_text TEXT NOT NULL"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_metric_values_run_metric ON metric_values(run_id,metric_name);";

/* Applied when an existing database is found at STORE_SCHEMA_VERSION 1
 * (the shape wspy-store originally shipped with, before metric_values
 * existed) -- test_store.c covers this migration end to end (see
 * INVESTIGATION.md's "Shipped since 4.1", "Testing"), rather than
 * a second "no migration path yet" caveat. Safe to run exactly once
 * (ensure_schema() only reaches this at user_version==1); the ADD COLUMN
 * statements would error on a second run, which is why it's strictly
 * version-gated rather than using IF NOT EXISTS the way SCHEMA_DDL's
 * fresh-database path can. */
static const char *MIGRATION_V1_TO_V2 =
  "ALTER TABLE runs ADD COLUMN metrics_ingested INTEGER NOT NULL DEFAULT 0;"
  "ALTER TABLE runs ADD COLUMN metrics_row_count INTEGER;"
  "CREATE TABLE IF NOT EXISTS metric_values ("
  "  id INTEGER PRIMARY KEY,"
  "  run_id INTEGER NOT NULL REFERENCES runs(id) ON DELETE CASCADE,"
  "  row_index INTEGER NOT NULL,"
  "  tick_time REAL,"
  "  core INTEGER,"
  "  phase TEXT,"
  "  metric_name TEXT NOT NULL,"
  "  value REAL,"
  "  is_percent INTEGER NOT NULL DEFAULT 0,"
  "  raw_text TEXT NOT NULL"
  ");"
  "CREATE INDEX IF NOT EXISTS idx_metric_values_run_metric ON metric_values(run_id,metric_name);";

struct store_stats {
  int records_seen;
  int records_new;
  int records_updated;
  int records_malformed;
  int records_collision;
  int schema_mismatch_warned;
  int manifests_enriched;
  int manifests_skipped;
  int manifests_mismatched;
  int metrics_ingested;
  int metrics_skipped;
  int metrics_row_mismatches;
};

static char *read_whole_file_from(const char *path,long offset,long *size_out){
  FILE *fp;
  long size;
  char *buf;

  fp = fopen(path,"rb");
  if (!fp) return NULL;
  fseek(fp,0,SEEK_END);
  size = ftell(fp);
  if (size < 0 || offset > size){ fclose(fp); return NULL; }
  fseek(fp,offset,SEEK_SET);
  buf = malloc((size_t)(size - offset) + 1);
  if (buf == NULL){ fclose(fp); return NULL; }
  if (size > offset && fread(buf,1,(size_t)(size - offset),fp) != (size_t)(size - offset)){
    free(buf);
    fclose(fp);
    return NULL;
  }
  buf[size - offset] = '\0';
  fclose(fp);
  *size_out = size;
  return buf;
}

/* Splits buf into a NULL-terminated-count array of mutable line strings
 * (newlines stripped, blank lines dropped). A line without a trailing
 * newline (the file's writer was interrupted mid-append) is dropped, not
 * returned, since it may be incomplete JSON -- it will be picked up
 * (from the same byte offset) the next time this file is ingested, once
 * the writer finishes it. Sets *consumed_out to the number of bytes
 * that were part of complete, newline-terminated lines. */
static char **split_complete_lines(char *buf,int *count_out,long *consumed_out){
  char **lines = NULL;
  int cap = 0,n = 0;
  char *p = buf;
  long consumed = 0;

  while (*p){
    char *line_start = p;
    while (*p && *p != '\n') p++;
    if (*p != '\n') break; /* incomplete trailing line, stop here */
    *p = '\0';
    p++;
    consumed = p - buf;
    if (*line_start){
      if (n == cap){ cap = cap ? cap * 2 : 16; lines = realloc(lines,cap * sizeof(*lines)); }
      lines[n++] = line_start;
    }
  }
  *count_out = n;
  *consumed_out = consumed;
  return lines;
}

static int ensure_schema(sqlite3 *db){
  char *errmsg = NULL;
  int user_version = 0;
  sqlite3_stmt *stmt;

  if (sqlite3_exec(db,"PRAGMA user_version;",NULL,NULL,NULL) != SQLITE_OK) return -1;
  if (sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(stmt) == SQLITE_ROW) user_version = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }

  if (user_version > STORE_SCHEMA_VERSION){
    fprintf(stderr,"wspy-store: database schema version %d is newer than this build understands "
                    "(%d) -- refusing to write; use a newer wspy-store\n",
            user_version,STORE_SCHEMA_VERSION);
    return -1;
  }

  if (user_version == STORE_SCHEMA_VERSION) return 0; /* already current, nothing to do */

  if (user_version == 0){
    if (sqlite3_exec(db,SCHEMA_DDL,NULL,NULL,&errmsg) != SQLITE_OK){
      fprintf(stderr,"wspy-store: schema creation failed: %s\n",errmsg ? errmsg : "unknown error");
      sqlite3_free(errmsg);
      return -1;
    }
    {
      char now_buf[32];
      sqlite3_stmt *meta_stmt;
      now_iso8601(now_buf,sizeof(now_buf));
      if (sqlite3_prepare_v2(db,"INSERT OR REPLACE INTO store_meta(key,value) VALUES('created_at',?);",
                             -1,&meta_stmt,NULL) == SQLITE_OK){
        sqlite3_bind_text(meta_stmt,1,now_buf,-1,SQLITE_TRANSIENT);
        sqlite3_step(meta_stmt);
        sqlite3_finalize(meta_stmt);
      }
    }
  } else if (user_version == 1){
    fprintf(stderr,"wspy-store: migrating database from schema version 1 to %d "
                    "(adding metric_values)\n",STORE_SCHEMA_VERSION);
    if (sqlite3_exec(db,MIGRATION_V1_TO_V2,NULL,NULL,&errmsg) != SQLITE_OK){
      fprintf(stderr,"wspy-store: migration to schema version %d failed: %s\n",
              STORE_SCHEMA_VERSION,errmsg ? errmsg : "unknown error");
      sqlite3_free(errmsg);
      return -1;
    }
  }

  {
    char pragma[64];
    snprintf(pragma,sizeof(pragma),"PRAGMA user_version = %d;",STORE_SCHEMA_VERSION);
    if (sqlite3_exec(db,pragma,NULL,NULL,&errmsg) != SQLITE_OK){
      fprintf(stderr,"wspy-store: unable to set schema version: %s\n",errmsg ? errmsg : "unknown error");
      sqlite3_free(errmsg);
      return -1;
    }
  }
  return 0;
}

static sqlite3 *open_store(const char *path){
  sqlite3 *db = NULL;

  if (sqlite3_open(path,&db) != SQLITE_OK){
    fprintf(stderr,"wspy-store: unable to open database %s: %s\n",path,sqlite3_errmsg(db));
    if (db) sqlite3_close(db);
    return NULL;
  }
  sqlite3_busy_timeout(db,30000);
  sqlite3_exec(db,"PRAGMA journal_mode=WAL;",NULL,NULL,NULL);
  sqlite3_exec(db,"PRAGMA synchronous=NORMAL;",NULL,NULL,NULL);
  sqlite3_exec(db,"PRAGMA foreign_keys=ON;",NULL,NULL,NULL);

  if (ensure_schema(db) != 0){
    /* ensure_schema() already printed a specific reason (schema-version
     * refusal or DDL failure); nothing more to add here. */
    sqlite3_close(db);
    return NULL;
  }
  return db;
}

/* Checks a run-index record's "schema_version" against RUN_INDEX_SCHEMA_VERSION,
 * matching ledger.c's check_record_schema_version() -- only a MAJOR
 * version difference is worth mentioning, since a MINOR/PATCH bump only
 * adds fields, which json_reader.h's default-on-missing-key accessors
 * already tolerate. Returns 1 if this call newly warned (used to bump
 * stats->records_malformed-equivalent counting for --strict). */
static int check_schema_major_mismatch(const struct json_value *record,const char *path,
                                        int *already_warned){
  const char *version = json_get_string(record,"schema_version",NULL);

  if (!version) return 0;
  if (atoi(version) == atoi(RUN_INDEX_SCHEMA_VERSION)) return 0;
  if (!*already_warned){
    fprintf(stderr,"wspy-store: %s: run index records use schema version %s, which has a "
                    "different major version than this tool understands (%s)\n",
            path,version,RUN_INDEX_SCHEMA_VERSION);
    *already_warned = 1;
  }
  return 1;
}

static void bind_text_or_null(sqlite3_stmt *stmt,int idx,const char *val){
  if (val) sqlite3_bind_text(stmt,idx,val,-1,SQLITE_TRANSIENT);
  else sqlite3_bind_null(stmt,idx);
}

/* Inserts/updates the run_environment row for run_row_id from the
 * record's "environment"/"environment_coverage" objects (both are
 * present directly in every run-index record -- unlike host detail,
 * this doesn't depend on manifest enrichment). */
static void upsert_run_environment(sqlite3 *db,sqlite3_int64 run_row_id,const struct json_value *record){
  const struct json_value *env = json_object_get(record,"environment");
  const struct json_value *cov = json_object_get(record,"environment_coverage");
  sqlite3_stmt *stmt;
  static const char *sql =
    "INSERT INTO run_environment (run_id,virt_role,hypervisor_vendor,microcode_version,"
    "bios_vendor,bios_version,bios_date,cpu_governor,cpu_scaling_driver,cpu_governor_uniform,"
    "memory_total_kb,compiler_version,libc_version,captured_count,probed_count) "
    "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
    "ON CONFLICT(run_id) DO UPDATE SET "
    "virt_role=excluded.virt_role,hypervisor_vendor=excluded.hypervisor_vendor,"
    "microcode_version=excluded.microcode_version,bios_vendor=excluded.bios_vendor,"
    "bios_version=excluded.bios_version,bios_date=excluded.bios_date,"
    "cpu_governor=excluded.cpu_governor,cpu_scaling_driver=excluded.cpu_scaling_driver,"
    "cpu_governor_uniform=excluded.cpu_governor_uniform,memory_total_kb=excluded.memory_total_kb,"
    "compiler_version=excluded.compiler_version,libc_version=excluded.libc_version,"
    "captured_count=excluded.captured_count,probed_count=excluded.probed_count;";

  if (!env) return;
  if (sqlite3_prepare_v2(db,sql,-1,&stmt,NULL) != SQLITE_OK) return;
  sqlite3_bind_int64(stmt,1,run_row_id);
  bind_text_or_null(stmt,2,json_get_string(env,"virt_role",NULL));
  bind_text_or_null(stmt,3,json_get_string(env,"hypervisor_vendor",NULL));
  bind_text_or_null(stmt,4,json_get_string(env,"microcode_version",NULL));
  bind_text_or_null(stmt,5,json_get_string(env,"bios_vendor",NULL));
  bind_text_or_null(stmt,6,json_get_string(env,"bios_version",NULL));
  bind_text_or_null(stmt,7,json_get_string(env,"bios_date",NULL));
  bind_text_or_null(stmt,8,json_get_string(env,"cpu_governor",NULL));
  bind_text_or_null(stmt,9,json_get_string(env,"cpu_scaling_driver",NULL));
  sqlite3_bind_int(stmt,10,json_get_bool(env,"cpu_governor_uniform",0));
  sqlite3_bind_int64(stmt,11,(sqlite3_int64)json_get_number(env,"memory_total_kb",0));
  bind_text_or_null(stmt,12,json_get_string(env,"compiler_version",NULL));
  bind_text_or_null(stmt,13,json_get_string(env,"libc_version",NULL));
  sqlite3_bind_int(stmt,14,cov ? (int)json_get_number(cov,"captured",0) : 0);
  sqlite3_bind_int(stmt,15,cov ? (int)json_get_number(cov,"probed",0) : 0);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static void replace_run_command_args(sqlite3 *db,sqlite3_int64 run_row_id,const struct json_value *record){
  const struct json_value *cmd = json_object_get(record,"command");
  sqlite3_stmt *del,*ins;
  size_t i,n;

  if (sqlite3_prepare_v2(db,"DELETE FROM run_command_args WHERE run_id=?;",-1,&del,NULL) == SQLITE_OK){
    sqlite3_bind_int64(del,1,run_row_id);
    sqlite3_step(del);
    sqlite3_finalize(del);
  }
  if (!cmd || cmd->type != JSON_ARRAY) return;
  n = json_array_len(cmd);
  if (sqlite3_prepare_v2(db,"INSERT INTO run_command_args (run_id,arg_index,arg_value) VALUES (?,?,?);",
                         -1,&ins,NULL) != SQLITE_OK) return;
  for (i = 0; i < n; i++){
    const struct json_value *item = json_array_get(cmd,i);
    if (!item || item->type != JSON_STRING) continue;
    sqlite3_bind_int64(ins,1,run_row_id);
    sqlite3_bind_int(ins,2,(int)i);
    sqlite3_bind_text(ins,3,item->u.string,-1,SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_reset(ins);
  }
  sqlite3_finalize(ins);
}

/* Reads manifest_path (if non-NULL and the file exists), cross-checks its
 * command.argv[0]/timing.start_time against the run-index record's own
 * command[0]/start_time (a fixed/reused output filename could otherwise
 * point at a manifest from a different run), and on a match copies the
 * handful of host fields the index doesn't carry onto the runs row.
 * Never fatal -- a missing/unreadable/mismatched manifest just leaves
 * manifest_ingested at 0, matching this codebase's "measured vs
 * unavailable" graceful-degradation idiom used throughout coverage.c/
 * provenance.c. */
static void enrich_from_manifest(sqlite3 *db,sqlite3_int64 run_row_id,const char *manifest_path,
                                  const struct json_value *record,struct store_stats *stats){
  struct json_value *root;
  char errbuf[256];
  const struct json_value *cmd_obj,*argv,*timing,*host;
  const char *manifest_argv0,*manifest_start;
  const struct json_value *index_cmd = json_object_get(record,"command");
  const char *index_argv0 = NULL;
  const char *index_start = json_get_string(record,"start_time",NULL);
  sqlite3_stmt *stmt;

  if (!manifest_path){ stats->manifests_skipped++; return; }

  root = json_parse_file(manifest_path,errbuf,sizeof(errbuf));
  if (!root){ stats->manifests_skipped++; return; }

  cmd_obj = json_object_get(root,"command");
  argv = cmd_obj ? json_object_get(cmd_obj,"argv") : NULL;
  manifest_argv0 = (argv && argv->type == JSON_ARRAY && json_array_len(argv) > 0 &&
                    json_array_get(argv,0)->type == JSON_STRING) ? json_array_get(argv,0)->u.string : NULL;
  timing = json_object_get(root,"timing");
  manifest_start = timing ? json_get_string(timing,"start_time",NULL) : NULL;

  if (index_cmd && index_cmd->type == JSON_ARRAY && json_array_len(index_cmd) > 0 &&
      json_array_get(index_cmd,0)->type == JSON_STRING)
    index_argv0 = json_array_get(index_cmd,0)->u.string;

  if (!manifest_argv0 || !index_argv0 || strcmp(manifest_argv0,index_argv0) != 0 ||
      !manifest_start || !index_start || strcmp(manifest_start,index_start) != 0){
    fprintf(stderr,"wspy-store: %s: does not match the run-index record it was referenced from "
                    "(command/start_time mismatch) -- skipping enrichment\n",manifest_path);
    stats->manifests_mismatched++;
    json_free(root);
    return;
  }

  host = json_object_get(root,"host");
  if (sqlite3_prepare_v2(db,
        "UPDATE runs SET kernel_release=?,num_cores=?,num_cores_available=?,is_hybrid=?,"
        "manifest_ingested=1 WHERE id=?;",-1,&stmt,NULL) == SQLITE_OK){
    bind_text_or_null(stmt,1,host ? json_get_string(host,"kernel_release",NULL) : NULL);
    sqlite3_bind_int(stmt,2,host ? (int)json_get_number(host,"num_cores",0) : 0);
    sqlite3_bind_int(stmt,3,host ? (int)json_get_number(host,"num_cores_available",0) : 0);
    sqlite3_bind_int(stmt,4,host ? json_get_bool(host,"is_hybrid",0) : 0);
    sqlite3_bind_int64(stmt,5,run_row_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  stats->manifests_enriched++;
  json_free(root);
}

/* CSV cell parsing, mirrored verbatim from validate.c's split_csv_line()/
 * parse_numeric_field() rather than reimplemented -- same file format
 * (wspy's own CSV writer never quotes a field, always trailing-comma-
 * terminates a row), so the same rules apply: a cell is "numeric" iff
 * strtod() consumes the whole string, optionally followed by exactly one
 * trailing '%'. strtod() itself already accepts "nan"/"-nan"/"inf" (glibc)
 * -- topdown.c's print_ipc()/print_topdown_be()/etc. can legitimately
 * emit those on a zero divisor (confirmed empirically), so the caller
 * checks isnan()/isinf() and stores NULL for value while keeping raw_text
 * for audit, rather than rejecting the cell outright. */
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

static int parse_numeric_field(const char *s,double *out,int *is_percent){
  char *end;

  if (!s || !*s) return 0;
  *out = strtod(s,&end);
  if (end == s) return 0;
  if (*end == '\0'){ *is_percent = 0; return 1; }
  if (*end == '%' && end[1] == '\0'){ *is_percent = 1; return 1; }
  return 0;
}

/* Parses one wspy CSV output file into metric_values rows. Column
 * *identity* always comes from the actual header row, never inferred
 * from run-index flags (a column named exactly "time"/"core"/"phase" is
 * a dimension, every other non-empty-named column is a metric) -- this
 * is what lets one code path cover the aggregate, --interval, --per-core,
 * and --interval+--per-core (well-formed rows) shapes without special-
 * casing any of them.
 *
 * Per-row, not per-file, column-count checking: a data row whose field
 * count doesn't match the header's is skipped (counted, not fatal) and
 * parsing continues with the next row. This recovers strictly more real
 * data than stopping at the first mismatch would, against two confirmed
 * wspy CSV bugs neither of which this function's job is to fix: every
 * --interval --gpu-smi periodic tick is short by 4 columns (only the
 * tail row is correctly shaped -- skipping bad rows still lets the good
 * tail row through) and --per-core --interval appends a trailing,
 * unheaded, differently-shaped block after the well-formed interval rows
 * (each of those lines individually mismatches the header and is
 * skipped, same as if it were random garbage). */
static void ingest_csv_metrics(sqlite3 *db,sqlite3_int64 run_row_id,const char *csv_path,
                                struct store_stats *stats){
  long size;
  char *buf;
  char **lines;
  int nlines,i,j;
  long consumed;
  char *header_fields[MAX_CSV_FIELDS];
  int header_n;
  int time_col = -1,core_col = -1,phase_col = -1;
  int rows_ingested = 0;
  sqlite3_stmt *del,*ins;

  buf = read_whole_file_from(csv_path,0,&size);
  if (!buf || size == 0){
    free(buf);
    stats->metrics_skipped++;
    return;
  }
  lines = split_complete_lines(buf,&nlines,&consumed);
  if (nlines < 1){
    free(lines);
    free(buf);
    stats->metrics_skipped++;
    return;
  }

  header_n = split_csv_line(lines[0],header_fields,MAX_CSV_FIELDS);
  for (j = 0; j < header_n; j++){
    if (!strcmp(header_fields[j],"time")) time_col = j;
    else if (!strcmp(header_fields[j],"core")) core_col = j;
    else if (!strcmp(header_fields[j],"phase")) phase_col = j;
  }

  if (sqlite3_prepare_v2(db,"DELETE FROM metric_values WHERE run_id=?;",-1,&del,NULL) == SQLITE_OK){
    sqlite3_bind_int64(del,1,run_row_id);
    sqlite3_step(del);
    sqlite3_finalize(del);
  }

  if (sqlite3_prepare_v2(db,
        "INSERT INTO metric_values (run_id,row_index,tick_time,core,phase,metric_name,value,"
        "is_percent,raw_text) VALUES (?,?,?,?,?,?,?,?,?);",-1,&ins,NULL) != SQLITE_OK){
    free(lines);
    free(buf);
    stats->metrics_skipped++;
    return;
  }

  for (i = 1; i < nlines; i++){
    char *row_fields[MAX_CSV_FIELDS];
    int row_n = split_csv_line(lines[i],row_fields,MAX_CSV_FIELDS);
    double tick_time_val,core_val,num;
    int have_tick = 0,have_core = 0,is_pct;
    const char *phase_val = NULL;

    if (row_n != header_n){
      stats->metrics_row_mismatches++;
      continue;
    }

    if (time_col >= 0) have_tick = parse_numeric_field(row_fields[time_col],&tick_time_val,&is_pct);
    if (core_col >= 0) have_core = parse_numeric_field(row_fields[core_col],&core_val,&is_pct);
    if (phase_col >= 0 && *row_fields[phase_col]) phase_val = row_fields[phase_col];

    for (j = 0; j < header_n; j++){
      if (j == time_col || j == core_col || j == phase_col) continue;
      if (!*header_fields[j]) continue; /* trailing empty header cell from wspy's own trailing comma */
      if (!parse_numeric_field(row_fields[j],&num,&is_pct)) continue; /* non-numeric cell, skip */

      sqlite3_bind_int64(ins,1,run_row_id);
      sqlite3_bind_int(ins,2,i - 1);
      if (have_tick) sqlite3_bind_double(ins,3,tick_time_val); else sqlite3_bind_null(ins,3);
      if (have_core) sqlite3_bind_int(ins,4,(int)core_val); else sqlite3_bind_null(ins,4);
      bind_text_or_null(ins,5,phase_val);
      sqlite3_bind_text(ins,6,header_fields[j],-1,SQLITE_TRANSIENT);
      if (isnan(num) || isinf(num)) sqlite3_bind_null(ins,7);
      else sqlite3_bind_double(ins,7,num);
      sqlite3_bind_int(ins,8,is_pct);
      sqlite3_bind_text(ins,9,row_fields[j],-1,SQLITE_TRANSIENT);
      sqlite3_step(ins);
      sqlite3_reset(ins);
    }
    rows_ingested++;
  }
  sqlite3_finalize(ins);

  if (sqlite3_prepare_v2(db,"UPDATE runs SET metrics_ingested=1,metrics_row_count=? WHERE id=?;",
                         -1,&ins,NULL) == SQLITE_OK){
    sqlite3_bind_int(ins,1,rows_ingested);
    sqlite3_bind_int64(ins,2,run_row_id);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
  }
  stats->metrics_ingested++;

  free(lines);
  free(buf);
}

static int now_iso8601(char *buf,size_t bufsize){
  time_t t = time(NULL);
  struct tm tm_utc;
  gmtime_r(&t,&tm_utc);
  return strftime(buf,bufsize,"%Y-%m-%dT%H:%M:%SZ",&tm_utc) > 0 ? 0 : -1;
}

/* Inserts or updates one runs row from a parsed run-index record. On a
 * (hostname,run_id) conflict, checks whether the existing row's
 * start_time/command still match before overwriting -- a mismatch means
 * two different runs collided on that key (e.g. containers sharing a
 * host's UTS namespace with low-numbered, easily-duplicated PIDs), not a
 * genuine re-ingest of the same run, and the existing row is left alone
 * rather than silently merged with unrelated data. */
static void upsert_run(sqlite3 *db,const struct json_value *record,const char *source_path,
                        int enrich_manifest,int ingest_metrics,struct store_stats *stats){
  const char *hostname = json_get_string(record,"hostname",NULL);
  const char *run_id = json_get_string(record,"run_id",NULL);
  const struct json_value *cmd = json_object_get(record,"command");
  const struct json_value *exit_status = json_object_get(record,"exit_status");
  const struct json_value *options = json_object_get(record,"options");
  const struct json_value *coverage = json_object_get(record,"counter_coverage");
  const struct json_value *outfiles = json_object_get(record,"output_files");
  const char *start_time = json_get_string(record,"start_time",NULL);
  const char *command0 = NULL;
  const char *counter_mask_str;
  unsigned long counter_mask_int = 0;
  sqlite3_stmt *stmt;
  sqlite3_int64 run_row_id;
  char now_buf[32];
  int existed = 0;

  if (!hostname || !run_id || !start_time){
    stats->records_malformed++;
    return;
  }
  if (cmd && cmd->type == JSON_ARRAY && json_array_len(cmd) > 0 &&
      json_array_get(cmd,0)->type == JSON_STRING)
    command0 = json_array_get(cmd,0)->u.string;

  /* Collision check: does a row for this (hostname,run_id) already exist
   * with a materially different start_time/command? */
  if (sqlite3_prepare_v2(db,"SELECT id,start_time,command FROM runs WHERE hostname=? AND run_id=?;",
                         -1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_text(stmt,1,hostname,-1,SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt,2,run_id,-1,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW){
      const unsigned char *existing_start = sqlite3_column_text(stmt,1);
      const unsigned char *existing_cmd = sqlite3_column_text(stmt,2);
      existed = 1;
      if ((existing_start && strcmp((const char *)existing_start,start_time) != 0) ||
          (command0 && existing_cmd && strcmp((const char *)existing_cmd,command0) != 0)){
        fprintf(stderr,"wspy-store: %s: (hostname=%s,run_id=%s) collides with a different "
                        "existing run -- leaving the stored row unchanged\n",
                source_path,hostname,run_id);
        stats->records_collision++;
        sqlite3_finalize(stmt);
        return;
      }
    }
    sqlite3_finalize(stmt);
  }

  counter_mask_str = options ? json_get_string(options,"counter_mask",NULL) : NULL;
  if (counter_mask_str) counter_mask_int = strtoul(counter_mask_str,NULL,16);
  now_iso8601(now_buf,sizeof(now_buf));

  if (sqlite3_prepare_v2(db,
      "INSERT INTO runs (run_id,hostname,run_index_schema_version,collector,wspy_version,"
      "cpu_vendor,cpu_family,cpu_model,start_time,finish_time,elapsed_seconds,command,"
      "exit_known,exit_exited,exit_code,exit_signaled,exit_term_signal,"
      "per_core,system_flag,csv_flag,tree_flag,interval_seconds,"
      "counter_mask,counter_mask_int,counters_requested,counters_measured,"
      "output_path,tree_output_path,manifest_path,source_run_index_path,ingested_at) "
      "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(hostname,run_id) DO UPDATE SET "
      "run_index_schema_version=excluded.run_index_schema_version,collector=excluded.collector,"
      "wspy_version=excluded.wspy_version,cpu_vendor=excluded.cpu_vendor,"
      "cpu_family=excluded.cpu_family,cpu_model=excluded.cpu_model,"
      "finish_time=excluded.finish_time,elapsed_seconds=excluded.elapsed_seconds,"
      "exit_known=excluded.exit_known,exit_exited=excluded.exit_exited,"
      "exit_code=excluded.exit_code,exit_signaled=excluded.exit_signaled,"
      "exit_term_signal=excluded.exit_term_signal,per_core=excluded.per_core,"
      "system_flag=excluded.system_flag,csv_flag=excluded.csv_flag,tree_flag=excluded.tree_flag,"
      "interval_seconds=excluded.interval_seconds,counter_mask=excluded.counter_mask,"
      "counter_mask_int=excluded.counter_mask_int,counters_requested=excluded.counters_requested,"
      "counters_measured=excluded.counters_measured,output_path=excluded.output_path,"
      "tree_output_path=excluded.tree_output_path,manifest_path=excluded.manifest_path,"
      "source_run_index_path=excluded.source_run_index_path,ingested_at=excluded.ingested_at;",
      -1,&stmt,NULL) != SQLITE_OK){
    stats->records_malformed++;
    return;
  }
  sqlite3_bind_text(stmt,1,run_id,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,2,hostname,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,3,json_get_string(record,"schema_version",""),-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,4,json_get_string(record,"collector","wspy"),-1,SQLITE_TRANSIENT);
  bind_text_or_null(stmt,5,json_get_string(record,"wspy_version",NULL));
  bind_text_or_null(stmt,6,json_get_string(record,"cpu_vendor",NULL));
  sqlite3_bind_int(stmt,7,(int)json_get_number(record,"cpu_family",0));
  sqlite3_bind_int(stmt,8,(int)json_get_number(record,"cpu_model",0));
  sqlite3_bind_text(stmt,9,start_time,-1,SQLITE_TRANSIENT);
  bind_text_or_null(stmt,10,json_get_string(record,"finish_time",NULL));
  sqlite3_bind_double(stmt,11,json_get_number(record,"elapsed_seconds",0));
  sqlite3_bind_text(stmt,12,command0 ? command0 : "",-1,SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt,13,exit_status ? json_get_bool(exit_status,"known",0) : 0);
  sqlite3_bind_int(stmt,14,exit_status ? json_get_bool(exit_status,"exited",0) : 0);
  sqlite3_bind_int(stmt,15,exit_status ? (int)json_get_number(exit_status,"exit_code",0) : 0);
  sqlite3_bind_int(stmt,16,exit_status ? json_get_bool(exit_status,"signaled",0) : 0);
  sqlite3_bind_int(stmt,17,exit_status ? (int)json_get_number(exit_status,"term_signal",0) : 0);
  sqlite3_bind_int(stmt,18,options ? json_get_bool(options,"per_core",0) : 0);
  sqlite3_bind_int(stmt,19,options ? json_get_bool(options,"system",0) : 0);
  sqlite3_bind_int(stmt,20,options ? json_get_bool(options,"csv",0) : 0);
  sqlite3_bind_int(stmt,21,options ? json_get_bool(options,"tree",0) : 0);
  sqlite3_bind_int(stmt,22,options ? (int)json_get_number(options,"interval_seconds",0) : 0);
  bind_text_or_null(stmt,23,counter_mask_str);
  sqlite3_bind_int64(stmt,24,(sqlite3_int64)counter_mask_int);
  sqlite3_bind_int(stmt,25,coverage ? (int)json_get_number(coverage,"requested",0) : 0);
  sqlite3_bind_int(stmt,26,coverage ? (int)json_get_number(coverage,"measured",0) : 0);
  bind_text_or_null(stmt,27,outfiles ? json_get_string(outfiles,"output_path",NULL) : NULL);
  bind_text_or_null(stmt,28,outfiles ? json_get_string(outfiles,"tree_output_path",NULL) : NULL);
  bind_text_or_null(stmt,29,outfiles ? json_get_string(outfiles,"manifest_path",NULL) : NULL);
  sqlite3_bind_text(stmt,30,source_path,-1,SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt,31,now_buf,-1,SQLITE_TRANSIENT);

  if (sqlite3_step(stmt) != SQLITE_DONE){
    sqlite3_finalize(stmt);
    stats->records_malformed++;
    return;
  }
  sqlite3_finalize(stmt);

  run_row_id = existed ? 0 : sqlite3_last_insert_rowid(db);
  if (existed){
    sqlite3_stmt *sel;
    if (sqlite3_prepare_v2(db,"SELECT id FROM runs WHERE hostname=? AND run_id=?;",-1,&sel,NULL) == SQLITE_OK){
      sqlite3_bind_text(sel,1,hostname,-1,SQLITE_TRANSIENT);
      sqlite3_bind_text(sel,2,run_id,-1,SQLITE_TRANSIENT);
      if (sqlite3_step(sel) == SQLITE_ROW) run_row_id = sqlite3_column_int64(sel,0);
      sqlite3_finalize(sel);
    }
    stats->records_updated++;
  } else {
    stats->records_new++;
  }

  replace_run_command_args(db,run_row_id,record);
  upsert_run_environment(db,run_row_id,record);

  if (enrich_manifest){
    const char *manifest_path = outfiles ? json_get_string(outfiles,"manifest_path",NULL) : NULL;
    struct stat st;
    if (manifest_path && stat(manifest_path,&st) == 0)
      enrich_from_manifest(db,run_row_id,manifest_path,record,stats);
    else
      stats->manifests_skipped++;
  } else {
    stats->manifests_skipped++;
  }

  if (ingest_metrics){
    int csv_flag = options ? json_get_bool(options,"csv",0) : 0;
    const char *output_path = outfiles ? json_get_string(outfiles,"output_path",NULL) : NULL;
    struct stat st;
    if (csv_flag && output_path && stat(output_path,&st) == 0 && st.st_size > 0)
      ingest_csv_metrics(db,run_row_id,output_path,stats);
    else
      stats->metrics_skipped++;
  } else {
    stats->metrics_skipped++;
  }
}

static long get_source_offset(sqlite3 *db,const char *path){
  sqlite3_stmt *stmt;
  long offset = 0;
  long last_size = 0;
  struct stat st;

  if (sqlite3_prepare_v2(db,"SELECT last_byte_offset,last_size FROM ingest_sources WHERE path=?;",
                         -1,&stmt,NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(stmt,1,path,-1,SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW){
    offset = (long)sqlite3_column_int64(stmt,0);
    last_size = (long)sqlite3_column_int64(stmt,1);
  }
  sqlite3_finalize(stmt);

  if (stat(path,&st) == 0 && (long)st.st_size < last_size){
    fprintf(stderr,"wspy-store: %s: file shrank since last ingest (rotated/truncated?) -- rescanning from the start\n",path);
    return 0;
  }
  return offset;
}

static void set_source_offset(sqlite3 *db,const char *path,long offset,long size){
  sqlite3_stmt *stmt;
  char now_buf[32];

  now_iso8601(now_buf,sizeof(now_buf));
  if (sqlite3_prepare_v2(db,
      "INSERT INTO ingest_sources (path,last_byte_offset,last_size,last_ingested_at) VALUES (?,?,?,?) "
      "ON CONFLICT(path) DO UPDATE SET last_byte_offset=excluded.last_byte_offset,"
      "last_size=excluded.last_size,last_ingested_at=excluded.last_ingested_at;",
      -1,&stmt,NULL) != SQLITE_OK) return;
  sqlite3_bind_text(stmt,1,path,-1,SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt,2,offset);
  sqlite3_bind_int64(stmt,3,size);
  sqlite3_bind_text(stmt,4,now_buf,-1,SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

/* Ingests one run-index (JSONL) file, resuming from the byte offset
 * recorded for it in ingest_sources (if any) so repeated invocations
 * against a growing file only do work proportional to what's new.
 * Returns 0 on success, -1 if the file could not be read at all. */
static int ingest_run_index_file(sqlite3 *db,const char *path,int enrich_manifest,int ingest_metrics,
                                  struct store_stats *stats){
  long start_offset,file_size,consumed;
  char *buf;
  char **lines;
  int nlines,i,schema_warned = 0;
  const int BATCH = 5000;

  start_offset = get_source_offset(db,path);
  buf = read_whole_file_from(path,start_offset,&file_size);
  if (!buf){
    fprintf(stderr,"wspy-store: unable to read run index file: %s\n",path);
    return -1;
  }
  lines = split_complete_lines(buf,&nlines,&consumed);

  sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL);
  for (i = 0; i < nlines; i++){
    char errbuf[256];
    struct json_value *root;

    if (i > 0 && i % BATCH == 0){
      sqlite3_exec(db,"COMMIT;",NULL,NULL,NULL);
      sqlite3_exec(db,"BEGIN IMMEDIATE;",NULL,NULL,NULL);
    }

    root = json_parse(lines[i],errbuf,sizeof(errbuf));
    if (!root){
      fprintf(stderr,"wspy-store: %s: skipping malformed record: %s\n",path,errbuf);
      stats->records_malformed++;
      continue;
    }
    stats->records_seen++;
    if (check_schema_major_mismatch(root,path,&schema_warned)) stats->schema_mismatch_warned = 1;
    upsert_run(db,root,path,enrich_manifest,ingest_metrics,stats);
    json_free(root);
  }
  sqlite3_exec(db,"COMMIT;",NULL,NULL,NULL);

  set_source_offset(db,path,start_offset + consumed,file_size);

  free(lines);
  free(buf);
  return 0;
}

#ifndef TEST_STORE
static void usage(const char *prog){
  fprintf(stderr,
    "Usage: %s --db <path> --run-index <file> [--run-index <file> ...] [options]\n"
    "\n"
    "Ingests one or more --run-index (JSONL) files into a normalized SQLite\n"
    "\"run catalog\" plus a per-run metric_values table parsed from each\n"
    "run's CSV output -- INVESTIGATION.md's 4.1 \"canonical metrics\n"
    "schema + normalized store\" item.\n"
    "\n"
    "Records are upserted keyed on (hostname,run_id); re-running against the\n"
    "same or a grown run-index file is safe and will not duplicate rows.\n"
    "Each record's output_files.manifest_path (if present and readable) is\n"
    "also read for a handful of host fields the run index itself doesn't\n"
    "carry, and output_files.output_path's CSV (if options.csv was true and\n"
    "the file is present) is parsed into metric_values -- both best-effort,\n"
    "since these paths often won't resolve when aggregating run-index files\n"
    "copied in from other hosts.\n"
    "\n"
    "Options:\n"
    "  --db <path>          SQLite database to create/update (required)\n"
    "  --run-index <file>   run-index (JSONL) file to ingest; may be repeated\n"
    "  --no-manifest-enrich skip the best-effort per-record manifest.json read\n"
    "  --no-metrics-ingest  skip parsing each record's CSV output into metric_values\n"
    "  -q, --quiet          only print the final summary line\n"
    "  -s, --strict         exit non-zero if any record was malformed, had a\n"
    "                       schema major-version mismatch, or collided with\n"
    "                       an existing (hostname,run_id) run\n"
    "  -h, --help            show this help\n"
    "\n"
    "Exit status: 0 normally (1 with --strict if any record needed attention),\n"
    "2 on a usage error or if the database could not be opened/prepared.\n",
    prog);
}

int main(int argc,char **argv){
  int opt;
  const char *db_path = NULL;
  const char *run_index_paths[64];
  int nrun_index = 0;
  int quiet = 0,strict = 0,enrich_manifest = 1,ingest_metrics = 1;
  sqlite3 *db;
  struct store_stats stats;
  int i,rc = 0;
  sqlite3_stmt *count_stmt;

  static struct option long_options[] = {
    { "db",                 required_argument, 0, 'd' },
    { "run-index",          required_argument, 0, 'r' },
    { "no-manifest-enrich", no_argument,       0, 'M' },
    { "no-metrics-ingest",  no_argument,       0, 'C' },
    { "quiet",              no_argument,       0, 'q' },
    { "strict",             no_argument,       0, 's' },
    { "help",               no_argument,       0, 'h' },
    { 0,0,0,0 }
  };

  memset(&stats,0,sizeof(stats));

  while ((opt = getopt_long(argc,argv,"qsh",long_options,NULL)) != -1){
    switch (opt){
    case 'd': db_path = optarg; break;
    case 'r':
      if (nrun_index >= (int)(sizeof(run_index_paths)/sizeof(run_index_paths[0]))){
        fprintf(stderr,"wspy-store: too many --run-index files\n");
        return 2;
      }
      run_index_paths[nrun_index++] = optarg;
      break;
    case 'M': enrich_manifest = 0; break;
    case 'C': ingest_metrics = 0; break;
    case 'q': quiet = 1; break;
    case 's': strict = 1; break;
    case 'h': usage(argv[0]); return 0;
    default: usage(argv[0]); return 2;
    }
  }
  if (!db_path){
    fprintf(stderr,"wspy-store: --db <path> is required\n\n");
    usage(argv[0]);
    return 2;
  }
  if (nrun_index == 0){
    fprintf(stderr,"wspy-store: at least one --run-index <file> is required\n\n");
    usage(argv[0]);
    return 2;
  }

  db = open_store(db_path);
  if (!db) return 2;

  for (i = 0; i < nrun_index; i++){
    if (ingest_run_index_file(db,run_index_paths[i],enrich_manifest,ingest_metrics,&stats) < 0) rc = 2;
    if (!quiet)
      printf("%s: %d record(s): %d new, %d updated, %d malformed, %d collision(s); "
             "%d manifest(s) enriched, %d skipped, %d mismatched; "
             "%d metric-set(s) ingested, %d skipped, %d row(s) mismatched\n",
             run_index_paths[i],stats.records_seen,stats.records_new,stats.records_updated,
             stats.records_malformed,stats.records_collision,
             stats.manifests_enriched,stats.manifests_skipped,stats.manifests_mismatched,
             stats.metrics_ingested,stats.metrics_skipped,stats.metrics_row_mismatches);
  }

  if (sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM runs;",-1,&count_stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(count_stmt) == SQLITE_ROW)
      printf("%s: %d total run(s) in store\n",db_path,sqlite3_column_int(count_stmt,0));
    sqlite3_finalize(count_stmt);
  }

  sqlite3_close(db);

  if (rc) return rc;
  if (strict && (stats.records_malformed || stats.records_collision || stats.schema_mismatch_warned)) return 1;
  return 0;
}
#endif /* TEST_STORE */
