#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define TEST_VALIDATE 1
#include "validate.c"

static void write_file(const char *path,const char *content){
  FILE *fp = fopen(path,"w");
  assert(fp != NULL);
  fputs(content,fp);
  fclose(fp);
}

static void test_json_reader_roundtrip(void){
  const char *text =
    "{\n"
    "  \"name\": \"hello \\\"world\\\"\",\n"
    "  \"count\": 42,\n"
    "  \"ratio\": -3.5,\n"
    "  \"enabled\": true,\n"
    "  \"disabled\": false,\n"
    "  \"nothing\": null,\n"
    "  \"tags\": [\"a\", \"b\", \"c\"],\n"
    "  \"nested\": { \"x\": 1, \"y\": [true, false] }\n"
    "}\n";
  struct json_value *root;
  char errbuf[256];
  const struct json_value *nested,*tags,*y;

  printf("Testing json_reader round-trip...\n");

  root = json_parse(text,errbuf,sizeof(errbuf));
  assert(root != NULL);
  assert(root->type == JSON_OBJECT);

  assert(!strcmp(json_get_string(root,"name","?"),"hello \"world\""));
  assert(json_get_number(root,"count",-1) == 42);
  assert(json_get_number(root,"ratio",0) == -3.5);
  assert(json_get_bool(root,"enabled",0) == 1);
  assert(json_get_bool(root,"disabled",1) == 0);
  assert(json_object_get(root,"nothing")->type == JSON_NULL);
  assert(!strcmp(json_get_string(root,"missing","default"),"default"));

  tags = json_object_get(root,"tags");
  assert(json_array_len(tags) == 3);
  assert(!strcmp(json_array_get(tags,0)->u.string,"a"));
  assert(!strcmp(json_array_get(tags,2)->u.string,"c"));
  assert(json_array_get(tags,3) == NULL);

  nested = json_object_get(root,"nested");
  assert(json_get_number(nested,"x",-1) == 1);
  y = json_object_get(nested,"y");
  assert(json_array_len(y) == 2);

  json_free(root);
  printf("PASS: json_reader round-trip\n");
}

static void test_json_reader_malformed(void){
  char errbuf[256];
  struct json_value *v;

  printf("Testing json_reader malformed input...\n");

  errbuf[0] = '\0';
  v = json_parse("{\"a\": }",errbuf,sizeof(errbuf));
  assert(v == NULL);
  assert(errbuf[0] != '\0');

  errbuf[0] = '\0';
  v = json_parse("{\"a\": 1,}",errbuf,sizeof(errbuf));
  assert(v == NULL);

  errbuf[0] = '\0';
  v = json_parse("[1, 2",errbuf,sizeof(errbuf));
  assert(v == NULL);

  errbuf[0] = '\0';
  v = json_parse("42 garbage",errbuf,sizeof(errbuf));
  assert(v == NULL);

  printf("PASS: json_reader malformed input\n");
}

static void test_parse_numeric_field(void){
  double num;
  int is_pct;

  printf("Testing parse_numeric_field...\n");

  assert(parse_numeric_field("1.00",&num,&is_pct) == 1);
  assert(num == 1.00 && is_pct == 0);

  assert(parse_numeric_field("26.61%",&num,&is_pct) == 1);
  assert(num > 26.6 && num < 26.62 && is_pct == 1);

  assert(parse_numeric_field("",&num,&is_pct) == 0);
  assert(parse_numeric_field("net0",&num,&is_pct) == 0);
  assert(parse_numeric_field("-5",&num,&is_pct) == 1);
  assert(num == -5 && is_pct == 0);

  printf("PASS: parse_numeric_field\n");
}

static void test_split_csv_line(void){
  char line[] = "0.41,1,26.61%,,3,";
  char *fields[MAX_CSV_FIELDS];
  int n;

  printf("Testing split_csv_line...\n");

  n = split_csv_line(line,fields,MAX_CSV_FIELDS);
  assert(n == 6);
  assert(!strcmp(fields[0],"0.41"));
  assert(!strcmp(fields[1],"1"));
  assert(!strcmp(fields[2],"26.61%"));
  assert(!strcmp(fields[3],""));
  assert(!strcmp(fields[4],"3"));
  assert(!strcmp(fields[5],""));

  printf("PASS: split_csv_line\n");
}

/* Builds a minimal but realistic manifest.json referencing csv_path, with
 * exit_code/coverage plugged in so each test case can push the checks down
 * a specific path without hand-writing the whole manifest shape each time. */
static void write_test_manifest(const char *manifest_path,const char *csv_path,
                                 int exit_code,int counters_requested,int counters_measured){
  char buf[2048];
  snprintf(buf,sizeof(buf),
    "{\n"
    "  \"schema_version\": \"%s\",\n"
    "  \"timing\": { \"elapsed_seconds\": 0.5 },\n"
    "  \"exit_status\": { \"known\": true, \"exited\": true, \"exit_code\": %d, \"signaled\": false, \"term_signal\": null },\n"
    "  \"options\": { \"csv\": true },\n"
    "  \"counter_coverage\": { \"requested\": %d, \"measured\": %d, \"unavailable\": [] },\n"
    "  \"output_files\": [ { \"kind\": \"output\", \"path\": \"%s\" } ]\n"
    "}\n",
    MANIFEST_SCHEMA_VERSION,exit_code,counters_requested,counters_measured,csv_path);
  write_file(manifest_path,buf);
}

static void test_validate_manifest_pass(void){
  const char *manifest_path = "/tmp/test_validate_pass.manifest.json";
  const char *csv_path = "/tmp/test_validate_pass.csv";

  printf("Testing validate_manifest: clean run passes...\n");

  write_file(csv_path,"elapsed,ipc,cpu\n0.5,1.25,26.61%\n");
  write_test_manifest(manifest_path,csv_path,0,3,3);

  assert(validate_manifest(manifest_path,1) == SEV_PASS);

  remove(manifest_path);
  remove(csv_path);
  printf("PASS: validate_manifest clean run\n");
}

static void test_validate_manifest_missing_file(void){
  const char *manifest_path = "/tmp/test_validate_missing.manifest.json";

  printf("Testing validate_manifest: missing output file fails...\n");

  write_test_manifest(manifest_path,"/tmp/test_validate_does_not_exist.csv",0,3,3);
  assert(validate_manifest(manifest_path,1) == SEV_FAIL);

  remove(manifest_path);
  printf("PASS: validate_manifest missing output file\n");
}

static void test_validate_manifest_bad_exit(void){
  const char *manifest_path = "/tmp/test_validate_exit.manifest.json";
  const char *csv_path = "/tmp/test_validate_exit.csv";

  printf("Testing validate_manifest: nonzero exit code fails...\n");

  write_file(csv_path,"elapsed,ipc\n0.5,1.25\n");
  write_test_manifest(manifest_path,csv_path,1,3,3);

  assert(validate_manifest(manifest_path,1) == SEV_FAIL);

  remove(manifest_path);
  remove(csv_path);
  printf("PASS: validate_manifest nonzero exit code\n");
}

static void test_validate_manifest_sanity_fail(void){
  const char *manifest_path = "/tmp/test_validate_sanity.manifest.json";
  const char *csv_path = "/tmp/test_validate_sanity.csv";

  printf("Testing validate_manifest: out-of-range ipc fails sanity check...\n");

  write_file(csv_path,"elapsed,ipc\n0.5,999.0\n");
  write_test_manifest(manifest_path,csv_path,0,3,3);

  assert(validate_manifest(manifest_path,1) == SEV_FAIL);

  remove(manifest_path);
  remove(csv_path);
  printf("PASS: validate_manifest sanity range\n");
}

static void test_validate_manifest_empty_csv_fail(void){
  const char *manifest_path = "/tmp/test_validate_empty.manifest.json";
  const char *csv_path = "/tmp/test_validate_empty.csv";

  printf("Testing validate_manifest: empty CSV fails...\n");

  write_file(csv_path,"");
  write_test_manifest(manifest_path,csv_path,0,3,3);

  assert(validate_manifest(manifest_path,1) == SEV_FAIL);

  remove(manifest_path);
  remove(csv_path);
  printf("PASS: validate_manifest empty CSV\n");
}

static void test_validate_manifest_partial_coverage_warns(void){
  const char *manifest_path = "/tmp/test_validate_warn.manifest.json";
  const char *csv_path = "/tmp/test_validate_warn.csv";

  printf("Testing validate_manifest: partial coverage warns, not fails...\n");

  write_file(csv_path,"elapsed,ipc\n0.5,1.25\n");
  write_test_manifest(manifest_path,csv_path,0,3,1);

  assert(validate_manifest(manifest_path,1) == SEV_WARN);

  remove(manifest_path);
  remove(csv_path);
  printf("PASS: validate_manifest partial coverage warning\n");
}

static void test_validate_manifest_malformed_json(void){
  const char *manifest_path = "/tmp/test_validate_malformed.manifest.json";

  printf("Testing validate_manifest: unparseable manifest fails...\n");

  write_file(manifest_path,"{ this is not json");
  assert(validate_manifest(manifest_path,1) == SEV_FAIL);

  remove(manifest_path);
  printf("PASS: validate_manifest malformed json\n");
}

int main(void){
  test_json_reader_roundtrip();
  test_json_reader_malformed();
  test_parse_numeric_field();
  test_split_csv_line();
  test_validate_manifest_pass();
  test_validate_manifest_missing_file();
  test_validate_manifest_bad_exit();
  test_validate_manifest_sanity_fail();
  test_validate_manifest_empty_csv_fail();
  test_validate_manifest_partial_coverage_warns();
  test_validate_manifest_malformed_json();

  printf("\nAll test_validate tests passed.\n");
  return 0;
}
