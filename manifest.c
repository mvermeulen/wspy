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

/* Writes the known/exited/exit_code/signaled/term_signal fields of one
 * manifest_exit_status as JSON object members (no enclosing braces -- the
 * caller writes those, since the top-level "exit_status" block and each
 * "passes[]" entry's own exit_status use different surrounding indent).
 * indent is the per-line leading whitespace, matching whichever block this
 * is nested inside. */
static void write_exit_status_fields(FILE *fp,const struct manifest_exit_status *es,const char *indent){
  fprintf(fp,"%s\"known\": %s,\n",indent,es->known ? "true" : "false");
  if (es->known){
    fprintf(fp,"%s\"exited\": %s,\n",indent,es->exited ? "true" : "false");
    if (es->exited) fprintf(fp,"%s\"exit_code\": %d,\n",indent,es->exit_code);
    else fprintf(fp,"%s\"exit_code\": null,\n",indent);
    fprintf(fp,"%s\"signaled\": %s,\n",indent,es->signaled ? "true" : "false");
    if (es->signaled) fprintf(fp,"%s\"term_signal\": %d\n",indent,es->term_signal);
    else fprintf(fp,"%s\"term_signal\": null\n",indent);
  } else {
    fprintf(fp,"%s\"exited\": null,\n",indent);
    fprintf(fp,"%s\"exit_code\": null,\n",indent);
    fprintf(fp,"%s\"signaled\": null,\n",indent);
    fprintf(fp,"%s\"term_signal\": null\n",indent);
  }
}

/* Writes one environment field as "name": <value-or-null>, where a string
 * field is a quoted JSON string and a numeric field (memory_total_kb) is a
 * bare JSON number -- the caller passes as_number to select which. */
static void write_provenance_field(FILE *fp,const char *name,const struct provenance_field *f,int as_number){
  fprintf(fp,"    \"%s\": ",name);
  if (!f->available){
    fputs("null",fp);
    return;
  }
  if (as_number) fputs(f->value,fp);
  else json_write_string(fp,f->value);
}

static void write_environment(FILE *fp,const struct provenance_info *prov){
  struct provenance_gap gaps[PROVENANCE_TRACKED_FIELD_COUNT];
  int ngaps,i;

  fprintf(fp,"  \"environment\": {\n");
  write_provenance_field(fp,"virt_role",&prov->virt_role,0);           fprintf(fp,",\n");
  write_provenance_field(fp,"hypervisor_vendor",&prov->hypervisor_vendor,0); fprintf(fp,",\n");
  write_provenance_field(fp,"microcode_version",&prov->microcode_version,0); fprintf(fp,",\n");
  write_provenance_field(fp,"bios_vendor",&prov->bios_vendor,0);       fprintf(fp,",\n");
  write_provenance_field(fp,"bios_version",&prov->bios_version,0);     fprintf(fp,",\n");
  write_provenance_field(fp,"bios_date",&prov->bios_date,0);           fprintf(fp,",\n");
  write_provenance_field(fp,"cpu_governor",&prov->cpu_governor,0);     fprintf(fp,",\n");
  write_provenance_field(fp,"cpu_scaling_driver",&prov->cpu_scaling_driver,0); fprintf(fp,",\n");
  fprintf(fp,"    \"cpu_governor_uniform\": %s,\n",prov->cpu_governor.available ? (prov->cpu_governor_uniform ? "true" : "false") : "null");
  write_provenance_field(fp,"memory_total_kb",&prov->mem_total_kb,1);  fprintf(fp,",\n");
  write_provenance_field(fp,"compiler_version",&prov->compiler_version,0); fprintf(fp,",\n");
  write_provenance_field(fp,"libc_version",&prov->libc_version,0);     fprintf(fp,"\n");
  fprintf(fp,"  },\n");

  ngaps = provenance_gaps(prov,gaps);
  fprintf(fp,"  \"environment_coverage\": {\n");
  fprintf(fp,"    \"captured\": %d,\n",PROVENANCE_TRACKED_FIELD_COUNT - ngaps);
  fprintf(fp,"    \"probed\": %d,\n",PROVENANCE_TRACKED_FIELD_COUNT);
  fprintf(fp,"    \"unavailable\": [\n");
  for (i = 0; i < ngaps; i++){
    fprintf(fp,"     %s{ \"field\": ",i ? ",\n" : "");
    json_write_string(fp,gaps[i].field_name);
    fprintf(fp,", \"reason\": ");
    json_write_string(fp,gaps[i].reason);
    fprintf(fp," }");
  }
  fprintf(fp,"%s    ]\n",ngaps ? "\n" : "");
  fprintf(fp,"  },\n");
}

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
  fprintf(fp,"  \"collector\": \"%s\",\n",info->collector ? info->collector : "wspy");
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
  write_exit_status_fields(fp,&info->exit_status,"    ");
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

  write_environment(fp,&info->provenance);

  fprintf(fp,"  \"options\": {\n");
  fprintf(fp,"    \"counter_mask\": \"0x%x\",\n",info->counter_mask);
  fprintf(fp,"    \"per_core\": %s,\n",info->aflag ? "true" : "false");
  fprintf(fp,"    \"system\": %s,\n",info->sflag ? "true" : "false");
  fprintf(fp,"    \"csv\": %s,\n",info->csvflag ? "true" : "false");
  fprintf(fp,"    \"tree\": %s,\n",info->treeflag ? "true" : "false");
  fprintf(fp,"    \"interval_seconds\": %d\n",info->interval);
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"counter_coverage\": {\n");
  fprintf(fp,"    \"requested\": %d,\n",info->counters_requested);
  fprintf(fp,"    \"measured\": %d,\n",info->counters_measured);
  fprintf(fp,"    \"unavailable\": [\n");
  for (i = 0; i < info->counters_unavailable_count; i++){
    fprintf(fp,"     %s{ \"group\": ",i ? ",\n" : "");
    json_write_string(fp,info->counters_unavailable[i].group_label);
    fprintf(fp,", \"counter\": ");
    json_write_string(fp,info->counters_unavailable[i].counter_label);
    fprintf(fp,", \"errno\": %d, \"reason\": ",info->counters_unavailable[i].open_errno);
    json_write_string(fp,strerror(info->counters_unavailable[i].open_errno));
    fprintf(fp," }");
  }
  fprintf(fp,"%s    ]\n",info->counters_unavailable_count ? "\n" : "");
  fprintf(fp,"  },\n");

  fprintf(fp,"  \"passes\": [\n");
  for (i = 0; i < info->npasses; i++){
    const struct manifest_pass_info *mp = &info->passes[i];
    char pass_start_buf[40],pass_finish_buf[40];

    format_iso8601(&mp->start_time,pass_start_buf,sizeof(pass_start_buf));
    format_iso8601(&mp->finish_time,pass_finish_buf,sizeof(pass_finish_buf));
    fprintf(fp,"%s    {\n",i ? ",\n" : "");
    fprintf(fp,"      \"counter_mask\": \"0x%x\",\n",mp->counter_mask);
    fprintf(fp,"      \"counters_requested\": %d,\n",mp->counters_requested);
    fprintf(fp,"      \"counters_measured\": %d,\n",mp->counters_measured);
    fprintf(fp,"      \"start_time\": \"%s\",\n",pass_start_buf);
    fprintf(fp,"      \"finish_time\": \"%s\",\n",pass_finish_buf);
    fprintf(fp,"      \"exit_status\": {\n");
    write_exit_status_fields(fp,&mp->exit_status,"        ");
    fprintf(fp,"      }\n");
    fprintf(fp,"    }");
  }
  fprintf(fp,"%s  ],\n",info->npasses ? "\n" : "");

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
