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
#define MANIFEST_SCHEMA_VERSION "1.9.0"

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

/* Structured configuration provenance (INVESTIGATION.md, "What shipped in
 * 4.1"): which named preset (if any) and/or launcher-vocabulary configuration category
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

/* cgroup v2 identity + resource limits + CPU-throttling delta over the
 * measurement window (INVESTIGATION.md's 4.2 Tier 1 "cgroup identity +
 * limits in manifest, cpu.stat throttling stats" item, cgroup.h). A
 * deliberately leaner, manifest-facing projection of cgroup.h's own
 * struct cgroup_info/cgroup_throttle -- mirroring manifest_gpu_info's own
 * precedent of not reusing the collecting module's internal struct
 * directly -- so manifest.h stays self contained (no cgroup.h include
 * needed here). available=0 means no cgroup v2 unified hierarchy was
 * found at all (e.g. a pure cgroup v1 host); path is NULL in that case.
 * Each limit/throttle field degrades independently -- a leaf cgroup with
 * the cpu controller not enabled (a real, confirmed-live case: a desktop
 * session's terminal-emulator scope has memory.max but no cpu.max/
 * cpu.weight/cpu.stat throttling fields at all) leaves just that field's
 * own *_available at 0, not the whole struct. */
struct manifest_cgroup_info {
  int available;
  const char *path;            /* NULL if !available */

  int cpu_max_available;
  long long cpu_quota_us;      /* -1 = unlimited ("max") */
  long long cpu_period_us;

  int cpu_weight_available;
  int cpu_weight;

  int memory_max_available;
  long long memory_max_bytes;  /* -1 = unlimited ("max") */

  int memory_high_available;
  long long memory_high_bytes; /* -1 = not set ("max") */

  /* Delta over the measurement window (baseline taken near workload
   * launch, final reading taken at manifest-write time) -- not since the
   * cgroup's own creation. available requires both readings to have
   * succeeded (implies cpu.max/cpu.stat's throttling fields were present,
   * i.e. the cpu controller was enabled on this cgroup). */
  int throttle_available;
  unsigned long long nr_periods_delta;
  unsigned long long nr_throttled_delta;
  unsigned long long throttled_usec_delta;
};

/* GPU telemetry provenance (INVESTIGATION.md's 4.2 Tier 1 "manifest/index/
 * profile pipeline extended to GPU runs" item). Deliberately provenance-only
 * -- which flag(s) were requested, which device index actually got selected,
 * and whether each backend produced valid data on this run's last read --
 * not a duplicate of the measured busy/temp/activity/power/freq/VRAM values
 * themselves, which stay CSV-only like every other metric this codebase
 * collects. All-zero/-1 (the default, e.g. a build without AMDGPU/NVIDIA, or
 * a run that never touched a GPU flag) means "not applicable", not a gap --
 * most runs don't request any GPU flag at all. amd_device_index is shared by
 * amd_sysfs.c/amd_smi.c (both are initialized with the same --gpu-device
 * value); nvidia_device_index is the --gpu-nvidia-device counterpart. */
struct manifest_gpu_info {
  int gpu_busy_requested;     /* --gpu-busy    */
  int gpu_metrics_requested;  /* --gpu-metrics (fused sysfs+SMI stream) */
  int gpu_smi_requested;      /* --gpu-smi (legacy, raw SMI columns)    */
  int gpu_nvidia_requested;   /* --gpu-nvidia  */

  int amd_device_index;       /* resolved selected AMD device index, -1 if
                                  none selected or no AMD GPU flag requested */
  int nvidia_device_index;    /* resolved selected NVIDIA device index, -1
                                  if none selected or --gpu-nvidia not given */

  /* Whether each backend actually produced valid data on this run's final
   * read (the same read already reflected in the last CSV row/human
   * output) -- lets a manifest-only reader tell "GPU flag given but this
   * host/driver never actually worked" apart from "GPU flag given and it
   * worked", without needing the CSV. amd_sysfs_busy_valid reflects
   * whether the sysfs gpu_busy_percent file was found for the selected
   * device at init time (--gpu-busy has no later per-read failure mode to
   * distinguish); the rest reflect the last actual read's success. */
  int amd_sysfs_busy_valid;
  int amd_sysfs_metrics_valid;
  int amd_smi_metrics_valid;
  int amd_smi_memory_valid;
  int nvidia_metrics_valid;
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
   * INVESTIGATION.md's "Collector-plugin architecture" row. */
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
  /* Core/thread affinity control (INVESTIGATION.md's "Core/thread
   * affinity control" item, affinity.h): the resolved placement is part of
   * a run's provenance, not just implicit in how it was launched.
   * affinity_mode is one of affinity.h's affinity_mode_name() strings
   * ("all"/"thread"/"nosmt"/"domain"/"cpuset"); affinity_requested is the
   * literal --affinity=<spec> argument text (NULL if --affinity wasn't
   * given at all, i.e. the implicit "all" default); affinity_cpus is the
   * final resolved cpu list in "0,2-3,7" form (affinity_format_cpu_set()) --
   * always populated, even for the "all" default, so a reader never has to
   * cross-reference cpu_info.num_cores to know what "all" meant on this
   * host. */
  const char *affinity_mode;
  const char *affinity_requested;
  const char *affinity_cpus;
  /* Counter capability discovery + coverage reporting (see coverage.h): how
   * many counters this run's counter_mask/aflag combination requested vs
   * how many actually opened via perf_event_open. Independent of
   * exit_status -- a run can exit cleanly with partial counter coverage. */
  int counters_requested;
  int counters_measured;
  int counters_unavailable_count;
  const struct manifest_counter_gap *counters_unavailable; /* array, length counters_unavailable_count */
  /* Formula/version metadata for topdown.c's print_topdown() hierarchical
   * (L1->L2) percentage decomposition (TOPDOWN_FORMULA_VERSION, wspy.h) --
   * NULL when this run's counter_mask includes neither COUNTER_TOPDOWN nor
   * COUNTER_TOPDOWN2, matching the "measured vs not applicable" convention
   * output_path/tree_output_path already use above. */
  const char *topdown_formula_version;
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
  /* GPU telemetry provenance (see manifest_gpu_info above): all-zero/-1
   * unless a --gpu-* flag was given. */
  struct manifest_gpu_info gpu;
  /* cgroup identity/limits/throttling (see manifest_cgroup_info above). */
  struct manifest_cgroup_info cgroup;
};

/* Writes the manifest as JSON to path. Returns 0 on success, -1 if the file
 * could not be opened for writing (a warning is logged in that case). */
int write_manifest(const char *path,const struct manifest_info *info);

#endif
