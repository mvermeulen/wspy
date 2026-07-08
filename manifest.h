/*
 * manifest.h - JSON run manifest: schema version + writer API
 *
 * The manifest is a machine-readable record of what a single wspy run did
 * (command line, timing, host, options, output files) -- not a run
 * configuration. It is the foundation artifact that downstream tooling
 * (run index, reproducibility bundles, publication scripts) can depend on
 * instead of re-deriving this information from CSV headers or shell history.
 */
#ifndef _WSPY_MANIFEST_H
#define _WSPY_MANIFEST_H 1

#include <time.h>

/* SemVer of the manifest.json *document shape*, independent of the wspy
 * tool version. Bump MAJOR when a field is removed/renamed in a way that
 * breaks existing readers, MINOR when a field is added in a backward
 * compatible way, PATCH for fixes that don't change the shape. Consumers
 * should warn (not silently misparse) on an unrecognized MAJOR version. */
#define MANIFEST_SCHEMA_VERSION "1.0.0"

struct manifest_exit_status {
  int known;       /* 0 if the exit status was not observed this run       */
  int exited;      /* 1 if the child exited normally (valid if known)      */
  int exit_code;   /* valid if known && exited                             */
  int signaled;    /* 1 if the child was terminated by a signal            */
  int term_signal; /* valid if known && signaled                           */
};

struct manifest_info {
  struct timespec start_time;
  struct timespec finish_time;
  int argc;
  char *const *argv;         /* the workload command line, argv[0]..argv[argc-1] */
  struct manifest_exit_status exit_status;
  unsigned int counter_mask;
  int aflag;                 /* --per-core */
  int sflag;                 /* --system   */
  int csvflag;                /* --csv      */
  int treeflag;                /* --tree     */
  int interval;
  const char *output_path;   /* -o <file>, NULL if output went to stdout   */
  const char *tree_output_path; /* --tree <file>, NULL if not used         */
  const char *manifest_path; /* path this manifest itself is written to    */
};

/* Writes the manifest as JSON to path. Returns 0 on success, -1 if the file
 * could not be opened for writing (a warning is logged in that case). */
int write_manifest(const char *path,const struct manifest_info *info);

#endif
