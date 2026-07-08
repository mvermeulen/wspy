/*
 * coverage.h - counter capability discovery + "measured vs unavailable"
 * coverage reporting.
 *
 * setup_counters() (topdown.c) calls coverage_note() for every counter it
 * attempts to open via perf_event_open, whether or not that open succeeds.
 * This gives two related but distinct views built from the same data:
 *   - print_counter_coverage(): a concise "N/M measured" summary plus a list
 *     of just the gaps, meant to travel with a real run's output/manifest so
 *     a downstream reader knows if a metric is missing rather than zero.
 *   - print_capability_report(): a full per-counter available/unavailable
 *     listing, meant for standalone discovery (wspy --capabilities) before
 *     any workload is run.
 */
#ifndef _WSPY_COVERAGE_H
#define _WSPY_COVERAGE_H 1

/* Uses FILE *outfile and enum output_format from wspy.h -- like manifest.h
 * and run_index.h, this header doesn't include wspy.h itself (wspy.h/
 * cpu_info.h have no include guards, so each .c file includes wspy.h
 * exactly once, before any header that depends on its types). */

struct coverage_entry {
  char *group_label;
  char *counter_label;
  int available;
  int open_errno; /* valid only when !available */
  struct coverage_entry *next;
};

extern int coverage_requested;
extern int coverage_measured;
extern struct coverage_entry *coverage_entries; /* all entries, setup order */

/* Clears accumulated coverage state. Safe to call even when nothing has
 * been recorded yet (e.g. at the top of main()). */
void coverage_reset(void);

/* Records the outcome of one perf_event_open attempt. open_errno is only
 * read when available is 0. */
void coverage_note(const char *group_label,const char *counter_label,int available,int open_errno);

void print_counter_coverage(enum output_format oformat);
void print_capability_report(void);

#endif
