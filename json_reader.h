/*
 * json_reader.h - small JSON-parsing helpers, the read-side counterpart to
 * json_util.c's write-side helpers. Written for wspy-validate to parse the
 * manifest.json documents manifest.c writes (see manifest.h), but not tied
 * to that shape -- a generic-enough recursive-descent parser for a JSON
 * value tree, not a schema-specific reader.
 */
#ifndef _WSPY_JSON_READER_H
#define _WSPY_JSON_READER_H 1

#include <stddef.h>

enum json_type {
  JSON_NULL,
  JSON_BOOL,
  JSON_NUMBER,
  JSON_STRING,
  JSON_ARRAY,
  JSON_OBJECT
};

struct json_value {
  enum json_type type;
  union {
    int boolean;
    double number;
    char *string;
    struct {
      struct json_value **items;
      size_t count;
    } array;
    struct {
      char **keys;
      struct json_value **values;
      size_t count;
    } object;
  } u;
};

/* Parses a NUL-terminated JSON document in text. Returns the root value on
 * success, or NULL on a parse error -- when non-NULL, errbuf receives a
 * short human-readable description of what went wrong (line number, what
 * was expected); errbuf is untouched on success. Caller owns the return
 * value and must free it with json_free(). */
struct json_value *json_parse(const char *text,char *errbuf,size_t errbuf_size);

/* Reads path into memory and parses it via json_parse(). Returns NULL both
 * when the file can't be read and when it fails to parse -- errbuf
 * distinguishes the two ("unable to open file: ..." vs a parse error). */
struct json_value *json_parse_file(const char *path,char *errbuf,size_t errbuf_size);

void json_free(struct json_value *v);

/* Accessors. All return NULL/0/default on type mismatch or missing key/
 * index rather than asserting -- callers validating an untrusted or
 * evolving document should treat "field absent or wrong shape" as a normal
 * outcome, not a crash. */
const struct json_value *json_object_get(const struct json_value *obj,const char *key);
const struct json_value *json_array_get(const struct json_value *arr,size_t index);
size_t json_array_len(const struct json_value *arr);

/* Convenience lookups combining json_object_get() with a type check and a
 * default value, for the common case of reading one named scalar field. */
const char *json_get_string(const struct json_value *obj,const char *key,const char *default_value);
double json_get_number(const struct json_value *obj,const char *key,double default_value);
int json_get_bool(const struct json_value *obj,const char *key,int default_value);

#endif
