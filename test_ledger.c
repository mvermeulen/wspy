#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/stat.h>

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

static void mkdir_p(const char *path){
  char tmp[512];
  char *p;

  snprintf(tmp,sizeof(tmp),"%s",path);
  for (p = tmp+1; *p; p++){
    if (*p == '/'){
      *p = '\0';
      mkdir(tmp,0755);
      *p = '/';
    }
  }
  mkdir(tmp,0755);
}

static void rmdir_recursive(const char *path){
  char cmd[600];
  snprintf(cmd,sizeof(cmd),"rm -rf '%s'",path);
  system(cmd);
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

static void test_strip_version_suffix(void){
  char out[MAX_NAME];

  printf("Testing strip_version_suffix...\n");

  strip_version_suffix("dirt-rally2-1.0.3",out,sizeof(out));
  assert(!strcmp(out,"dirt-rally2"));

  strip_version_suffix("build-linux-kernel-1.0.0",out,sizeof(out));
  assert(!strcmp(out,"build-linux-kernel"));

  strip_version_suffix("xplane11-1.1.2",out,sizeof(out));
  assert(!strcmp(out,"xplane11"));

  /* No version-looking suffix -- left unchanged. */
  strip_version_suffix("no-version-here",out,sizeof(out));
  assert(!strcmp(out,"no-version-here"));

  printf("PASS: strip_version_suffix\n");
}

static void test_extract_external_dependencies(void){
  char out[1024];
  const char *xml_with = "<TestProfile>\n"
    "  <ExternalDependencies>steam, vulkan-development</ExternalDependencies>\n"
    "</TestProfile>\n";
  const char *xml_without = "<TestProfile>\n<ProjectURL>http://example.com</ProjectURL>\n</TestProfile>\n";

  printf("Testing extract_external_dependencies...\n");

  assert(extract_external_dependencies(xml_with,out,sizeof(out)) == 1);
  assert(!strcmp(out,"steam, vulkan-development"));

  assert(extract_external_dependencies(xml_without,out,sizeof(out)) == 0);

  printf("PASS: extract_external_dependencies\n");
}

static void test_count_option_combinations(void){
  int has_freeform;
  const char *no_settings = "<PhoronixTestSuite>\n<TestProfile>\n</TestProfile>\n</PhoronixTestSuite>\n";
  const char *empty_settings = "<TestSettings>\n</TestSettings>\n";
  /* blender-1.2.1's real shape: 5-entry "Blend File" x 2-entry "Compute". */
  const char *two_menus =
    "<TestSettings>\n"
    "<Option><DisplayName>Blend File</DisplayName><Menu>\n"
    "<Entry><Name>BMW27</Name><Value>bmw27</Value></Entry>\n"
    "<Entry><Name>Classroom</Name><Value>classroom</Value></Entry>\n"
    "<Entry><Name>Fishy Cat</Name><Value>fishy_cat</Value></Entry>\n"
    "<Entry><Name>Pabellon Barcelona</Name><Value>pabellon</Value></Entry>\n"
    "<Entry><Name>Barbershop</Name><Value>barbershop</Value></Entry>\n"
    "</Menu></Option>\n"
    "<Option><DisplayName>Compute</DisplayName><Menu>\n"
    "<Entry><Name>CPU-Only</Name><Value>NONE</Value></Entry>\n"
    "<Entry><Name>CUDA</Name><Value>CUDA</Value></Entry>\n"
    "</Menu></Option>\n"
    "</TestSettings>\n";
  /* fio's real shape: one Menu option alongside one free-form (no-Menu) one. */
  const char *menu_plus_freeform =
    "<TestSettings>\n"
    "<Option><DisplayName>Type</DisplayName><Menu>\n"
    "<Entry><Name>Random Read</Name><Value>randread</Value></Entry>\n"
    "<Entry><Name>Random Write</Name><Value>randwrite</Value></Entry>\n"
    "</Menu></Option>\n"
    "<Option><DisplayName>Disk Target</DisplayName><Identifier>auto-disk-mount-points</Identifier>\n"
    "<DefaultEntry>0</DefaultEntry></Option>\n"
    "</TestSettings>\n";

  printf("Testing count_option_combinations...\n");

  has_freeform = 0;
  assert(count_option_combinations(no_settings,&has_freeform) == 1);
  assert(has_freeform == 0);

  has_freeform = 0;
  assert(count_option_combinations(empty_settings,&has_freeform) == 1);
  assert(has_freeform == 0);

  has_freeform = 0;
  assert(count_option_combinations(two_menus,&has_freeform) == 10);
  assert(has_freeform == 0);

  has_freeform = 0;
  assert(count_option_combinations(menu_plus_freeform,&has_freeform) == 2);
  assert(has_freeform == 1); /* the Disk Target option isn't counted -- lower bound */

  printf("PASS: count_option_combinations\n");
}

static void test_load_unavailable_deps(void){
  const char *path = "/tmp/test_ledger_deps.txt";
  char tags[MAX_UNAVAILABLE_TAGS][MAX_DEP_TAG];
  int n;

  printf("Testing load_unavailable_deps...\n");

  write_file(path,
    "steam\n"
    "  vulkan-development  \n"
    "\n"
    "# a whole-line comment\n"
    "opencl  # trailing comment\n");

  n = load_unavailable_deps(path,tags,MAX_UNAVAILABLE_TAGS);
  assert(n == 3);
  assert(!strcmp(tags[0],"steam"));
  assert(!strcmp(tags[1],"vulkan-development"));
  assert(!strcmp(tags[2],"opencl"));

  assert(dep_set_contains(tags,n,"Steam") == 1); /* case-insensitive */
  assert(dep_set_contains(tags,n,"java") == 0);

  assert(load_unavailable_deps("/tmp/test_ledger_deps_missing.txt",tags,MAX_UNAVAILABLE_TAGS) == -1);

  remove(path);
  printf("PASS: load_unavailable_deps\n");
}

/* Writes a fake "<profiles_dir>/<suite>/<dirname>/test-definition.xml" with
 * the given ExternalDependencies text (NULL to omit the field entirely). */
static void make_fake_profile(const char *profiles_dir,const char *suite,const char *dirname,const char *deps){
  char dir[512],path[700];
  char content[1024];

  snprintf(dir,sizeof(dir),"%s/%s/%s",profiles_dir,suite,dirname);
  mkdir_p(dir);
  snprintf(path,sizeof(path),"%s/test-definition.xml",dir);
  if (deps){
    snprintf(content,sizeof(content),
      "<?xml version=\"1.0\"?>\n<PhoronixTestSuite>\n<TestProfile>\n"
      "<ExternalDependencies>%s</ExternalDependencies>\n</TestProfile>\n</PhoronixTestSuite>\n",deps);
  } else {
    snprintf(content,sizeof(content),
      "<?xml version=\"1.0\"?>\n<PhoronixTestSuite>\n<TestProfile>\n</TestProfile>\n</PhoronixTestSuite>\n");
  }
  write_file(path,content);
}

static void test_scan_phoronix_dependencies(void){
  const char *profiles_dir = "/tmp/test_ledger_profiles";
  const char *list_path = "/tmp/test_ledger_dep_list.txt";
  const char *index_path = "/tmp/test_ledger_dep_index.jsonl";
  char tags[MAX_UNAVAILABLE_TAGS][MAX_DEP_TAG];
  struct ledger_entry *entries;
  int n,ntags;
  struct ledger_entry *e;
  FILE *fp;

  printf("Testing scan_phoronix_dependencies...\n");

  rmdir_recursive(profiles_dir);
  make_fake_profile(profiles_dir,"pts","dirt-rally2-1.0.3","steam");
  make_fake_profile(profiles_dir,"pts","cs2-1.0.2","steam, vulkan-development");
  make_fake_profile(profiles_dir,"pts","scikit-learn-1.0.1","python-scipy, python-sklearn, python");
  make_fake_profile(profiles_dir,"pts","no-deps-1.0.0",NULL);

  snprintf(tags[0],MAX_DEP_TAG,"steam");
  ntags = 1;

  /* dirt-rally2: unannotated, no runs -- should pick up the dep scan.
   * cs2-hand-annotated: explicit annotation must not be overridden.
   * cs2-succeeded: a proven successful run must not be overridden either,
   * even though its own profile also needs steam.
   * scikit-learn: has ExternalDependencies, but none are in the unavailable
   * set, so it should be untouched. */
  write_file(list_path,
    "dirt-rally2\n"
    "cs2-hand-annotated\tunsupported\thand annotated already\n"
    "cs2-succeeded\n"
    "scikit-learn\n");

  fp = fopen(index_path,"w");
  assert(fp != NULL);
  /* cs2-succeeded matches profile dir "cs2-1.0.2" only via its own
   * ledger-list *name*, not the run's command line -- command_matches()
   * only needs the name to appear as a substring, so "cs2-succeeded"
   * itself never has to appear in the profile scan; give it a clean run
   * under its own name so it resolves to DONE regardless of dep_unavailable. */
  fprintf(fp,"{\"schema_version\":\"1.0.0\",\"run_id\":\"r1\",\"start_time\":\"2026-01-01T00:00:00Z\","
              "\"command\":[\"phoronix-test-suite\",\"run\",\"cs2-succeeded\"],"
              "\"exit_status\":{\"known\":true,\"exited\":true,\"signaled\":false,\"exit_code\":0}}\n");
  fclose(fp);

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 4);
  assert(process_run_index_file(index_path,entries,n) == 1);

  /* cs2-succeeded's own profile dir ("cs2-1.0.2") won't literally match its
   * ledger name ("cs2-succeeded") by strip_version_suffix, so give the scan
   * a dedicated profile dir under that exact name too. */
  make_fake_profile(profiles_dir,"pts","cs2-succeeded-2.0.0","steam");

  scan_phoronix_dependencies(profiles_dir,tags,ntags,entries,n);

  e = find_entry(entries,n,"dirt-rally2");
  assert(e->dep_unavailable == 1);
  assert(strstr(e->dep_note,"steam") != NULL);
  assert(strstr(e->dep_note,"pts/dirt-rally2-1.0.3") != NULL);
  assert(entry_status(e) == LEDGER_UNSUPPORTED);

  e = find_entry(entries,n,"cs2-hand-annotated");
  assert(e->dep_unavailable == 0); /* explicit annotation short-circuits the scan match */
  assert(entry_status(e) == LEDGER_UNSUPPORTED);
  {
    char detail[MAX_NOTE + 160];
    format_detail(e,entry_status(e),detail,sizeof(detail));
    assert(!strcmp(detail,"hand annotated already"));
  }

  e = find_entry(entries,n,"cs2-succeeded");
  assert(e->dep_unavailable == 1); /* the scan still matched it... */
  assert(entry_status(e) == LEDGER_DONE); /* ...but a real success outranks it */

  e = find_entry(entries,n,"scikit-learn");
  assert(e->dep_unavailable == 0); /* its deps exist, just none are "unavailable" */
  assert(entry_status(e) == LEDGER_SKIPPED);

  free(entries);
  remove(list_path);
  remove(index_path);
  rmdir_recursive(profiles_dir);
  printf("PASS: scan_phoronix_dependencies\n");
}

/* Writes a fake "<profiles_dir>/<suite>/<dirname>/test-definition.xml" whose
 * <TestSettings> is exactly settings_xml (NULL to omit <TestSettings>
 * entirely, i.e. a one-combination profile). */
static void make_fake_option_profile(const char *profiles_dir,const char *suite,const char *dirname,
                                      const char *settings_xml){
  char dir[512],path[700];
  char content[2048];

  snprintf(dir,sizeof(dir),"%s/%s/%s",profiles_dir,suite,dirname);
  mkdir_p(dir);
  snprintf(path,sizeof(path),"%s/test-definition.xml",dir);
  snprintf(content,sizeof(content),
    "<?xml version=\"1.0\"?>\n<PhoronixTestSuite>\n<TestProfile>\n</TestProfile>\n%s</PhoronixTestSuite>\n",
    settings_xml ? settings_xml : "");
  write_file(path,content);
}

static void test_scan_phoronix_option_combinations(void){
  const char *profiles_dir = "/tmp/test_ledger_combo_profiles";
  const char *list_path = "/tmp/test_ledger_combo_list.txt";
  const char *two_option_menu =
    "<TestSettings>\n"
    "<Option><Menu><Entry><Name>A</Name><Value>a</Value></Entry>"
    "<Entry><Name>B</Name><Value>b</Value></Entry></Menu></Option>\n"
    "<Option><Menu><Entry><Name>X</Name><Value>x</Value></Entry>"
    "<Entry><Name>Y</Name><Value>y</Value></Entry>"
    "<Entry><Name>Z</Name><Value>z</Value></Entry></Menu></Option>\n"
    "</TestSettings>\n";
  const char *five_entry_menu =
    "<TestSettings>\n"
    "<Option><Menu>"
    "<Entry><Name>1</Name><Value>1</Value></Entry>"
    "<Entry><Name>2</Name><Value>2</Value></Entry>"
    "<Entry><Name>3</Name><Value>3</Value></Entry>"
    "<Entry><Name>4</Name><Value>4</Value></Entry>"
    "<Entry><Name>5</Name><Value>5</Value></Entry>"
    "</Menu></Option>\n"
    "</TestSettings>\n";
  struct ledger_entry *entries;
  struct ledger_entry *e;
  int n;

  printf("Testing scan_phoronix_option_combinations...\n");

  rmdir_recursive(profiles_dir);
  /* some-test: 2x3=6 combinations. */
  make_fake_option_profile(profiles_dir,"pts","some-test-1.0.0",two_option_menu);
  /* no-options-test: no <TestSettings> at all -- 1 combination. */
  make_fake_option_profile(profiles_dir,"pts","no-options-test-1.0.0",NULL);
  /* ambiguous-test: two profile dirs (different suites) disagree on count. */
  make_fake_option_profile(profiles_dir,"pts","ambiguous-test-1.0.0",two_option_menu);
  make_fake_option_profile(profiles_dir,"system","ambiguous-test-1.0.0",five_entry_menu);

  write_file(list_path,
    "some-test\n"
    "no-options-test\n"
    "ambiguous-test\n"
    "unmatched-test\n");

  entries = load_workload_list(list_path,&n);
  assert(entries != NULL && n == 4);

  scan_phoronix_option_combinations(profiles_dir,entries,n);

  e = find_entry(entries,n,"some-test");
  assert(e->combo_known == 1);
  assert(e->combo_count == 6);
  assert(e->combo_has_freeform == 0);
  assert(e->combo_ambiguous == 0);
  assert(strstr(e->combo_profile,"some-test-1.0.0") != NULL);

  e = find_entry(entries,n,"no-options-test");
  assert(e->combo_known == 1);
  assert(e->combo_count == 1);

  e = find_entry(entries,n,"ambiguous-test");
  assert(e->combo_known == 1);
  assert(e->combo_ambiguous == 1); /* 6 vs 5 -- disagreement flagged, not silently picked */

  e = find_entry(entries,n,"unmatched-test");
  assert(e->combo_known == 0); /* no matching profile dir at all */

  free(entries);
  remove(list_path);
  rmdir_recursive(profiles_dir);
  printf("PASS: scan_phoronix_option_combinations\n");
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
  test_strip_version_suffix();
  test_extract_external_dependencies();
  test_count_option_combinations();
  test_load_unavailable_deps();
  test_scan_phoronix_dependencies();
  test_scan_phoronix_option_combinations();

  printf("\nAll test_ledger tests passed.\n");
  return 0;
}
