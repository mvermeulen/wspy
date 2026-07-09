/*
 * json_reader.c - recursive-descent JSON parser, read-side counterpart to
 * json_util.c. See json_reader.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include "json_reader.h"

struct parser {
  const char *p;
  int line;
  char *errbuf;
  size_t errbuf_size;
  int failed;
};

static void set_error(struct parser *ps,const char *fmt,...){
  va_list ap;
  if (ps->failed) return; /* keep the first error, it's usually the useful one */
  ps->failed = 1;
  if (!ps->errbuf || ps->errbuf_size == 0) return;
  va_start(ap,fmt);
  vsnprintf(ps->errbuf,ps->errbuf_size,fmt,ap);
  va_end(ap);
}

static void skip_ws(struct parser *ps){
  while (*ps->p){
    if (*ps->p == '\n'){ ps->line++; ps->p++; }
    else if (isspace((unsigned char)*ps->p)) ps->p++;
    else break;
  }
}

static struct json_value *new_value(enum json_type type){
  struct json_value *v = calloc(1,sizeof(*v));
  v->type = type;
  return v;
}

static struct json_value *parse_value(struct parser *ps);

static int hex_digit(char c){
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

/* Parses a JSON string literal (the opening quote must be the current
 * character). Supports the escapes json_write_string() emits plus \/ and
 * \uXXXX (encoded to UTF-8; surrogate pairs are not decoded -- manifest.json
 * never contains characters outside the BMP that would need one). */
static char *parse_string_raw(struct parser *ps){
  const char *start,*s;
  char *out,*o;
  size_t cap;

  if (*ps->p != '"'){
    set_error(ps,"line %d: expected '\"'",ps->line);
    return NULL;
  }
  ps->p++;
  start = ps->p;
  cap = 0;
  for (s = start; *s && *s != '"'; s++){
    if (*s == '\\' && s[1]) s++;
    cap++;
  }
  out = malloc(cap * 4 + 1); /* worst case: every char is a 4-byte UTF-8 sequence */
  o = out;
  while (*ps->p && *ps->p != '"'){
    unsigned char c = (unsigned char)*ps->p;
    if (c == '\\'){
      ps->p++;
      switch (*ps->p){
      case '"': *o++ = '"'; ps->p++; break;
      case '\\': *o++ = '\\'; ps->p++; break;
      case '/': *o++ = '/'; ps->p++; break;
      case 'b': *o++ = '\b'; ps->p++; break;
      case 'f': *o++ = '\f'; ps->p++; break;
      case 'n': *o++ = '\n'; ps->p++; break;
      case 'r': *o++ = '\r'; ps->p++; break;
      case 't': *o++ = '\t'; ps->p++; break;
      case 'u': {
        int cp,d0,d1,d2,d3;
        ps->p++;
        d0 = hex_digit(ps->p[0]); d1 = hex_digit(ps->p[1]);
        d2 = hex_digit(ps->p[2]); d3 = hex_digit(ps->p[3]);
        if (d0 < 0 || d1 < 0 || d2 < 0 || d3 < 0){
          set_error(ps,"line %d: invalid \\u escape",ps->line);
          free(out);
          return NULL;
        }
        cp = (d0 << 12) | (d1 << 8) | (d2 << 4) | d3;
        ps->p += 4;
        if (cp < 0x80){
          *o++ = (char)cp;
        } else if (cp < 0x800){
          *o++ = (char)(0xc0 | (cp >> 6));
          *o++ = (char)(0x80 | (cp & 0x3f));
        } else {
          *o++ = (char)(0xe0 | (cp >> 12));
          *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
          *o++ = (char)(0x80 | (cp & 0x3f));
        }
        break;
      }
      default:
        set_error(ps,"line %d: invalid escape '\\%c'",ps->line,*ps->p);
        free(out);
        return NULL;
      }
    } else {
      if (c == '\n') ps->line++;
      *o++ = *ps->p++;
    }
  }
  if (*ps->p != '"'){
    set_error(ps,"line %d: unterminated string",ps->line);
    free(out);
    return NULL;
  }
  ps->p++;
  *o = '\0';
  return out;
}

static struct json_value *parse_string(struct parser *ps){
  struct json_value *v;
  char *s = parse_string_raw(ps);
  if (!s) return NULL;
  v = new_value(JSON_STRING);
  v->u.string = s;
  return v;
}

static struct json_value *parse_number(struct parser *ps){
  const char *start = ps->p;
  char *endptr;
  double n;
  struct json_value *v;

  if (*ps->p == '-') ps->p++;
  if (!isdigit((unsigned char)*ps->p)){
    set_error(ps,"line %d: invalid number",ps->line);
    return NULL;
  }
  n = strtod(start,&endptr);
  if (endptr == start){
    set_error(ps,"line %d: invalid number",ps->line);
    return NULL;
  }
  ps->p = endptr;
  v = new_value(JSON_NUMBER);
  v->u.number = n;
  return v;
}

static struct json_value *parse_literal(struct parser *ps){
  struct json_value *v;
  if (!strncmp(ps->p,"true",4)){
    ps->p += 4;
    v = new_value(JSON_BOOL);
    v->u.boolean = 1;
    return v;
  }
  if (!strncmp(ps->p,"false",5)){
    ps->p += 5;
    v = new_value(JSON_BOOL);
    v->u.boolean = 0;
    return v;
  }
  if (!strncmp(ps->p,"null",4)){
    ps->p += 4;
    return new_value(JSON_NULL);
  }
  set_error(ps,"line %d: unexpected character '%c'",ps->line,*ps->p);
  return NULL;
}

static struct json_value *parse_array(struct parser *ps){
  struct json_value *v = new_value(JSON_ARRAY);
  size_t cap = 0;

  ps->p++; /* consume '[' */
  skip_ws(ps);
  if (*ps->p == ']'){
    ps->p++;
    return v;
  }
  for (;;){
    struct json_value *item;
    skip_ws(ps);
    item = parse_value(ps);
    if (!item || ps->failed){ json_free(v); return NULL; }
    if (v->u.array.count == cap){
      cap = cap ? cap * 2 : 4;
      v->u.array.items = realloc(v->u.array.items,cap * sizeof(*v->u.array.items));
    }
    v->u.array.items[v->u.array.count++] = item;
    skip_ws(ps);
    if (*ps->p == ','){ ps->p++; continue; }
    if (*ps->p == ']'){ ps->p++; break; }
    set_error(ps,"line %d: expected ',' or ']' in array",ps->line);
    json_free(v);
    return NULL;
  }
  return v;
}

static struct json_value *parse_object(struct parser *ps){
  struct json_value *v = new_value(JSON_OBJECT);
  size_t cap = 0;

  ps->p++; /* consume '{' */
  skip_ws(ps);
  if (*ps->p == '}'){
    ps->p++;
    return v;
  }
  for (;;){
    char *key;
    struct json_value *val;

    skip_ws(ps);
    key = parse_string_raw(ps);
    if (!key){ json_free(v); return NULL; }
    skip_ws(ps);
    if (*ps->p != ':'){
      set_error(ps,"line %d: expected ':' after object key",ps->line);
      free(key);
      json_free(v);
      return NULL;
    }
    ps->p++;
    skip_ws(ps);
    val = parse_value(ps);
    if (!val || ps->failed){
      free(key);
      json_free(v);
      return NULL;
    }
    if (v->u.object.count == cap){
      cap = cap ? cap * 2 : 4;
      v->u.object.keys = realloc(v->u.object.keys,cap * sizeof(*v->u.object.keys));
      v->u.object.values = realloc(v->u.object.values,cap * sizeof(*v->u.object.values));
    }
    v->u.object.keys[v->u.object.count] = key;
    v->u.object.values[v->u.object.count] = val;
    v->u.object.count++;
    skip_ws(ps);
    if (*ps->p == ','){ ps->p++; continue; }
    if (*ps->p == '}'){ ps->p++; break; }
    set_error(ps,"line %d: expected ',' or '}' in object",ps->line);
    json_free(v);
    return NULL;
  }
  return v;
}

static struct json_value *parse_value(struct parser *ps){
  skip_ws(ps);
  switch (*ps->p){
  case '{': return parse_object(ps);
  case '[': return parse_array(ps);
  case '"': return parse_string(ps);
  case 't': case 'f': case 'n': return parse_literal(ps);
  case '\0':
    set_error(ps,"line %d: unexpected end of input",ps->line);
    return NULL;
  default:
    if (*ps->p == '-' || isdigit((unsigned char)*ps->p)) return parse_number(ps);
    set_error(ps,"line %d: unexpected character '%c'",ps->line,*ps->p);
    return NULL;
  }
}

struct json_value *json_parse(const char *text,char *errbuf,size_t errbuf_size){
  struct parser ps;
  struct json_value *v;

  if (errbuf && errbuf_size) errbuf[0] = '\0';
  ps.p = text;
  ps.line = 1;
  ps.errbuf = errbuf;
  ps.errbuf_size = errbuf_size;
  ps.failed = 0;

  v = parse_value(&ps);
  if (!v || ps.failed){
    if (v) json_free(v);
    return NULL;
  }
  skip_ws(&ps);
  if (*ps.p != '\0'){
    set_error(&ps,"line %d: trailing data after JSON value",ps.line);
    json_free(v);
    return NULL;
  }
  return v;
}

struct json_value *json_parse_file(const char *path,char *errbuf,size_t errbuf_size){
  FILE *fp;
  char *buf;
  long size;
  struct json_value *v;

  fp = fopen(path,"r");
  if (!fp){
    if (errbuf && errbuf_size) snprintf(errbuf,errbuf_size,"unable to open file: %s",strerror(errno));
    return NULL;
  }
  fseek(fp,0,SEEK_END);
  size = ftell(fp);
  if (size < 0){
    if (errbuf && errbuf_size) snprintf(errbuf,errbuf_size,"unable to determine file size");
    fclose(fp);
    return NULL;
  }
  fseek(fp,0,SEEK_SET);
  buf = malloc((size_t)size + 1);
  if (fread(buf,1,(size_t)size,fp) != (size_t)size){
    if (errbuf && errbuf_size) snprintf(errbuf,errbuf_size,"short read while loading file");
    free(buf);
    fclose(fp);
    return NULL;
  }
  buf[size] = '\0';
  fclose(fp);

  v = json_parse(buf,errbuf,errbuf_size);
  free(buf);
  return v;
}

void json_free(struct json_value *v){
  size_t i;

  if (!v) return;
  switch (v->type){
  case JSON_STRING:
    free(v->u.string);
    break;
  case JSON_ARRAY:
    for (i = 0; i < v->u.array.count; i++) json_free(v->u.array.items[i]);
    free(v->u.array.items);
    break;
  case JSON_OBJECT:
    for (i = 0; i < v->u.object.count; i++){
      free(v->u.object.keys[i]);
      json_free(v->u.object.values[i]);
    }
    free(v->u.object.keys);
    free(v->u.object.values);
    break;
  default:
    break;
  }
  free(v);
}

const struct json_value *json_object_get(const struct json_value *obj,const char *key){
  size_t i;
  if (!obj || obj->type != JSON_OBJECT) return NULL;
  for (i = 0; i < obj->u.object.count; i++){
    if (!strcmp(obj->u.object.keys[i],key)) return obj->u.object.values[i];
  }
  return NULL;
}

const struct json_value *json_array_get(const struct json_value *arr,size_t index){
  if (!arr || arr->type != JSON_ARRAY || index >= arr->u.array.count) return NULL;
  return arr->u.array.items[index];
}

size_t json_array_len(const struct json_value *arr){
  if (!arr || arr->type != JSON_ARRAY) return 0;
  return arr->u.array.count;
}

const char *json_get_string(const struct json_value *obj,const char *key,const char *default_value){
  const struct json_value *v = json_object_get(obj,key);
  if (!v || v->type != JSON_STRING) return default_value;
  return v->u.string;
}

double json_get_number(const struct json_value *obj,const char *key,double default_value){
  const struct json_value *v = json_object_get(obj,key);
  if (!v || v->type != JSON_NUMBER) return default_value;
  return v->u.number;
}

int json_get_bool(const struct json_value *obj,const char *key,int default_value){
  const struct json_value *v = json_object_get(obj,key);
  if (!v || v->type != JSON_BOOL) return default_value;
  return v->u.boolean;
}
