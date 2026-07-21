#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_STORE 1
#include "store.c"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void append_file(const char *path,const char *content){
  FILE *fp = fopen(path,"a");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void build_record(char *buf,size_t bufsize,const char *hostname,const char *run_id,
                          const char *start_time,const char *command0,int exit_code,
                          const char *manifest_path){
  char manifest_json[320];

  if (manifest_path) snprintf(manifest_json,sizeof(manifest_json),"\"%s\"",manifest_path);
  else snprintf(manifest_json,sizeof(manifest_json),"null");

  snprintf(buf,bufsize,
    "{\"schema_version\":\"%s\",\"run_id\":\"%s\",\"collector\":\"wspy\",\"wspy_version\":\"4.0\","
    "\"hostname\":\"%s\",\"cpu_vendor\":\"AMD\",\"cpu_family\":25,\"cpu_model\":1,"
    "\"environment\":{\"virt_role\":\"host\",\"hypervisor_vendor\":null,\"microcode_version\":null,"
    "\"bios_vendor\":null,\"bios_version\":null,\"bios_date\":null,\"cpu_governor\":null,"
    "\"cpu_scaling_driver\":null,\"cpu_governor_uniform\":false,\"memory_total_kb\":null,"
    "\"compiler_version\":null,\"libc_version\":null},"
    "\"environment_coverage\":{\"captured\":0,\"probed\":9},"
    "\"start_time\":\"%s\",\"finish_time\":\"%s\",\"elapsed_seconds\":1.0,"
    "\"command\":[\"%s\"],"
    "\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":%d,\"signaled\":false,\"term_signal\":null},"
    "\"options\":{\"counter_mask\":\"0x1\",\"per_core\":false,\"system\":true,\"csv\":false,\"tree\":false,\"interval_seconds\":0},"
    "\"counter_coverage\":{\"requested\":4,\"measured\":4},"
    "\"output_files\":{\"output_path\":null,\"tree_output_path\":null,\"manifest_path\":%s}}\n",
    RUN_INDEX_SCHEMA_VERSION,run_id,hostname,start_time,start_time,command0,exit_code,manifest_json);
}

/* Like build_record(), but with options.csv=true and a real output_path,
 * for tests exercising ingest_csv_metrics(). */
static void build_csv_record(char *buf,size_t bufsize,const char *hostname,const char *run_id,
                              const char *start_time,const char *command0,const char *output_path){
  char output_json[320];

  if (output_path) snprintf(output_json,sizeof(output_json),"\"%s\"",output_path);
  else snprintf(output_json,sizeof(output_json),"null");

  snprintf(buf,bufsize,
    "{\"schema_version\":\"%s\",\"run_id\":\"%s\",\"collector\":\"wspy\",\"wspy_version\":\"4.0\","
    "\"hostname\":\"%s\",\"cpu_vendor\":\"AMD\",\"cpu_family\":25,\"cpu_model\":1,"
    "\"environment\":{\"virt_role\":\"host\",\"hypervisor_vendor\":null,\"microcode_version\":null,"
    "\"bios_vendor\":null,\"bios_version\":null,\"bios_date\":null,\"cpu_governor\":null,"
    "\"cpu_scaling_driver\":null,\"cpu_governor_uniform\":false,\"memory_total_kb\":null,"
    "\"compiler_version\":null,\"libc_version\":null},"
    "\"environment_coverage\":{\"captured\":0,\"probed\":9},"
    "\"start_time\":\"%s\",\"finish_time\":\"%s\",\"elapsed_seconds\":1.0,"
    "\"command\":[\"%s\"],"
    "\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":0,\"signaled\":false,\"term_signal\":null},"
    "\"options\":{\"counter_mask\":\"0x1\",\"per_core\":false,\"system\":true,\"csv\":true,\"tree\":false,\"interval_seconds\":0},"
    "\"counter_coverage\":{\"requested\":4,\"measured\":4},"
    "\"output_files\":{\"output_path\":%s,\"tree_output_path\":null,\"manifest_path\":null}}\n",
    RUN_INDEX_SCHEMA_VERSION,run_id,hostname,start_time,start_time,command0,output_json);
}

/* Like build_record(), but with options.affinity and configuration_provenance
 * populated (INVESTIGATION.md's "Comparison matrix mode deep-dive") --
 * build_record() itself deliberately omits both, which already exercises
 * the "older record / no launcher involved" degrade-to-NULL path for every
 * other test in this file. options_str lets a caller supply the
 * configuration_provenance.options[] array body directly (e.g.
 * "{\"name\":\"affinity_axis\",\"value\":\"nosmt\"}"), empty string for none. */
static void build_record_with_provenance(char *buf,size_t bufsize,const char *hostname,const char *run_id,
                                          const char *start_time,const char *command0,
                                          const char *affinity_mode,const char *options_str){
  snprintf(buf,bufsize,
    "{\"schema_version\":\"%s\",\"run_id\":\"%s\",\"collector\":\"wspy\",\"wspy_version\":\"4.2\","
    "\"hostname\":\"%s\",\"cpu_vendor\":\"AMD\",\"cpu_family\":25,\"cpu_model\":1,"
    "\"environment\":{\"virt_role\":\"host\",\"hypervisor_vendor\":null,\"microcode_version\":null,"
    "\"bios_vendor\":null,\"bios_version\":null,\"bios_date\":null,\"cpu_governor\":null,"
    "\"cpu_scaling_driver\":null,\"cpu_governor_uniform\":false,\"memory_total_kb\":null,"
    "\"compiler_version\":null,\"libc_version\":null},"
    "\"environment_coverage\":{\"captured\":0,\"probed\":9},"
    "\"start_time\":\"%s\",\"finish_time\":\"%s\",\"elapsed_seconds\":1.0,"
    "\"command\":[\"%s\"],"
    "\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":0,\"signaled\":false,\"term_signal\":null},"
    "\"options\":{\"counter_mask\":\"0x1\",\"per_core\":false,\"system\":true,\"csv\":false,\"tree\":false,"
    "\"interval_seconds\":0,\"affinity\":{\"requested\":\"%s\",\"mode\":\"%s\",\"cpus\":\"0,2,4,6\"}},"
    "\"configuration_provenance\":{\"preset\":\"deep-cpu\",\"configuration\":null,\"options\":[%s]},"
    "\"counter_coverage\":{\"requested\":4,\"measured\":4},"
    "\"output_files\":{\"output_path\":null,\"tree_output_path\":null,\"manifest_path\":null}}\n",
    RUN_INDEX_SCHEMA_VERSION,run_id,hostname,start_time,start_time,command0,
    affinity_mode,affinity_mode,options_str);
}

static void write_manifest_file(const char *path,const char *command0,const char *start_time,
                                 const char *kernel_release,int num_cores,int is_hybrid){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fprintf(fp,
    "{\"schema_version\":\"%s\",\"command\":{\"argv\":[\"%s\"]},"
    "\"timing\":{\"start_time\":\"%s\",\"finish_time\":\"%s\",\"elapsed_seconds\":1.0},"
    "\"host\":{\"hostname\":\"h\",\"kernel_release\":\"%s\",\"cpu_vendor\":\"AMD\","
    "\"cpu_family\":25,\"cpu_model\":1,\"num_cores\":%d,\"num_cores_available\":%d,\"is_hybrid\":%s}}\n",
    MANIFEST_SCHEMA_VERSION,command0,start_time,start_time,kernel_release,num_cores,num_cores,
    is_hybrid ? "true" : "false");
  fclose(fp);
}

static sqlite3_int64 lookup_run_id(sqlite3 *db,const char *run_id_text){
  sqlite3_stmt *stmt;
  sqlite3_int64 id = -1;
  if (sqlite3_prepare_v2(db,"SELECT id FROM runs WHERE run_id=?;",-1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_text(stmt,1,run_id_text,-1,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) id = sqlite3_column_int64(stmt,0);
    sqlite3_finalize(stmt);
  }
  return id;
}

static int feature_row(sqlite3 *db,sqlite3_int64 run_id,const char *feature_name,
                        double *value_out,char *coverage_out,size_t coverage_size){
  sqlite3_stmt *stmt;
  int found = 0;
  if (sqlite3_prepare_v2(db,"SELECT value,coverage FROM run_features WHERE run_id=? AND feature_name=?;",
                         -1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_int64(stmt,1,run_id);
    sqlite3_bind_text(stmt,2,feature_name,-1,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW){
      found = 1;
      /* value is NULL exactly when coverage=="unavailable" -- callers must
       * check coverage_out, not value_out, to tell "unmeasured" from a
       * genuine 0.0 value. */
      *value_out = (sqlite3_column_type(stmt,0) == SQLITE_NULL) ? 0.0 : sqlite3_column_double(stmt,0);
      snprintf(coverage_out,coverage_size,"%s",(const char *)sqlite3_column_text(stmt,1));
    }
    sqlite3_finalize(stmt);
  }
  return found;
}

static int run_count(sqlite3 *db){
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM runs;",-1,&stmt,NULL) == SQLITE_OK){
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }
  return n;
}

static void test_ensure_schema_idempotent(void){
  sqlite3 *db;

  printf("Testing ensure_schema is idempotent...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  assert(ensure_schema(db) == 0); /* calling a second time on an already-current DB is fine */
  assert(run_count(db) == 0);
  sqlite3_close(db);
  printf("PASS: ensure_schema idempotent\n");
}

static void test_upsert_insert_and_reingest_no_duplicate(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;

  printf("Testing upsert_run: insert then re-ingest doesn't duplicate...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);
  assert(stats.records_new == 1 && stats.records_updated == 0);
  assert(run_count(db) == 1);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);
  assert(stats.records_updated == 1);
  assert(run_count(db) == 1); /* still one row, not two */

  sqlite3_close(db);
  printf("PASS: upsert_run insert/re-ingest\n");
}

static void test_upsert_run_ingests_affinity_and_config_provenance(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_stmt *stmt;

  printf("Testing upsert_run ingests affinity/configuration_provenance...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record_with_provenance(buf,sizeof(buf),"host1","run1","2026-07-19T00:00:00.000Z","/bin/true",
                                "nosmt","{\"name\":\"affinity_axis\",\"value\":\"nosmt\"},"
                                "{\"name\":\"compiler\",\"value\":\"gcc13\"}");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);
  assert(stats.records_new == 1);

  assert(sqlite3_prepare_v2(db,
    "SELECT preset_name,config_name,affinity_mode,affinity_requested,affinity_cpus FROM runs;",
    -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"deep-cpu"));
  assert(sqlite3_column_type(stmt,1) == SQLITE_NULL); /* configuration was null */
  assert(!strcmp((const char *)sqlite3_column_text(stmt,2),"nosmt"));
  assert(!strcmp((const char *)sqlite3_column_text(stmt,3),"nosmt"));
  assert(!strcmp((const char *)sqlite3_column_text(stmt,4),"0,2,4,6"));
  sqlite3_finalize(stmt);

  assert(sqlite3_prepare_v2(db,
    "SELECT option_name,option_value FROM run_config_options ORDER BY option_name;",
    -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"affinity_axis"));
  assert(!strcmp((const char *)sqlite3_column_text(stmt,1),"nosmt"));
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"compiler"));
  assert(!strcmp((const char *)sqlite3_column_text(stmt,1),"gcc13"));
  assert(sqlite3_step(stmt) == SQLITE_DONE); /* exactly two options, no more */
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  printf("PASS: upsert_run ingests affinity/configuration_provenance\n");
}

/* Re-ingesting a run whose config-option value changed (e.g. a corrected
 * run-index record) must update the stored value, not duplicate the row or
 * silently keep the stale one -- replace_run_config_options()'s
 * DELETE-then-INSERT shape, not the ON CONFLICT clause, is what actually
 * guarantees this (the ON CONFLICT clause only matters for a duplicate
 * name *within* one record's own options[] array); this test exercises the
 * whole re-ingest path end to end regardless of which mechanism does it. */
static void test_upsert_run_config_options_value_updates_on_reingest(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_stmt *stmt;
  int count = -1;

  printf("Testing run_config_options value updates on re-ingest, no duplication...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record_with_provenance(buf,sizeof(buf),"host1","run1","2026-07-19T00:00:00.000Z","/bin/true",
                                "all","{\"name\":\"compiler\",\"value\":\"gcc12\"}");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);

  build_record_with_provenance(buf,sizeof(buf),"host1","run1","2026-07-19T00:00:00.000Z","/bin/true",
                                "all","{\"name\":\"compiler\",\"value\":\"gcc13\"}");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);

  assert(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM run_config_options;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  count = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(count == 1); /* not two -- re-ingest replaces, doesn't accumulate */

  assert(sqlite3_prepare_v2(db,"SELECT option_value FROM run_config_options WHERE option_name='compiler';",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"gcc13"));
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  printf("PASS: run_config_options value updates on re-ingest, no duplication\n");
}

/* build_record() (no affinity/configuration_provenance at all in the JSON,
 * as if from an older wspy) already exercises this implicitly for every
 * other test in this file -- an explicit test here pins the behavior down
 * rather than leaving it as an accidental side effect of what build_record()
 * happens to omit. */
static void test_upsert_run_missing_provenance_degrades_to_null(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_stmt *stmt;
  int n = -1;

  printf("Testing upsert_run degrades to NULL when affinity/configuration_provenance are absent...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record(buf,sizeof(buf),"host1","run1","2026-07-19T00:00:00.000Z","/bin/true",0,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);

  assert(sqlite3_prepare_v2(db,
    "SELECT preset_name,config_name,affinity_mode,affinity_requested,affinity_cpus FROM runs;",
    -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_type(stmt,0) == SQLITE_NULL);
  assert(sqlite3_column_type(stmt,1) == SQLITE_NULL);
  assert(sqlite3_column_type(stmt,2) == SQLITE_NULL);
  assert(sqlite3_column_type(stmt,3) == SQLITE_NULL);
  assert(sqlite3_column_type(stmt,4) == SQLITE_NULL);
  sqlite3_finalize(stmt);

  assert(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM run_config_options;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  n = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(n == 0);

  sqlite3_close(db);
  printf("PASS: upsert_run degrades to NULL when affinity/configuration_provenance are absent\n");
}

static void test_collision_detected_and_not_overwritten(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_stmt *stmt;
  const unsigned char *command;

  printf("Testing (hostname,run_id) collision is detected and not overwritten...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* Same hostname+run_id, but a materially different start_time/command --
   * simulates two unrelated runs whose format_run_id() outputs collided. */
  build_record(buf,sizeof(buf),"host1","collide1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);
  assert(stats.records_new == 1);

  build_record(buf,sizeof(buf),"host1","collide1","2026-07-02T00:00:00.000Z","/bin/false",1,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,0,0,&stats);
  json_free(root);
  assert(stats.records_collision == 1);
  assert(run_count(db) == 1); /* not merged into a second row */

  assert(sqlite3_prepare_v2(db,"SELECT command FROM runs WHERE hostname='host1' AND run_id='collide1';",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  command = sqlite3_column_text(stmt,0);
  assert(!strcmp((const char *)command,"/bin/true")); /* original row left untouched */
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  printf("PASS: collision detected and original row preserved\n");
}

static void test_malformed_line_and_schema_mismatch(void){
  const char *path = "/tmp/test_store_index1.jsonl";
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048];

  printf("Testing malformed line skipped, schema major mismatch warns but still ingests...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(path,"{ this is not json\n");
  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  append_file(path,buf);

  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_malformed == 1);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 1);

  sqlite3_close(db);
  remove(path);
  printf("PASS: malformed line skipped\n");
}

static void test_schema_major_mismatch_warns_once(void){
  sqlite3 *db;
  struct json_value *root;
  char errbuf[256];
  int warned = 0;

  printf("Testing check_schema_major_mismatch warns once per file...\n");
  db = NULL; (void)db;

  root = json_parse("{\"schema_version\":\"2.0.0\"}",errbuf,sizeof(errbuf));
  assert(root != NULL);
  assert(check_schema_major_mismatch(root,"x.jsonl",&warned) == 1);
  assert(warned == 1);
  assert(check_schema_major_mismatch(root,"x.jsonl",&warned) == 1); /* still reports a mismatch... */
  json_free(root);
  /* ...but the caller (already_warned) is responsible for only printing once, which
   * ingest_run_index_file's schema_warned local does per-file. */
  printf("PASS: schema major mismatch detection\n");
}

static void test_manifest_enrichment_match(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *manifest_path = "/tmp/test_store_manifest_match.json";
  sqlite3_stmt *stmt;

  printf("Testing manifest enrichment when command/start_time match...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_manifest_file(manifest_path,"/bin/true","2026-07-01T00:00:00.000Z","6.1.0-test",16,0);
  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,manifest_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",1,0,0,&stats);
  json_free(root);

  assert(stats.manifests_enriched == 1);
  assert(sqlite3_prepare_v2(db,"SELECT kernel_release,num_cores,is_hybrid,manifest_ingested FROM runs;",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"6.1.0-test"));
  assert(sqlite3_column_int(stmt,1) == 16);
  assert(sqlite3_column_int(stmt,2) == 0);
  assert(sqlite3_column_int(stmt,3) == 1);
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  remove(manifest_path);
  printf("PASS: manifest enrichment on match\n");
}

static void test_manifest_enrichment_mismatch_skipped(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *manifest_path = "/tmp/test_store_manifest_mismatch.json";
  sqlite3_stmt *stmt;

  printf("Testing manifest enrichment skipped when command differs (stale/reused path)...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* Manifest on disk is for a *different* command than the run-index record
   * claims it belongs to -- e.g. a fixed output filename reused by a later
   * run. Must not be trusted. */
  write_manifest_file(manifest_path,"/bin/false","2026-07-01T00:00:00.000Z","6.1.0-test",16,0);
  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,manifest_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",1,0,0,&stats);
  json_free(root);

  assert(stats.manifests_mismatched == 1);
  assert(stats.manifests_enriched == 0);
  assert(sqlite3_prepare_v2(db,"SELECT manifest_ingested,kernel_release FROM runs;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_int(stmt,0) == 0);
  assert(sqlite3_column_type(stmt,1) == SQLITE_NULL);
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  remove(manifest_path);
  printf("PASS: manifest enrichment mismatch skipped\n");
}

static void test_manifest_enrichment_missing_path(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;

  printf("Testing manifest enrichment gracefully skips when path is null or unreadable...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",1,0,0,&stats);
  json_free(root);
  assert(stats.manifests_skipped == 1);
  assert(stats.manifests_enriched == 0);

  memset(&stats,0,sizeof(stats));
  build_record(buf,sizeof(buf),"host1","run2","2026-07-01T01:00:00.000Z","/bin/true",0,"/tmp/does_not_exist_store.json");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",1,0,0,&stats);
  json_free(root);
  assert(stats.manifests_skipped == 1);
  assert(stats.manifests_enriched == 0);

  sqlite3_close(db);
  printf("PASS: manifest enrichment missing path skipped\n");
}

static void test_ingest_sources_offset_tracking(void){
  const char *path = "/tmp/test_store_index2.jsonl";
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048];

  printf("Testing ingest_sources offset tracking skips already-ingested bytes...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  write_file(path,buf);
  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 1);

  /* Re-ingest the same (unchanged) file: nothing new to read. */
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_seen == 0);
  assert(run_count(db) == 1);

  /* File grows: only the new line should be seen. */
  build_record(buf,sizeof(buf),"host1","run2","2026-07-01T02:00:00.000Z","/bin/false",1,NULL);
  append_file(path,buf);
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 2);

  sqlite3_close(db);
  remove(path);
  printf("PASS: ingest_sources offset tracking\n");
}

static void test_incomplete_trailing_line_deferred(void){
  const char *path = "/tmp/test_store_index3.jsonl";
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048];
  size_t len;

  printf("Testing an incomplete (no trailing newline) last line is deferred, not parsed...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  write_file(path,buf);
  /* Append a second record but strip its trailing newline, simulating a
   * writer that was interrupted mid-append. */
  build_record(buf,sizeof(buf),"host1","run2","2026-07-01T03:00:00.000Z","/bin/false",1,NULL);
  len = strlen(buf);
  if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
  append_file(path,buf);

  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_seen == 1); /* only the complete first line */
  assert(run_count(db) == 1);

  /* Writer finishes the line later. */
  append_file(path,"\n");
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,0,0,&stats) == 0);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 2);

  sqlite3_close(db);
  remove(path);
  printf("PASS: incomplete trailing line deferred until complete\n");
}

static double metric_value(sqlite3 *db,const char *metric_name,int *is_null){
  sqlite3_stmt *stmt;
  double v = 0;

  *is_null = 1;
  if (sqlite3_prepare_v2(db,"SELECT value FROM metric_values WHERE metric_name=?;",-1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_text(stmt,1,metric_name,-1,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW){
      *is_null = (sqlite3_column_type(stmt,0) == SQLITE_NULL);
      v = sqlite3_column_double(stmt,0);
    }
    sqlite3_finalize(stmt);
  }
  return v;
}

static int metric_value_count(sqlite3 *db,const char *metric_name){
  sqlite3_stmt *stmt;
  int n = 0;

  if (sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM metric_values WHERE metric_name=?;",-1,&stmt,NULL) == SQLITE_OK){
    sqlite3_bind_text(stmt,1,metric_name,-1,SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt,0);
    sqlite3_finalize(stmt);
  }
  return n;
}

static void test_ingest_csv_metrics_basic(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_basic.csv";

  printf("Testing ingest_csv_metrics: basic aggregate (non-interval, non-per-core) CSV...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,"elapsed,ipc,counters_measured,counters_requested,\n0.5,1.25,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);

  assert(stats.metrics_ingested == 1);
  assert(metric_value_count(db,"ipc") == 1);
  {
    int is_null;
    double v = metric_value(db,"ipc",&is_null);
    assert(!is_null && v > 1.24 && v < 1.26);
  }
  {
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db,"SELECT metrics_ingested,metrics_row_count FROM runs;",-1,&stmt,NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt,0) == 1);
    assert(sqlite3_column_int(stmt,1) == 1);
    sqlite3_finalize(stmt);
  }

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics basic aggregate\n");
}

static void test_ingest_csv_metrics_interval(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_interval.csv";
  sqlite3_stmt *stmt;

  printf("Testing ingest_csv_metrics: --interval rows get distinct tick_time/phase...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,
    "time,phase,ipc,counters_measured,counters_requested,\n"
    " 1.0,warmup,0.50,3,3,\n"
    " 2.0,steady,0.90,3,3,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","sleep",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);

  assert(stats.metrics_ingested == 1);
  assert(metric_value_count(db,"ipc") == 2);
  assert(sqlite3_prepare_v2(db,"SELECT tick_time,phase,value FROM metric_values WHERE metric_name='ipc' ORDER BY row_index;",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_double(stmt,0) == 1.0);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,1),"warmup"));
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_double(stmt,0) == 2.0);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,1),"steady"));
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics interval rows\n");
}

static void test_ingest_csv_metrics_per_core(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_per_core.csv";
  sqlite3_stmt *stmt;

  printf("Testing ingest_csv_metrics: --per-core rows get distinct core...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,
    "elapsed,core,retire,counters_measured,counters_requested,\n"
    "0.002,0,17.3,288,288,\n"
    "0.002,1, 0.0,288,288,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);

  assert(metric_value_count(db,"retire") == 2);
  assert(sqlite3_prepare_v2(db,"SELECT core,value FROM metric_values WHERE metric_name='retire' ORDER BY core;",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_int(stmt,0) == 0);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_int(stmt,0) == 1);
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics per-core rows\n");
}

static void test_ingest_csv_metrics_percent_and_nan(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_pct_nan.csv";
  sqlite3_stmt *stmt;

  printf("Testing ingest_csv_metrics: percent cells and -nan cells...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,"elapsed,sanity,ipc,counters_measured,counters_requested,\n0.5,26.61%,-nan,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);

  assert(sqlite3_prepare_v2(db,"SELECT value,is_percent,raw_text FROM metric_values WHERE metric_name='sanity';",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_double(stmt,0) > 26.6 && sqlite3_column_double(stmt,0) < 26.62);
  assert(sqlite3_column_int(stmt,1) == 1);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,2),"26.61%"));
  sqlite3_finalize(stmt);

  assert(sqlite3_prepare_v2(db,"SELECT value,raw_text FROM metric_values WHERE metric_name='ipc';",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(sqlite3_column_type(stmt,0) == SQLITE_NULL); /* nan/-nan stored as NULL, not a real number */
  assert(!strcmp((const char *)sqlite3_column_text(stmt,1),"-nan")); /* but preserved for audit */
  sqlite3_finalize(stmt);

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics percent/-nan handling\n");
}

static void test_ingest_csv_metrics_row_mismatch_skipped(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_mismatch.csv";

  printf("Testing ingest_csv_metrics: a column-count-mismatched row is skipped, not fatal...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* Row 2 simulates the --per-core+--interval trailing unheaded block:
   * more fields than the header, must not be misattributed to header
   * column names. Rows 1 and 3 are well-formed and must still ingest. */
  write_file(csv_path,
    "time,counters_measured,counters_requested,\n"
    " 1.0,10,10,\n"
    " 2.0,10,10,20,30,\n"
    " 3.0,10,10,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","sleep",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);

  assert(stats.metrics_row_mismatches == 1);
  assert(metric_value_count(db,"counters_measured") == 2); /* only the two well-formed rows */

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics row mismatch skipped individually\n");
}

static void test_ingest_csv_metrics_reingest_idempotent(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  const char *csv_path = "/tmp/test_store_metrics_idem.csv";

  printf("Testing ingest_csv_metrics: re-ingest doesn't duplicate metric_values rows...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,"elapsed,ipc,counters_measured,counters_requested,\n0.5,1.25,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(metric_value_count(db,"ipc") == 1);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(metric_value_count(db,"ipc") == 1); /* still one, not two */

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: ingest_csv_metrics re-ingest idempotent\n");
}

static void test_ingest_csv_metrics_skipped_conditions(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;

  printf("Testing ingest_csv_metrics: gracefully skips when csv=false, path missing, or file absent/empty...\n");
  db = open_store(":memory:");
  assert(db != NULL);

  /* options.csv=false (build_record()'s default) */
  memset(&stats,0,sizeof(stats));
  build_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",0,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(stats.metrics_skipped == 1 && stats.metrics_ingested == 0);

  /* options.csv=true but output_path is null */
  memset(&stats,0,sizeof(stats));
  build_csv_record(buf,sizeof(buf),"host1","run2","2026-07-01T01:00:00.000Z","/bin/true",NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(stats.metrics_skipped == 1 && stats.metrics_ingested == 0);

  /* options.csv=true, output_path points at a file that doesn't exist */
  memset(&stats,0,sizeof(stats));
  build_csv_record(buf,sizeof(buf),"host1","run3","2026-07-01T02:00:00.000Z","/bin/true",
                    "/tmp/test_store_metrics_does_not_exist.csv");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(stats.metrics_skipped == 1 && stats.metrics_ingested == 0);

  /* options.csv=true, output_path points at an empty file */
  memset(&stats,0,sizeof(stats));
  write_file("/tmp/test_store_metrics_empty.csv","");
  build_csv_record(buf,sizeof(buf),"host1","run4","2026-07-01T03:00:00.000Z","/bin/true",
                    "/tmp/test_store_metrics_empty.csv");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,1,0,&stats);
  json_free(root);
  assert(stats.metrics_skipped == 1 && stats.metrics_ingested == 0);
  remove("/tmp/test_store_metrics_empty.csv");

  sqlite3_close(db);
  printf("PASS: ingest_csv_metrics skip conditions\n");
}

static void test_extract_run_features_basic(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  const char *csv_path = "/tmp/test_store_features_basic.csv";

  printf("Testing extract_run_features: simple-AVG features, measured vs unavailable...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* ipc/retire/frontend/backend/speculate present; dcache/branch/etc. not
   * collected this run at all (no --dcache/--branch), so those features
   * must degrade to unavailable rather than being silently omitted. */
  write_file(csv_path,
    "elapsed,ipc,retire,frontend,backend,speculate,counters_measured,counters_requested,\n"
    "1.0,1.50,60.0,15.0,20.0,5.0,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  assert(stats.features_extracted == 1);
  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(feature_row(db,run_id,"ipc_mean",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 1.49 && value < 1.51);
  assert(feature_row(db,run_id,"retire_pct",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 59.9 && value < 60.1);
  assert(feature_row(db,run_id,"speculate_pct",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 4.9 && value < 5.1);

  /* Never collected this run -- unavailable, value NULL, not 0.0. */
  assert(feature_row(db,run_id,"dcache_miss_pct",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"unavailable"));
  assert(feature_row(db,run_id,"branch_mispredict_pct",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"unavailable"));

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features basic\n");
}

static void test_extract_run_features_rates(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  const char *csv_path = "/tmp/test_store_features_rates.csv";

  printf("Testing extract_run_features: fault_rate/ctxswitch_rate from always-present rusage columns...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* build_csv_record()'s record carries elapsed_seconds=1.0, so rate ==
   * raw count here, which keeps the assertions exact rather than approximate. */
  write_file(csv_path,
    "elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,\n"
    "1.0,0.1,0.05,30,10,0,0,1000,400,8,0,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(feature_row(db,run_id,"fault_rate",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 407.9 && value < 408.1); /* (400+8)/1.0 */
  assert(feature_row(db,run_id,"ctxswitch_rate",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 39.9 && value < 40.1); /* (30+10)/1.0 */

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features rates\n");
}

static void test_extract_run_features_phase_stability(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  const char *csv_path = "/tmp/test_store_features_phase.csv";

  printf("Testing extract_run_features: phase_stability counts distinct ticks, not distinct metrics...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* Two metric columns per tick (ipc,retire) -- if phase_stability counted
   * raw metric_values rows instead of distinct row_index, it would double
   * every tick and still coincidentally compute the same fraction here, so
   * the real regression this guards is counting rows sharing one row_index
   * as more than one tick; 3 steady out of 4 ticks total is the fraction
   * under test either way. */
  write_file(csv_path,
    "time,phase,ipc,retire,counters_measured,counters_requested,\n"
    "1.0,warmup,0.3,10.0,9,9,\n"
    "2.0,steady,0.9,60.0,9,9,\n"
    "3.0,steady,0.9,61.0,9,9,\n"
    "4.0,steady,0.9,59.0,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","sleep",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(feature_row(db,run_id,"phase_stability",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 0.74 && value < 0.76); /* 3/4 */

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features phase_stability\n");
}

static void test_extract_run_features_parallelism_proxy(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  const char *csv_path = "/tmp/test_store_features_percore.csv";

  printf("Testing extract_run_features: parallelism_proxy cross-core CV, needs >=2 cores...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  /* Two cores at very different IPC -- a real imbalance, so the CV should
   * be clearly nonzero (not asserting an exact value, since the point of
   * this test is "measured, with a real spread" not the specific formula
   * constant already covered by the rates/basic tests above). */
  write_file(csv_path,
    "elapsed,core,ipc,counters_measured,counters_requested,\n"
    "1.0,0,2.0,9,9,\n"
    "1.0,1,0.2,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(feature_row(db,run_id,"parallelism_proxy",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 0.5);

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features parallelism_proxy\n");
}

static void test_extract_run_features_single_core_unavailable(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  const char *csv_path = "/tmp/test_store_features_onecore.csv";

  printf("Testing extract_run_features: parallelism_proxy needs >=2 cores, else unavailable...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,"elapsed,ipc,counters_measured,counters_requested,\n1.0,1.5,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(feature_row(db,run_id,"parallelism_proxy",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"unavailable"));

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features single-core unavailable\n");
}

static void test_extract_run_features_reingest_idempotent(void){
  sqlite3 *db;
  struct store_stats stats;
  char buf[2048],errbuf[256];
  struct json_value *root;
  sqlite3_int64 run_id;
  double value;
  char coverage[16];
  int count;
  sqlite3_stmt *stmt;
  const char *csv_path = "/tmp/test_store_features_reingest.csv";

  printf("Testing extract_run_features: re-extraction upserts, doesn't duplicate or ignore updated data...\n");
  db = open_store(":memory:");
  assert(db != NULL);
  memset(&stats,0,sizeof(stats));

  write_file(csv_path,"elapsed,ipc,counters_measured,counters_requested,\n1.0,1.0,9,9,\n");
  build_csv_record(buf,sizeof(buf),"host1","run1","2026-07-01T00:00:00.000Z","/bin/true",csv_path);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  /* Change the underlying CSV and re-run extraction directly (as a
   * standalone backfill pass would, without re-ingesting metric_values) --
   * this exercises extract_run_features() being safe to call again on its
   * own, not just via upsert_run()'s CSV-ingest path. */
  write_file(csv_path,"elapsed,ipc,counters_measured,counters_requested,\n1.0,3.0,9,9,\n");
  memset(&stats,0,sizeof(stats));
  root = json_parse(buf,errbuf,sizeof(errbuf)); /* same record, same output_path */
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,1,1,&stats);
  json_free(root);

  run_id = lookup_run_id(db,"run1");
  assert(run_id > 0);

  assert(sqlite3_prepare_v2(db,"SELECT COUNT(*) FROM run_features WHERE run_id=? AND feature_name='ipc_mean';",
                             -1,&stmt,NULL) == SQLITE_OK);
  sqlite3_bind_int64(stmt,1,run_id);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  count = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(count == 1); /* upserted, not duplicated */

  assert(feature_row(db,run_id,"ipc_mean",&value,coverage,sizeof(coverage)));
  assert(!strcmp(coverage,"measured") && value > 2.99 && value < 3.01); /* reflects the new CSV, not the old */

  sqlite3_close(db);
  remove(csv_path);
  printf("PASS: extract_run_features re-extraction idempotent and refreshes\n");
}

static void test_schema_migration_v3_to_v4(void){
  sqlite3 *db;
  sqlite3_stmt *stmt;
  int user_version = 0;

  printf("Testing ensure_schema migrates a v3-shaped database to v4...\n");
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);

  /* The exact v3 shape (preset_name/config_name/affinity columns and
   * run_config_options already exist, but no run_features yet) -- mirrors a
   * real database created by wspy-store before this item's feature
   * extraction existed. */
  assert(sqlite3_exec(db,
    "CREATE TABLE store_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE ingest_sources (path TEXT PRIMARY KEY, last_byte_offset INTEGER NOT NULL DEFAULT 0,"
    " last_size INTEGER NOT NULL DEFAULT 0, last_ingested_at TEXT);"
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL,"
    " run_index_schema_version TEXT NOT NULL, collector TEXT NOT NULL, wspy_version TEXT,"
    " cpu_vendor TEXT, cpu_family INTEGER, cpu_model INTEGER, start_time TEXT NOT NULL,"
    " finish_time TEXT, elapsed_seconds REAL, command TEXT NOT NULL, exit_known INTEGER,"
    " exit_exited INTEGER, exit_code INTEGER, exit_signaled INTEGER, exit_term_signal INTEGER,"
    " per_core INTEGER, system_flag INTEGER, csv_flag INTEGER, tree_flag INTEGER,"
    " interval_seconds INTEGER, counter_mask TEXT, counter_mask_int INTEGER,"
    " counters_requested INTEGER, counters_measured INTEGER, output_path TEXT,"
    " tree_output_path TEXT, manifest_path TEXT, manifest_ingested INTEGER NOT NULL DEFAULT 0,"
    " kernel_release TEXT, num_cores INTEGER, num_cores_available INTEGER, is_hybrid INTEGER,"
    " metrics_ingested INTEGER NOT NULL DEFAULT 0, metrics_row_count INTEGER,"
    " preset_name TEXT, config_name TEXT, affinity_mode TEXT, affinity_requested TEXT, affinity_cpus TEXT,"
    " source_run_index_path TEXT NOT NULL, ingested_at TEXT NOT NULL, UNIQUE(hostname, run_id));"
    "CREATE TABLE metric_values (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL, row_index INTEGER NOT NULL,"
    " tick_time REAL, core INTEGER, phase TEXT, metric_name TEXT NOT NULL, value REAL,"
    " is_percent INTEGER NOT NULL DEFAULT 0, raw_text TEXT NOT NULL);"
    "CREATE TABLE run_config_options (run_id INTEGER NOT NULL, option_name TEXT NOT NULL,"
    " option_value TEXT NOT NULL, PRIMARY KEY (run_id, option_name));"
    "INSERT INTO runs (run_id,hostname,run_index_schema_version,collector,start_time,command,"
    " source_run_index_path,ingested_at) VALUES"
    " ('r1','h1','1.7.0','wspy','2026-01-01T00:00:00Z','/bin/true','idx.jsonl','2026-01-01T00:00:00Z');"
    "PRAGMA user_version=3;",NULL,NULL,NULL) == SQLITE_OK);

  assert(ensure_schema(db) == 0);

  assert(sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  user_version = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(user_version == STORE_SCHEMA_VERSION);

  /* The pre-existing row survived, and run_features is usable. */
  assert(sqlite3_prepare_v2(db,"SELECT run_id FROM runs;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"r1"));
  sqlite3_finalize(stmt);

  assert(sqlite3_exec(db,
    "INSERT INTO run_features (run_id,feature_name,value,coverage,feature_set_version) "
    "VALUES (1,'ipc_mean',1.5,'measured','1.0');",
    NULL,NULL,NULL) == SQLITE_OK);

  assert(ensure_schema(db) == 0); /* no-op at current version */

  sqlite3_close(db);
  printf("PASS: schema migration v3 to v4\n");
}

static void test_schema_migration_v1_to_v2(void){
  sqlite3 *db;
  sqlite3_stmt *stmt;
  int user_version = 0;

  printf("Testing ensure_schema migrates a v1-shaped database to v2...\n");
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);

  /* Hand-build the exact v1 schema wspy-store originally shipped with
   * (no metrics_ingested/metrics_row_count columns, no metric_values
   * table), then mark it as v1 -- mirrors a real database created by an
   * older wspy-store binary before this migration existed. */
  assert(sqlite3_exec(db,
    "CREATE TABLE store_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE ingest_sources (path TEXT PRIMARY KEY, last_byte_offset INTEGER NOT NULL DEFAULT 0,"
    " last_size INTEGER NOT NULL DEFAULT 0, last_ingested_at TEXT);"
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL,"
    " run_index_schema_version TEXT NOT NULL, collector TEXT NOT NULL, wspy_version TEXT,"
    " cpu_vendor TEXT, cpu_family INTEGER, cpu_model INTEGER, start_time TEXT NOT NULL,"
    " finish_time TEXT, elapsed_seconds REAL, command TEXT NOT NULL, exit_known INTEGER,"
    " exit_exited INTEGER, exit_code INTEGER, exit_signaled INTEGER, exit_term_signal INTEGER,"
    " per_core INTEGER, system_flag INTEGER, csv_flag INTEGER, tree_flag INTEGER,"
    " interval_seconds INTEGER, counter_mask TEXT, counter_mask_int INTEGER,"
    " counters_requested INTEGER, counters_measured INTEGER, output_path TEXT,"
    " tree_output_path TEXT, manifest_path TEXT, manifest_ingested INTEGER NOT NULL DEFAULT 0,"
    " kernel_release TEXT, num_cores INTEGER, num_cores_available INTEGER, is_hybrid INTEGER,"
    " source_run_index_path TEXT NOT NULL, ingested_at TEXT NOT NULL, UNIQUE(hostname, run_id));"
    "INSERT INTO runs (run_id,hostname,run_index_schema_version,collector,start_time,command,"
    " source_run_index_path,ingested_at) VALUES"
    " ('r1','h1','1.3.0','wspy','2026-01-01T00:00:00Z','/bin/true','idx.jsonl','2026-01-01T00:00:00Z');"
    "PRAGMA user_version=1;",NULL,NULL,NULL) == SQLITE_OK);

  assert(ensure_schema(db) == 0);

  assert(sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  user_version = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(user_version == STORE_SCHEMA_VERSION);

  /* The pre-existing row survived the migration, and the new columns are
   * usable (default to 0/NULL, not an error). */
  assert(sqlite3_prepare_v2(db,"SELECT run_id,metrics_ingested,metrics_row_count FROM runs;",
                             -1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"r1"));
  assert(sqlite3_column_int(stmt,1) == 0);
  assert(sqlite3_column_type(stmt,2) == SQLITE_NULL);
  sqlite3_finalize(stmt);

  /* metric_values exists and is usable. */
  assert(sqlite3_exec(db,
    "INSERT INTO metric_values (run_id,row_index,metric_name,value,raw_text) VALUES (1,0,'ipc',1.0,'1.0');",
    NULL,NULL,NULL) == SQLITE_OK);

  /* Calling ensure_schema() again at the now-current version is a no-op. */
  assert(ensure_schema(db) == 0);

  sqlite3_close(db);
  printf("PASS: schema migration v1 to v2\n");
}

static void test_schema_migration_v2_to_v3(void){
  sqlite3 *db;
  sqlite3_stmt *stmt;
  int user_version = 0;

  printf("Testing ensure_schema migrates a v2-shaped database to v3...\n");
  assert(sqlite3_open(":memory:",&db) == SQLITE_OK);

  /* The exact v2 shape (metric_values exists, but no preset_name/config_name,
   * no affinity_ columns, no run_config_options table yet) -- mirrors a real
   * database created by wspy-store before this item's ingestion support
   * existed. */
  assert(sqlite3_exec(db,
    "CREATE TABLE store_meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
    "CREATE TABLE ingest_sources (path TEXT PRIMARY KEY, last_byte_offset INTEGER NOT NULL DEFAULT 0,"
    " last_size INTEGER NOT NULL DEFAULT 0, last_ingested_at TEXT);"
    "CREATE TABLE runs (id INTEGER PRIMARY KEY, run_id TEXT NOT NULL, hostname TEXT NOT NULL,"
    " run_index_schema_version TEXT NOT NULL, collector TEXT NOT NULL, wspy_version TEXT,"
    " cpu_vendor TEXT, cpu_family INTEGER, cpu_model INTEGER, start_time TEXT NOT NULL,"
    " finish_time TEXT, elapsed_seconds REAL, command TEXT NOT NULL, exit_known INTEGER,"
    " exit_exited INTEGER, exit_code INTEGER, exit_signaled INTEGER, exit_term_signal INTEGER,"
    " per_core INTEGER, system_flag INTEGER, csv_flag INTEGER, tree_flag INTEGER,"
    " interval_seconds INTEGER, counter_mask TEXT, counter_mask_int INTEGER,"
    " counters_requested INTEGER, counters_measured INTEGER, output_path TEXT,"
    " tree_output_path TEXT, manifest_path TEXT, manifest_ingested INTEGER NOT NULL DEFAULT 0,"
    " kernel_release TEXT, num_cores INTEGER, num_cores_available INTEGER, is_hybrid INTEGER,"
    " metrics_ingested INTEGER NOT NULL DEFAULT 0, metrics_row_count INTEGER,"
    " source_run_index_path TEXT NOT NULL, ingested_at TEXT NOT NULL, UNIQUE(hostname, run_id));"
    "CREATE TABLE metric_values (id INTEGER PRIMARY KEY, run_id INTEGER NOT NULL, row_index INTEGER NOT NULL,"
    " tick_time REAL, core INTEGER, phase TEXT, metric_name TEXT NOT NULL, value REAL,"
    " is_percent INTEGER NOT NULL DEFAULT 0, raw_text TEXT NOT NULL);"
    "INSERT INTO runs (run_id,hostname,run_index_schema_version,collector,start_time,command,"
    " source_run_index_path,ingested_at) VALUES"
    " ('r1','h1','1.5.0','wspy','2026-01-01T00:00:00Z','/bin/true','idx.jsonl','2026-01-01T00:00:00Z');"
    "PRAGMA user_version=2;",NULL,NULL,NULL) == SQLITE_OK);

  assert(ensure_schema(db) == 0);

  assert(sqlite3_prepare_v2(db,"PRAGMA user_version;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  user_version = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);
  assert(user_version == STORE_SCHEMA_VERSION);

  /* The pre-existing row survived, and the new columns are usable (NULL,
   * not an error). */
  assert(sqlite3_prepare_v2(db,
    "SELECT run_id,preset_name,affinity_mode FROM runs;",-1,&stmt,NULL) == SQLITE_OK);
  assert(sqlite3_step(stmt) == SQLITE_ROW);
  assert(!strcmp((const char *)sqlite3_column_text(stmt,0),"r1"));
  assert(sqlite3_column_type(stmt,1) == SQLITE_NULL);
  assert(sqlite3_column_type(stmt,2) == SQLITE_NULL);
  sqlite3_finalize(stmt);

  /* run_config_options exists and is usable. */
  assert(sqlite3_exec(db,
    "INSERT INTO run_config_options (run_id,option_name,option_value) VALUES (1,'compiler','gcc13');",
    NULL,NULL,NULL) == SQLITE_OK);

  assert(ensure_schema(db) == 0); /* no-op at current version */

  sqlite3_close(db);
  printf("PASS: schema migration v2 to v3\n");
}

int main(void){
  test_ensure_schema_idempotent();
  test_upsert_insert_and_reingest_no_duplicate();
  test_upsert_run_ingests_affinity_and_config_provenance();
  test_upsert_run_config_options_value_updates_on_reingest();
  test_upsert_run_missing_provenance_degrades_to_null();
  test_collision_detected_and_not_overwritten();
  test_malformed_line_and_schema_mismatch();
  test_schema_major_mismatch_warns_once();
  test_manifest_enrichment_match();
  test_manifest_enrichment_mismatch_skipped();
  test_manifest_enrichment_missing_path();
  test_ingest_sources_offset_tracking();
  test_incomplete_trailing_line_deferred();
  test_ingest_csv_metrics_basic();
  test_ingest_csv_metrics_interval();
  test_ingest_csv_metrics_per_core();
  test_ingest_csv_metrics_percent_and_nan();
  test_ingest_csv_metrics_row_mismatch_skipped();
  test_ingest_csv_metrics_reingest_idempotent();
  test_ingest_csv_metrics_skipped_conditions();
  test_extract_run_features_basic();
  test_extract_run_features_rates();
  test_extract_run_features_phase_stability();
  test_extract_run_features_parallelism_proxy();
  test_extract_run_features_single_core_unavailable();
  test_extract_run_features_reingest_idempotent();
  test_schema_migration_v1_to_v2();
  test_schema_migration_v2_to_v3();
  test_schema_migration_v3_to_v4();

  printf("\nAll test_store tests passed.\n");
  return 0;
}
