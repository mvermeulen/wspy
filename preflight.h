/*
 * preflight.h - counter-fit preflight: estimates, from the counter groups a
 * counter_mask would request, whether they'll fit in the available
 * general-purpose hardware PMU counter slots without multiplexing -- before
 * any perf_event_open() calls are made (unlike coverage.c, which reports
 * what actually happened after setup_counters() tried to open everything).
 *
 * INVESTIGATION_4.0.md's "Counter-fit preflight" item: availability/
 * NMI-watchdog handling already exists at runtime (topdown.c's
 * check_nmi_watchdog(), coverage.c); this surfaces the same fit information
 * before a run instead of discovering it (as silent undercounting, since
 * read_counters() doesn't yet scale by time_running/time_enabled) after.
 *
 * Needs no privileges and opens no perf events: it's pure arithmetic over
 * the same event tables setup_counter_groups() (topdown.c) already builds
 * from, so it's safe to run as an unprivileged, standalone probe (see
 * wspy.c's --preflight) as well as automatically before every real run.
 *
 * Uses FILE *outfile and enum output_format from wspy.h -- like coverage.h,
 * this header doesn't include wspy.h itself (wspy.h/cpu_info.h have no
 * include guards, so each .c file includes wspy.h exactly once, before any
 * header that depends on its types).
 */
#ifndef _WSPY_PREFLIGHT_H
#define _WSPY_PREFLIGHT_H 1

/* One counter group's contribution to the general-purpose hardware PMU
 * counter budget. mask is the group's COUNTER_* bit(s) (struct
 * counter_group's own .mask field), used to look up matching --no-<flag>
 * downgrade hints. */
struct preflight_group_usage {
  char *label;
  unsigned int mask;
  int count;
  struct preflight_group_usage *next;
};

struct preflight_result {
  int requested;           /* general-purpose HW PMU counters this mask would request */
  int available;           /* estimated available slots given current NMI-watchdog state */
  int nmi_watchdog_active;
  int fits;                /* requested <= available */
  struct preflight_group_usage *groups; /* per-group breakdown, largest contributors listed first */
};

/* Pure computation over an already-built counter_group_list (e.g. one
 * constructed by hand in a test, or by setup_counter_groups()). Doesn't
 * touch counter_mask or call setup_counter_groups() itself -- see
 * preflight_evaluate() below for that. */
struct preflight_result preflight_evaluate_groups(struct counter_group *list);

/* Convenience wrapper for real use: builds a throwaway counter_group_list
 * for `mask` via setup_counter_groups() (temporarily swapping the global
 * counter_mask, the same idiom wspy.c's run_capabilities_probe() already
 * uses for --capabilities), evaluates it, and leaves the throwaway list to
 * be reclaimed at process exit (counter_group lists are never individually
 * freed anywhere in this codebase; see topdown.c/wspy.c). */
struct preflight_result preflight_evaluate(unsigned int mask);

/* Frees a preflight_result's groups list. Does not free counter_group_list
 * itself, since preflight_evaluate() doesn't keep a pointer to it. */
void preflight_result_free(struct preflight_result *result);

/* Prints the full per-group breakdown + fit verdict, unconditionally --
 * used by the standalone --preflight probe. Writes to outfile (not
 * error_stream), matching print_capability_report()'s style since this is
 * the primary output of that probe invocation. */
void print_preflight_report(struct preflight_result *result);

/* Prints a warning (via error.c's warning()/notice(), i.e. error_stream --
 * usually stderr, so it never corrupts a real run's CSV output on outfile)
 * with suggested downgrades, but only when the fit doesn't hold -- silent
 * otherwise, mirroring check_nmi_watchdog()'s "quiet unless there's
 * something to say" style. Used automatically before every real run. */
void preflight_warn_if_tight(struct preflight_result *result);

#endif
