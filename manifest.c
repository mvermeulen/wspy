/*
 * manifest.c - write the JSON run manifest described in manifest.h
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/utsname.h>
#include "manifest.h"
#include "wspy.h"
#include "error.h"
#include "json_util.h"

int write_manifest(const char *path,const struct manifest_info *info){
  FILE *fp;
  int i;
  char start_buf[40],finish_buf[40],generated_buf[40];
  char hostname[256];
  struct utsname uts;
  struct timespec now;
  char wspy_version[16];
  double elapsed;
  int have_output_file = 0;

  fp = fopen(path,"w");
  if (!fp){
    warning("unable to open manifest file: %s\n",path);
    return -1;
  }

  clock_gettime(CLOCK_REALTIME,&now);
  format_iso8601(&now,generated_buf,sizeof(generated_buf));
  format_iso8601(&info->start_time,start_buf,sizeof(start_buf));
  format_iso8601(&info->finish_time,finish_buf,sizeof(finish_buf));
  elapsed = (info->finish_time.tv_sec - info->start_time.tv_sec) +
    (info->finish_time.tv_nsec - info->start_time.tv_nsec) / 1000000000.0;

  if (gethostname(hostname,sizeof(hostname)) != 0){
    strcpy(hostname,"unknown");
  }
  hostname[sizeof(hostname)-1] = '\0';

  if (uname(&uts) != 0){
    strcpy(uts.release,"unknown");
  }

  snprintf(wspy_version,sizeof(wspy_version),"%d.%d",WSPY_VERSION_MAJOR,WSPY_VERSION_MINOR);

  fprintf(fp,"{\n");
  fprintf(fp,"  \"schema_version\": \"%s\",\n",MANIFEST_SCHEMA_VERSION);
  fprintf(fp,"  \"wspy_version\": \"%s\",\n",wspy_version);
  fprintf(fp,"  \"generated_at\": \"%s\",\n",generated_buf);

  fprintf(fp,"  \"command\": {\n");
  fprintf(fp,"    \"argv\": [");
  for (i = 0; i < info->argc; i++){
    if (i) fprintf(fp,", ");
    json_write_string(fp,info->argv[i]);
  }
  fprintf(fp,"]\n");
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"timing\": {\n");
  fprintf(fp,"    \"start_time\": \"%s\",\n",start_buf);
  fprintf(fp,"    \"finish_time\": \"%s\",\n",finish_buf);
  fprintf(fp,"    \"elapsed_seconds\": %.3f\n",elapsed);
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"exit_status\": {\n");
  fprintf(fp,"    \"known\": %s,\n",info->exit_status.known ? "true" : "false");
  if (info->exit_status.known){
    fprintf(fp,"    \"exited\": %s,\n",info->exit_status.exited ? "true" : "false");
    if (info->exit_status.exited) fprintf(fp,"    \"exit_code\": %d,\n",info->exit_status.exit_code);
    else fprintf(fp,"    \"exit_code\": null,\n");
    fprintf(fp,"    \"signaled\": %s,\n",info->exit_status.signaled ? "true" : "false");
    if (info->exit_status.signaled) fprintf(fp,"    \"term_signal\": %d\n",info->exit_status.term_signal);
    else fprintf(fp,"    \"term_signal\": null\n");
  } else {
    fprintf(fp,"    \"exited\": null,\n");
    fprintf(fp,"    \"exit_code\": null,\n");
    fprintf(fp,"    \"signaled\": null,\n");
    fprintf(fp,"    \"term_signal\": null\n");
  }
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"host\": {\n");
  fprintf(fp,"    \"hostname\": ");
  json_write_string(fp,hostname);
  fprintf(fp,",\n");
  fprintf(fp,"    \"kernel_release\": ");
  json_write_string(fp,uts.release);
  fprintf(fp,",\n");
  fprintf(fp,"    \"cpu_vendor\": ");
  json_write_string(fp,cpu_vendor_name(cpu_info->vendor));
  fprintf(fp,",\n");
  fprintf(fp,"    \"cpu_family\": %u,\n",cpu_info->family);
  fprintf(fp,"    \"cpu_model\": %u,\n",cpu_info->model);
  fprintf(fp,"    \"num_cores\": %u,\n",cpu_info->num_cores);
  fprintf(fp,"    \"num_cores_available\": %u,\n",cpu_info->num_cores_available);
  fprintf(fp,"    \"is_hybrid\": %s\n",cpu_info->is_hybrid ? "true" : "false");
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"options\": {\n");
  fprintf(fp,"    \"counter_mask\": \"0x%x\",\n",info->counter_mask);
  fprintf(fp,"    \"per_core\": %s,\n",info->aflag ? "true" : "false");
  fprintf(fp,"    \"system\": %s,\n",info->sflag ? "true" : "false");
  fprintf(fp,"    \"csv\": %s,\n",info->csvflag ? "true" : "false");
  fprintf(fp,"    \"tree\": %s,\n",info->treeflag ? "true" : "false");
  fprintf(fp,"    \"interval_seconds\": %d\n",info->interval);
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"output_files\": [\n");
  if (info->output_path){
    fprintf(fp,"    { \"kind\": \"output\", \"path\": ");
    json_write_string(fp,info->output_path);
    fprintf(fp," }");
    have_output_file = 1;
  }
  if (info->tree_output_path){
    fprintf(fp,"%s    { \"kind\": \"tree\", \"path\": ",have_output_file ? ",\n" : "");
    json_write_string(fp,info->tree_output_path);
    fprintf(fp," }");
    have_output_file = 1;
  }
  if (info->manifest_path){
    fprintf(fp,"%s    { \"kind\": \"manifest\", \"path\": ",have_output_file ? ",\n" : "");
    json_write_string(fp,info->manifest_path);
    fprintf(fp," }");
    have_output_file = 1;
  }
  fprintf(fp,"\n  ]\n");
  fprintf(fp,"}\n");

  fclose(fp);
  return 0;
}
