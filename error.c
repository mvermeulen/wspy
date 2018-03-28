/*
 * error.c - standard error printing routines
 */
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "error.h"

static char *program = NULL;
static char *filename = "-";
static FILE *error_stream = NULL;
static int initialized = 0;
static enum error_level elevel = ERROR_LEVEL_NOTICE;
int n_error = 0;
int n_warning = 0;

void initialize_error_subsystem(char *prog,char *filename){
  FILE *new_stream;
  program = prog;
  initialized = 1;
  if (!strcmp(filename,"-")){
    error_stream = stderr;
  } else {
    if (new_stream = fopen(filename,"a+")){
      error_stream = new_stream;
    } else {
      /* avoid looping in call to fatal */
      fatal("can not open error file: %s\n",filename);
    }
  }
  return;
}

void fatal(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_FATAL){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    fprintf(error_stream,"fatal error: ");
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
  exit(1);
}

void error(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_ERROR){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    fprintf(error_stream,"error: ");
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
  n_error++;
}

void warning(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_WARNING){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    fprintf(error_stream,"warning: ");
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
  n_warning++;
}

void notice(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_NOTICE){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
}

void notice_noprogram(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_NOTICE){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    //    if (program != NULL) fprintf(error_stream,"%s: ",program);
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
}

void debug(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_DEBUG){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    fprintf(error_stream,"debug: ");
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
}


void debug2(char *message,...){
  va_list ap;
  if (elevel >= ERROR_LEVEL_DEBUG2){
    if (!initialized) initialize_error_subsystem(NULL,"-");
    va_start(ap,message);
    if (program != NULL) fprintf(error_stream,"%s: ",program);
    fprintf(error_stream,"debug: ");
    vfprintf(error_stream,message,ap);
    va_end(ap);
  }
}

void set_error_level(enum error_level level){
  elevel = level;
}
