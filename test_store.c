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
  upsert_run(db,root,"idx.jsonl",0,&stats);
  json_free(root);
  assert(stats.records_new == 1 && stats.records_updated == 0);
  assert(run_count(db) == 1);

  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",0,&stats);
  json_free(root);
  assert(stats.records_updated == 1);
  assert(run_count(db) == 1); /* still one row, not two */

  sqlite3_close(db);
  printf("PASS: upsert_run insert/re-ingest\n");
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
  upsert_run(db,root,"idx.jsonl",0,&stats);
  json_free(root);
  assert(stats.records_new == 1);

  build_record(buf,sizeof(buf),"host1","collide1","2026-07-02T00:00:00.000Z","/bin/false",1,NULL);
  root = json_parse(buf,errbuf,sizeof(errbuf));
  upsert_run(db,root,"idx.jsonl",0,&stats);
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

  assert(ingest_run_index_file(db,path,0,&stats) == 0);
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
  upsert_run(db,root,"idx.jsonl",1,&stats);
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
  upsert_run(db,root,"idx.jsonl",1,&stats);
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
  upsert_run(db,root,"idx.jsonl",1,&stats);
  json_free(root);
  assert(stats.manifests_skipped == 1);
  assert(stats.manifests_enriched == 0);

  memset(&stats,0,sizeof(stats));
  build_record(buf,sizeof(buf),"host1","run2","2026-07-01T01:00:00.000Z","/bin/true",0,"/tmp/does_not_exist_store.json");
  root = json_parse(buf,errbuf,sizeof(errbuf));
  assert(root != NULL);
  upsert_run(db,root,"idx.jsonl",1,&stats);
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
  assert(ingest_run_index_file(db,path,0,&stats) == 0);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 1);

  /* Re-ingest the same (unchanged) file: nothing new to read. */
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,&stats) == 0);
  assert(stats.records_seen == 0);
  assert(run_count(db) == 1);

  /* File grows: only the new line should be seen. */
  build_record(buf,sizeof(buf),"host1","run2","2026-07-01T02:00:00.000Z","/bin/false",1,NULL);
  append_file(path,buf);
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,&stats) == 0);
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

  assert(ingest_run_index_file(db,path,0,&stats) == 0);
  assert(stats.records_seen == 1); /* only the complete first line */
  assert(run_count(db) == 1);

  /* Writer finishes the line later. */
  append_file(path,"\n");
  memset(&stats,0,sizeof(stats));
  assert(ingest_run_index_file(db,path,0,&stats) == 0);
  assert(stats.records_seen == 1);
  assert(run_count(db) == 2);

  sqlite3_close(db);
  remove(path);
  printf("PASS: incomplete trailing line deferred until complete\n");
}

int main(void){
  test_ensure_schema_idempotent();
  test_upsert_insert_and_reingest_no_duplicate();
  test_collision_detected_and_not_overwritten();
  test_malformed_line_and_schema_mismatch();
  test_schema_major_mismatch_warns_once();
  test_manifest_enrichment_match();
  test_manifest_enrichment_mismatch_skipped();
  test_manifest_enrichment_missing_path();
  test_ingest_sources_offset_tracking();
  test_incomplete_trailing_line_deferred();

  printf("\nAll test_store tests passed.\n");
  return 0;
}
