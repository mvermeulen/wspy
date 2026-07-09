/*
 * provenance.h - best-effort environment/provenance capture for the run
 * manifest and run index: host/guest role, microcode version, BIOS
 * vendor/version/date, CPU frequency governor, total memory, and the
 * toolchain (compiler/libc) that built this wspy binary.
 *
 * Unlike counter coverage (coverage.h), these fields describe the host, not
 * the run, so provenance_collect() is cheap (a handful of small reads) and
 * has no setup/teardown lifecycle -- call it once, right before writing the
 * manifest/run index. Every field is captured independently and left
 * unavailable (not fatal) on failure -- e.g. no /sys/class/dmi/id on a VM
 * with a minimal firmware pass-through, or a restricted container without
 * cpufreq exposed -- mirroring coverage.c's "measured vs unavailable"
 * pattern for counters (see CLAUDE.md, "Reproducibility, comparability,
 * statistics" track). x86_64-only, like the rest of this codebase's cpuid
 * use (cpu_info.c).
 */
#ifndef _WSPY_PROVENANCE_H
#define _WSPY_PROVENANCE_H 1

#define PROVENANCE_VALUE_LEN 80

struct provenance_field {
  int available;
  char value[PROVENANCE_VALUE_LEN]; /* valid when available            */
  char reason[64];                  /* human-readable cause, when !available */
};

struct provenance_info {
  struct provenance_field virt_role;          /* "host" or "guest", from cpuid leaf 1 */
  struct provenance_field hypervisor_vendor;   /* cpuid leaf 0x40000000, only when guest */
  struct provenance_field microcode_version;   /* "microcode" line in /proc/cpuinfo */
  struct provenance_field bios_vendor;         /* /sys/class/dmi/id/bios_vendor */
  struct provenance_field bios_version;        /* /sys/class/dmi/id/bios_version */
  struct provenance_field bios_date;           /* /sys/class/dmi/id/bios_date */
  struct provenance_field cpu_governor;        /* cpu0's scaling_governor */
  struct provenance_field cpu_scaling_driver;  /* cpu0's scaling_driver */
  int cpu_governor_uniform;                    /* 1 if every online core's governor matches cpu0's; valid only when cpu_governor.available */
  struct provenance_field mem_total_kb;        /* total RAM, via sysinfo(2) */
  struct provenance_field compiler_version;    /* __VERSION__ at build time */
  struct provenance_field libc_version;        /* gnu_get_libc_version(), glibc only */
};

/* The subset of fields above that are independently probed and worth
 * reporting a measured/unavailable count for. Excludes hypervisor_vendor
 * (only meaningful when virt_role is "guest") and cpu_scaling_driver (bonus
 * detail alongside cpu_governor, not a separate probe). Keep in sync with
 * provenance_gaps()'s tracked[] table in provenance.c. */
#define PROVENANCE_TRACKED_FIELD_COUNT 9

struct provenance_gap {
  const char *field_name;
  const char *reason;
};

/* Best-effort collection of every field above. Always succeeds (never
 * fails the run); fields that can't be read are left with available=0 and a
 * human-readable reason. */
void provenance_collect(struct provenance_info *info);

/* Fills out[] with one entry per PROVENANCE_TRACKED_FIELD_COUNT field that
 * came back unavailable. out must have room for at least
 * PROVENANCE_TRACKED_FIELD_COUNT entries. Returns the count filled in. */
int provenance_gaps(const struct provenance_info *info,struct provenance_gap *out);

/* Convenience: PROVENANCE_TRACKED_FIELD_COUNT minus the number of gaps. */
int provenance_count_available(const struct provenance_info *info);

#endif
