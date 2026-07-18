#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_LEDGER 1
#include "ledger.c"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static struct ledger_entry *find_entry(struct ledger_entry *entries,int n,const char *name){
  int i;
  for (i = 0; i < n; i++) if (!strcmp(entries[i].name,name)) return &entries[i];
  return NULL;
}

static void test_parse_ledger_line(void){
  char plain[] = "  foo  ";
  char annotated[] = "bar\tunsupported\tfails to compile";
  char partial[] = "baz\tneeds-tool-support";
  char comment[] = "# a comment";
  char blank[] = "   ";
  char inline_comment[] = "qux  # trailing comment";
  char inline_annotated[] = "quux\tunsupported  # docker only";
  char *name,*status,*note;

  printf("Testing parse_ledger_line...\n");

  assert(parse_ledger_line(plain,&name,&status,&note) == 1);
  assert(!strcmp(name,"foo"));
  assert(status == NULL);
  assert(note == NULL);

  assert(parse_ledger_line(annotated,&name,&status,&note) == 1);
  assert(!strcmp(name,"bar"));
  assert(!strcmp(status,"unsupported"));
  assert(!strcmp(note,"fails to compile"));

  assert(parse_ledger_line(partial,&name,&status,&note) == 1);
  assert(!strcmp(name,"baz"));
  assert(!strcmp(status,"needs-tool-support"));
  assert(note == NULL);

  assert(parse_ledger_line(comment,&name,&status,&note) == 0);
  assert(parse_ledger_line(blank,&name,&status,&note) == 0);

  assert(parse_ledger_line(inline_comment,&name,&status,&note) == 1);
  assert(!strcmp(name,"qux"));
  assert(status == NULL);
  assert(note == NULL);

  assert(parse_ledger_line(inline_annotated,&name,&status,&note) == 1);
  assert(!strcmp(name,"quux"));
  assert(!strcmp(status,"unsupported"));
  assert(note == NULL);

  printf("PASS: parse_ledger_line\n");
}

static void test_load_workload_list(void){
  const char *path = "/tmp/test_ledger_list.txt";
  struct ledger_entry *entries;
  int n;
  struct ledger_entry *e;

  printf("Testing load_workload_list...\n");

  write_file(path,
    "# candidate workloads\n"
    "\n"
    "alpha\n"
    "beta\tunsupported\tdocker based\n"
    "gamma\tneeds-tool-support\tneeds AMDGPU build\n"
    "delta\tbogus-status\n");

  entries = load_workload_list(path,&n);
  assert(entries != NULL);
  assert(n == 4);

  e = find_entry(entries,n,"alpha");
  assert(e != NULL && !e->annotated);

  e = find_entry(entries,n,"beta");
  assert(e != NULL && e->annotated && e->annotated_status == LEDGER_UNSUPPORTED);
  assert(!strcmp(e->note,"docker based"));

  e = find_entry(entries,n,"gamma");
  assert(e != NULL && e->annotated && e->annotated_status == LEDGER_NEEDS_TOOL_SUPPORT);

  e = find_entry(entries,n,"delta");
  assert(e != NULL && !e->annotated); /* unrecognized status keyword is ignored */

  free(entries);
  remove(path);
  printf("PASS: load_workload_list\n");
}

static void write_run_index_record_v(FILE *fp,const char *run_id,const char *start_time,
                                      const char *cmd,int exit_known,int exit_code,const char *schema_version){
  fprintf(fp,"{\"schema_version\":\"%s\",\"run_id\":\"%s\",\"start_time\":\"%s\",\"command\":[\"wspy\",\"--\",\"%s\"],",
          schema_version,run_id,start_time,cmd);
  if (exit_known)
    fprintf(fp,"\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":%d,\"signaled\":false,\"term_signal\":null}}\n",
            exit_code);
  else
    fprintf(fp,"\"exit_status\":{\"known\":false,\"exited\":null,\"exit_code\":null,\"signaled\":null,\"term_signal\":null}}\n");
}

static void write_run_index_record(FILE *fp,const char *run_id,const char *start_time,
                                    const char *cmd,int exit_known,int exit_code){
  write_run_index_record_v(fp,run_id,start_time,cmd,exit_known,exit_code,RUN_INDEX_SCHEMA_VERSION);
}

static void test_process_run_index_file(void){
  const char *index_path = "/tmp/test_ledger_index.jsonl";
  const char *list_path = "/tmp/test_ledger_list2.txt";
  struct ledger_entry *entries;
  int n;
  FILE *fp;
  struct ledger_entry *e;

  printf("Testing process_run_index_file / entry_status...\n");

  write_file(list_path,
    "500.perlbench_r\n"
    "999.never_run\n"
    "541.leela_r\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  write_run_index_record(fp,"run1","2026-07-01T00:00:00.000Z","runcpu 500.perlbench_r",1,0);
  write_run_index_record(fp,"run2","2026-07-02T00:00:00.000Z","runcpu 541.leela_r",1,1);
  write_run_index_record(fp,"run3","2026-07-03T00:00:00.000Z","runcpu 541.leela_r",1,1);
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 3);
  assert(process_run_index_file(index_path,entries,n) == 3);

  e = find_entry(entries,n,"500.perlbench_r");
  assert(entry_status(e) == LEDGER_DONE);
  assert(e->runs_matched == 1 && e->runs_succeeded == 1);

  e = find_entry(entries,n,"999.never_run");
  assert(entry_status(e) == LEDGER_SKIPPED);
  assert(e->runs_matched == 0);

  e = find_entry(entries,n,"541.leela_r");
  assert(entry_status(e) == LEDGER_NEEDS_TOOL_SUPPORT);
  assert(e->runs_matched == 2 && e->runs_succeeded == 0);
  assert(!strcmp(e->last_run_id,"run3")); /* most recent by start_time, not file order */

  free(entries);
  remove(index_path);
  remove(list_path);
  printf("PASS: process_run_index_file / entry_status\n");
}

static void test_annotation_overrides_inference(void){
  const char *index_path = "/tmp/test_ledger_index2.jsonl";
  const char *list_path = "/tmp/test_ledger_list3.txt";
  struct ledger_entry *entries;
  int n;
  FILE *fp;
  struct ledger_entry *e;

  printf("Testing annotation overrides run-index inference...\n");

  write_file(list_path,"999.imagick_r\tunsupported\tIntel not supported\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  write_run_index_record(fp,"run1","2026-07-01T00:00:00.000Z","runcpu 999.imagick_r",1,0);
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 1);
  process_run_index_file(index_path,entries,n);

  e = find_entry(entries,n,"999.imagick_r");
  assert(e->runs_matched == 1); /* run index scan still counts the match */
  assert(entry_status(e) == LEDGER_UNSUPPORTED); /* but the annotation wins */

  free(entries);
  remove(index_path);
  remove(list_path);
  printf("PASS: annotation overrides run-index inference\n");
}

static void test_malformed_record_skipped(void){
  const char *index_path = "/tmp/test_ledger_index3.jsonl";
  const char *list_path = "/tmp/test_ledger_list4.txt";
  struct ledger_entry *entries;
  int n;
  FILE *fp;

  printf("Testing malformed run-index record is skipped, not fatal...\n");

  write_file(list_path,"500.perlbench_r\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  fprintf(fp,"{ this is not json\n");
  write_run_index_record(fp,"run1","2026-07-01T00:00:00.000Z","runcpu 500.perlbench_r",1,0);
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 1);
  assert(process_run_index_file(index_path,entries,n) == 1); /* only the valid line counted */
  assert(entry_status(&entries[0]) == LEDGER_DONE);

  free(entries);
  remove(index_path);
  remove(list_path);
  printf("PASS: malformed run-index record skipped\n");
}

static void test_schema_version_mismatch_warns(void){
  const char *index_path = "/tmp/test_ledger_index5.jsonl";
  const char *list_path = "/tmp/test_ledger_list6.txt";
  struct ledger_entry *entries;
  int n;
  FILE *fp;

  printf("Testing mismatched RUN_INDEX_SCHEMA_VERSION warns but still processes records...\n");

  write_file(list_path,"500.perlbench_r\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  write_run_index_record_v(fp,"run1","2026-07-01T00:00:00.000Z","runcpu 500.perlbench_r",1,0,"2.0.0");
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 1);
  /* A major-version mismatch is a warning (to stderr), not a processing
   * failure -- the record still counts, same as validate.c's
   * check_schema_version() only WARNs on manifests. */
  assert(process_run_index_file(index_path,entries,n) == 1);
  assert(entry_status(&entries[0]) == LEDGER_DONE);

  free(entries);
  remove(index_path);
  remove(list_path);
  printf("PASS: mismatched schema version warns but still processes records\n");
}

static void test_schema_version_missing_field(void){
  const char *index_path = "/tmp/test_ledger_index6.jsonl";
  const char *list_path = "/tmp/test_ledger_list7.txt";
  struct ledger_entry *entries;
  int n;
  FILE *fp;

  printf("Testing missing schema_version field warns but still processes records...\n");

  write_file(list_path,"500.perlbench_r\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  fprintf(fp,"{\"run_id\":\"run1\",\"start_time\":\"2026-07-01T00:00:00.000Z\","
             "\"command\":[\"wspy\",\"--\",\"runcpu 500.perlbench_r\"],"
             "\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":0,\"signaled\":false,\"term_signal\":null}}\n");
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 1);
  assert(process_run_index_file(index_path,entries,n) == 1);
  assert(entry_status(&entries[0]) == LEDGER_DONE);

  free(entries);
  remove(index_path);
  remove(list_path);
  printf("PASS: missing schema_version field warns but still processes records\n");
}

static void write_run_index_record_with_output(FILE *fp,const char *run_id,const char *start_time,
                                                const char *cmd,int exit_known,int exit_code,
                                                const char *output_path){
  fprintf(fp,"{\"schema_version\":\"%s\",\"run_id\":\"%s\",\"start_time\":\"%s\",\"command\":[\"wspy\",\"--\",\"%s\"],",
          RUN_INDEX_SCHEMA_VERSION,run_id,start_time,cmd);
  fprintf(fp,"\"output_files\":{\"output_path\":\"%s\",\"tree_output_path\":null,\"manifest_path\":null},",output_path);
  if (exit_known)
    fprintf(fp,"\"exit_status\":{\"known\":true,\"exited\":true,\"exit_code\":%d,\"signaled\":false,\"term_signal\":null}}\n",
            exit_code);
  else
    fprintf(fp,"\"exit_status\":{\"known\":false,\"exited\":null,\"exit_code\":null,\"signaled\":null,\"term_signal\":null}}\n");
}

static void test_record_paths_exist(void){
  const char *live_path = "/tmp/test_ledger_paths_live.txt";
  struct json_value *root;
  char errbuf[256];

  printf("Testing record_paths_exist...\n");

  write_file(live_path,"data\n");

  root = json_parse("{\"output_files\":{\"output_path\":null,\"tree_output_path\":null,\"manifest_path\":null}}",
                     errbuf,sizeof(errbuf));
  assert(root != NULL);
  assert(record_paths_exist(root) == -1); /* no paths named at all */
  json_free(root);

  {
    char buf[512];
    snprintf(buf,sizeof(buf),
             "{\"output_files\":{\"output_path\":\"%s\",\"tree_output_path\":null,\"manifest_path\":null}}",
             live_path);
    root = json_parse(buf,errbuf,sizeof(errbuf));
    assert(root != NULL);
    assert(record_paths_exist(root) == 1); /* names a path, and it exists */
    json_free(root);
  }

  root = json_parse("{\"output_files\":{\"output_path\":\"/tmp/test_ledger_paths_does_not_exist.txt\","
                     "\"tree_output_path\":null,\"manifest_path\":null}}",errbuf,sizeof(errbuf));
  assert(root != NULL);
  assert(record_paths_exist(root) == 0); /* names a path, but it's gone */
  json_free(root);

  remove(live_path);
  printf("PASS: record_paths_exist\n");
}

static void test_stale_run_excluded_from_scoring(void){
  const char *index_path = "/tmp/test_ledger_index8.jsonl";
  const char *list_path = "/tmp/test_ledger_list9.txt";
  const char *live_output = "/tmp/test_ledger_live_output.csv";
  const char *deleted_output = "/tmp/test_ledger_deleted_output.csv";
  struct ledger_entry *entries;
  int n;
  FILE *fp;
  struct ledger_entry *e;

  printf("Testing a matching run whose output file was deleted is excluded as stale...\n");

  write_file(list_path,
    "500.perlbench_r\n"
    "541.leela_r\n");

  write_file(live_output,"time,ipc\n1,0.5\n");
  remove(deleted_output); /* make sure it really doesn't exist */

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  /* perlbench: matching run's own output file still exists -> stays DONE */
  write_run_index_record_with_output(fp,"run1","2026-07-01T00:00:00.000Z","runcpu 500.perlbench_r",1,0,live_output);
  /* leela: the only matching run failed *and* its output file was deleted
   * -- simulating deleting a run directory after a bad, environment-caused
   * run -- so it should no longer count as evidence of a real attempt. */
  write_run_index_record_with_output(fp,"run2","2026-07-02T00:00:00.000Z","runcpu 541.leela_r",1,1,deleted_output);
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 2);
  assert(process_run_index_file(index_path,entries,n) == 2);

  e = find_entry(entries,n,"500.perlbench_r");
  assert(entry_status(e) == LEDGER_DONE);
  assert(e->runs_matched == 1 && e->runs_stale == 0);

  e = find_entry(entries,n,"541.leela_r");
  assert(entry_status(e) == LEDGER_SKIPPED); /* stale run doesn't count as an attempt */
  assert(e->runs_matched == 0 && e->runs_succeeded == 0 && e->runs_stale == 1);
  assert(!strcmp(e->last_stale_run_id,"run2"));

  free(entries);
  remove(index_path);
  remove(list_path);
  remove(live_output);
  printf("PASS: stale run excluded from scoring\n");
}

int main(void){
  test_parse_ledger_line();
  test_load_workload_list();
  test_process_run_index_file();
  test_annotation_overrides_inference();
  test_malformed_record_skipped();
  test_schema_version_mismatch_warns();
  test_schema_version_missing_field();
  test_record_paths_exist();
  test_stale_run_excluded_from_scoring();

  printf("\nAll test_ledger tests passed.\n");
  return 0;
}
