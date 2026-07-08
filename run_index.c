/*
 * run_index.c - append one JSONL record per run to a shared run index file,
 * described in run_index.h.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include "run_index.h"
#include "wspy.h"
#include "error.h"
#include "json_util.h"

/* Sortable, unique-enough-per-host run identifier: start timestamp (to
 * millisecond resolution) plus pid, so two runs started in the same second
 * (or even the same millisecond, by different processes) still get
 * distinct ids. Not guaranteed globally unique across hosts -- combine with
 * the record's "hostname" field for that. */
static void format_run_id(const struct timespec *start_time,char *buf,size_t bufsize){
  struct tm tm_utc;
  char stamp[32];

  gmtime_r(&start_time->tv_sec,&tm_utc);
  strftime(stamp,sizeof(stamp),"%Y%m%dT%H%M%S",&tm_utc);
  snprintf(buf,bufsize,"%s.%03ld-%d",stamp,start_time->tv_nsec/1000000,(int)getpid());
}

static void json_write_string_or_null(FILE *fp,const char *s){
  if (s) json_write_string(fp,s);
  else fputs("null",fp);
}

int append_run_index(const char *path,const struct manifest_info *info){
  FILE *fp;
  int i;
  char start_buf[40],finish_buf[40];
  char run_id[64];
  char hostname[256];
  char wspy_version[16];
  double elapsed;

  fp = fopen(path,"a");
  if (!fp){
    warning("unable to open run index file: %s\n",path);
    return -1;
  }
  if (flock(fileno(fp),LOCK_EX) != 0){
    warning("unable to lock run index file: %s\n",path);
    fclose(fp);
    return -1;
  }

  format_iso8601(&info->start_time,start_buf,sizeof(start_buf));
  format_iso8601(&info->finish_time,finish_buf,sizeof(finish_buf));
  format_run_id(&info->start_time,run_id,sizeof(run_id));
  elapsed = (info->finish_time.tv_sec - info->start_time.tv_sec) +
    (info->finish_time.tv_nsec - info->start_time.tv_nsec) / 1000000000.0;

  if (gethostname(hostname,sizeof(hostname)) != 0){
    strcpy(hostname,"unknown");
  }
  hostname[sizeof(hostname)-1] = '\0';

  snprintf(wspy_version,sizeof(wspy_version),"%d.%d",WSPY_VERSION_MAJOR,WSPY_VERSION_MINOR);

  fprintf(fp,"{");
  fprintf(fp,"\"schema_version\":\"%s\",",RUN_INDEX_SCHEMA_VERSION);
  fprintf(fp,"\"run_id\":\"%s\",",run_id);
  fprintf(fp,"\"wspy_version\":\"%s\",",wspy_version);
  fprintf(fp,"\"hostname\":");
  json_write_string(fp,hostname);
  fprintf(fp,",");
  fprintf(fp,"\"cpu_vendor\":");
  json_write_string(fp,cpu_vendor_name(cpu_info->vendor));
  fprintf(fp,",");
  fprintf(fp,"\"cpu_family\":%u,",cpu_info->family);
  fprintf(fp,"\"cpu_model\":%u,",cpu_info->model);
  fprintf(fp,"\"start_time\":\"%s\",",start_buf);
  fprintf(fp,"\"finish_time\":\"%s\",",finish_buf);
  fprintf(fp,"\"elapsed_seconds\":%.3f,",elapsed);

  fprintf(fp,"\"command\":[");
  for (i = 0; i < info->argc; i++){
    if (i) fprintf(fp,",");
    json_write_string(fp,info->argv[i]);
  }
  fprintf(fp,"],");

  fprintf(fp,"\"exit_status\":{\"known\":%s",info->exit_status.known ? "true" : "false");
  if (info->exit_status.known){
    fprintf(fp,",\"exited\":%s",info->exit_status.exited ? "true" : "false");
    if (info->exit_status.exited) fprintf(fp,",\"exit_code\":%d",info->exit_status.exit_code);
    else fprintf(fp,",\"exit_code\":null");
    fprintf(fp,",\"signaled\":%s",info->exit_status.signaled ? "true" : "false");
    if (info->exit_status.signaled) fprintf(fp,",\"term_signal\":%d",info->exit_status.term_signal);
    else fprintf(fp,",\"term_signal\":null");
  } else {
    fprintf(fp,",\"exited\":null,\"exit_code\":null,\"signaled\":null,\"term_signal\":null");
  }
  fprintf(fp,"},");

  fprintf(fp,"\"options\":{\"counter_mask\":\"0x%x\",\"per_core\":%s,\"system\":%s,\"csv\":%s,\"tree\":%s,\"interval_seconds\":%d},",
	  info->counter_mask,
	  info->aflag ? "true" : "false",
	  info->sflag ? "true" : "false",
	  info->csvflag ? "true" : "false",
	  info->treeflag ? "true" : "false",
	  info->interval);

  fprintf(fp,"\"output_files\":{\"output_path\":");
  json_write_string_or_null(fp,info->output_path);
  fprintf(fp,",\"tree_output_path\":");
  json_write_string_or_null(fp,info->tree_output_path);
  fprintf(fp,",\"manifest_path\":");
  json_write_string_or_null(fp,info->manifest_path);
  fprintf(fp,"}");

  fprintf(fp,"}\n");

  fflush(fp);
  flock(fileno(fp),LOCK_UN);
  fclose(fp);
  return 0;
}
