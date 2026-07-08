/*
 * json_util.h - small JSON-emission helpers shared by manifest.c and
 * run_index.c (both write hand-rolled JSON, not through a library).
 */
#ifndef _WSPY_JSON_UTIL_H
#define _WSPY_JSON_UTIL_H 1

#include <stdio.h>
#include <time.h>

/* Escape and quote a possibly-NULL string as a JSON string literal,
 * including the surrounding double quotes. */
void json_write_string(FILE *fp,const char *s);

/* Format ts as an ISO-8601 UTC timestamp with millisecond precision
 * (e.g. "2026-07-08T15:30:12.345Z") into buf. */
void format_iso8601(const struct timespec *ts,char *buf,size_t bufsize);

#endif
