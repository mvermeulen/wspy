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
#include "provenance.h"

/* SemVer of the manifest.json *document shape*, independent of the wspy
 * tool version. Bump MAJOR when a field is removed/renamed in a way that
 * breaks existing readers, MINOR when a field is added in a backward
 * compatible way, PATCH for fixes that don't change the shape. Consumers
 * should warn (not silently misparse) on an unrecognized MAJOR version. */
#define MANIFEST_SCHEMA_VERSION "1.5.0"

/* One counter that setup_counters() (topdown.c) tried and failed to open via
 * perf_event_open, as recorded by coverage.c. Kept as its own small struct
 * (rather than pulling in coverage.h here) so manifest.h stays self
 * contained, matching manifest_exit_status below. */
struct manifest_counter_gap {
  const char *group_label;
  const char *counter_label;
  int open_errno;
};

/* One launcher-vocabulary option name/value pair -- see
 * manifest_config_provenance below. Both are opaque strings from wspy's own
 * point of view; a front end (wspy-run, the web launcher) picks names that
 * make sense in its own preset/configuration/option vocabulary (e.g.
 * "counter_groups"="topdown,cache2"), not a fixed enum wspy validates. */
struct manifest_config_option {
  const char *name;
  const char *value;
};

/* Structured configuration provenance (INVESTIGATION_4.0.md item 16): which
 * named preset (if any) and/or launcher-vocabulary configuration category
 * and options produced this run. wspy itself has no notion of presets or
 * configurations -- that vocabulary belongs to a front end (wspy-run's
 * builtin profiles, the web launcher's preset picker/checklist) -- so these
 * fields are populated purely from --preset-name/--config-name/
 * --config-option (metadata only, no effect on what the run does) rather
 * than derived from counter_mask/aflag/etc. All-NULL/zero (the default for
 * a direct wspy invocation with none of those flags given) means "not
 * launched from a known preset or configuration", not a gap -- most direct
 * command-line uses of wspy will leave this empty. This is what lets a
 * report say "this was deep-cpu's performance-counters pass" in the same
 * words the launcher used, instead of re-deriving it from counter_mask/argv. */
struct manifest_config_provenance {
  const char *preset;          /* e.g. "deep-cpu", NULL if none              */
  const char *configuration;   /* e.g. "performance-counters", NULL if none  */
  int noptions;
  const struct manifest_config_option *options; /* array, length noptions   */
};

struct manifest_exit_status {
  int known;       /* 0 if the exit status was not observed this run       */
  int exited;      /* 1 if the child exited normally (valid if known)      */
  int exit_code;   /* valid if known && exited                             */
  int signaled;    /* 1 if the child was terminated by a signal            */
  int term_signal; /* valid if known && signaled                           */
};

/* One pass of a --passes=<list> (native multi-pass counter execution,
 * multipass.h) run: that pass's own counter_mask subset, its own
 * per-pass delta of coverage_requested/measured (not the running total),
 * and its own timing/exit status from its own separate re-execution of the
 * workload. A normal (non---passes) run has manifest_info.npasses == 0 and
 * never populates this. */
struct manifest_pass_info {
  unsigned int counter_mask;
  int counters_requested;
  int counters_measured;
  struct timespec start_time;
  struct timespec finish_time;
  struct manifest_exit_status exit_status;
};

struct manifest_info {
  /* Name of the tool that collected this run's data. Always "wspy" today --
   * this codebase has exactly one collector -- but the field exists so the
   * manifest/run-index schema doesn't silently assume that forever. A future
   * non-wspy collector (perf stat, trace-cmd, a GPU vendor tool) feeding the
   * same manifest+normalization path would populate its own name here rather
   * than requiring a breaking schema change to add the concept. See
   * INVESTIGATION_4.0.md's "Collector-plugin architecture" row. */
  const char *collector;
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
  /* Counter capability discovery + coverage reporting (see coverage.h): how
   * many counters this run's counter_mask/aflag combination requested vs
   * how many actually opened via perf_event_open. Independent of
   * exit_status -- a run can exit cleanly with partial counter coverage. */
  int counters_requested;
  int counters_measured;
  int counters_unavailable_count;
  const struct manifest_counter_gap *counters_unavailable; /* array, length counters_unavailable_count */
  /* Native multi-pass counter execution (--passes=<list>, multipass.h): 0/NULL
   * for a normal run. counter_mask above is the union of every pass's mask
   * when npasses > 0; counters_requested/measured above are still the
   * running totals across all passes (coverage_reset() runs once, not per
   * pass), so this array exists purely for per-pass audit detail, not to
   * recompute those top-level fields. */
  int npasses;
  const struct manifest_pass_info *passes; /* array, length npasses */
  /* Environment/provenance capture (see provenance.h): best-effort host
   * facts beyond the always-present host{} block below (virtualization
   * role, microcode, BIOS, governor, memory, toolchain). Populate via
   * provenance_collect() -- every field degrades to "unavailable" rather
   * than failing the run. */
  struct provenance_info provenance;
  /* Structured configuration provenance (see manifest_config_provenance
   * above): all-NULL/zero unless a front end passed --preset-name/
   * --config-name/--config-option. */
  struct manifest_config_provenance config_provenance;
};

/* Writes the manifest as JSON to path. Returns 0 on success, -1 if the file
 * could not be opened for writing (a warning is logged in that case). */
int write_manifest(const char *path,const struct manifest_info *info);

#endif
