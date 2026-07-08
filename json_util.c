/*
 * json_util.c - small JSON-emission helpers shared by manifest.c and
 * run_index.c
 */
#include "json_util.h"

void json_write_string(FILE *fp,const char *s){
  const unsigned char *p;

  fputc('"',fp);
  for (p = (const unsigned char *)(s ? s : ""); *p; p++){
    switch (*p){
    case '"':  fputs("\\\"",fp); break;
    case '\\': fputs("\\\\",fp); break;
    case '\n': fputs("\\n",fp); break;
    case '\r': fputs("\\r",fp); break;
    case '\t': fputs("\\t",fp); break;
    default:
      if (*p < 0x20) fprintf(fp,"\\u%04x",*p);
      else fputc(*p,fp);
    }
  }
  fputc('"',fp);
}

void format_iso8601(const struct timespec *ts,char *buf,size_t bufsize){
  struct tm tm_utc;
  size_t n;
  int ms;

  gmtime_r(&ts->tv_sec,&tm_utc);
  n = strftime(buf,bufsize,"%Y-%m-%dT%H:%M:%S",&tm_utc);
  ms = (int)(ts->tv_nsec / 1000000);
  if (n < bufsize) snprintf(buf+n,bufsize-n,".%03dZ",ms);
}
