/*
 * error.h - standard error routine handling
 */
#ifndef _MEV_ERROR_H
#define _MEV_ERROR_H 1
#include <stdio.h>
/* initialize error subsystem:
 *    program = argv[0] name to print preceding errors
 *    filename = output filename, "-" is default
 *    level = error level
 */
void initialize_error_subsystem(char *program,char *filename);
void fatal(char *message,...);
void error(char *message,...);
void warning(char *message,...);
void notice(char *message,...);
void notice_noprogram(char *message,...);
void debug(char *message,...);
void debug2(char *message,...);
void set_error_stream(FILE *fp);
enum error_level get_error_level(void);
extern int n_error;
extern int n_warning;

/* level of error messages to be printed */
enum error_level {
  ERROR_LEVEL_FATAL,
  ERROR_LEVEL_ERROR,
  ERROR_LEVEL_WARNING,
  ERROR_LEVEL_NOTICE,
  ERROR_LEVEL_DEBUG,
  ERROR_LEVEL_DEBUG2
};
void set_error_level(enum error_level level);
#endif
