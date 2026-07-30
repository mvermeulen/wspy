/*
 * symbol_sample.h - generic PERF_SAMPLE_IP sampling-mode capture, the (a)
 * half of 4.4 priorities item 9 ("symbol-level profiling"). Generalizes
 * ibs_sample.c's mmap ring-buffer plumbing (now factored out into
 * perf_ring.c/perf_ring.h, shared by both) from AMD-IBS-specific
 * PERF_SAMPLE_RAW decode to an ordinary perf_event_open() sampling event on
 * any counter, AMD or Intel (cycles by default; see INVESTIGATION.md's
 * "Symbol-level profiling deep-dive" for the curated event list --symbol-
 * sample will expose). Unlike ibs_sample.c's per-sample tag decode, a
 * PERF_SAMPLE_IP record's payload is just one u64 -- the interrupted
 * instruction pointer -- so this file's "decode" is an in-memory address
 * histogram (unique IP -> hit count) rather than a per-field bit decode.
 *
 * Scoped to --target-matched processes only (design decision, see
 * INVESTIGATION.md): one of these states is opened pid-scoped, alongside a
 * matched process's --target counter group, at PTRACE_EVENT_EXEC, and
 * drained once at that same pid's PTRACE_EVENT_EXIT -- before
 * /proc/<pid>/maps (needed for address-to-symbol resolution downstream, by
 * the not-yet-built wspy-symbolize tool) becomes unavailable. Draining only
 * ever happens then, never from a signal handler -- same non-real-time
 * constraint ibs_sample.c's file comment documents, and the same
 * consequence: a very long-lived target risks ring wraparound
 * (PERF_RECORD_LOST) before its one drain.
 *
 * No call-graph (PERF_SAMPLE_CALLCHAIN) in this cut -- a flat self-hit
 * histogram only, the moral equivalent of `perf report --no-children`.
 * Address resolution (PIE/shared-library load bias, ELF/DWARF symbol
 * lookup) is out of scope for this file entirely -- it happens in the
 * separate post-hoc wspy-symbolize tool against the raw addresses/counts
 * this file produces plus a /proc/<pid>/maps snapshot, not here.
 */
#ifndef _WSPY_SYMBOL_SAMPLE_H
#define _WSPY_SYMBOL_SAMPLE_H 1

#include <stdint.h>

/* Uses FILE *outfile/enum output_format from wspy.h -- like ibs_sample.h,
 * this header doesn't include wspy.h itself (see ibs.h's own comment:
 * wspy.h/cpu_info.h have no include guards, so each .c file includes
 * wspy.h exactly once, before any header depending on its types). */

/* Ring size: data pages only (a mandatory extra header page is added by
 * perf_ring_mmap() itself). 64 data pages = 256 KiB on a 4 KiB-page host --
 * generous on purpose, since nothing drains the ring until this pid's own
 * exit (see file comment); must be a power of 2 per the perf mmap ABI.
 * Same size as IBS_SAMPLE_MMAP_DATA_PAGES (ibs_sample.h) -- both tolerate
 * the same "never drained mid-run" constraint, no reason for a different
 * budget. */
#define SYMBOL_SAMPLE_MMAP_DATA_PAGES 64

/* Initial/growth capacity for the address histogram below. Small on
 * purpose -- real hot loops touch a handful to a few hundred distinct
 * addresses, not thousands; symbol_sample_record_addr() doubles on
 * overflow so an unexpectedly address-diverse target still degrades to
 * "slower, not wrong" rather than a hard cap. */
#define SYMBOL_SAMPLE_INITIAL_CAPACITY 64

/* One (instruction pointer, hit count) bucket. Linear-scanned, not hashed
 * -- see symbol_sample_record_addr()'s comment for why that's the right
 * tradeoff at this scale. */
struct symbol_sample_addr_count {
  uint64_t addr;
  uint64_t count;
};

/* One mmap'd PERF_SAMPLE_IP sampling event's ring buffer + accumulated
 * address histogram. Opaque to every caller except symbol_sample.c itself. */
struct symbol_sample_state {
  void *ring_base;   /* mmap() base: 1 header page + SYMBOL_SAMPLE_MMAP_DATA_PAGES data pages */
  size_t ring_len;   /* total mmap length in bytes, for perf_ring_unmap() */
  int drained;       /* 1 once symbol_sample_drain() has run for this state --
                       * guards against a second call double-counting. */

  uint64_t samples_seen;    /* PERF_RECORD_SAMPLE records seen (== sum of addrs[].count once drained) */
  uint64_t samples_lost;    /* summed PERF_RECORD_LOST .lost counts */
  uint64_t decode_skipped;  /* records shorter than one u64 -- shouldn't happen with sample_type=PERF_SAMPLE_IP alone, guarded anyway */

  struct symbol_sample_addr_count *addrs;
  int naddrs;
  int addrs_cap;
};

struct perf_event_attr;
/* Sets the fields that make an already-built counting-mode perf_event_attr
 * into a sampling one: sample_type=PERF_SAMPLE_IP plus a wakeup_events
 * value. Caller sets pe->sample_period beforehand (same convention
 * ibs_sample_attr_init() and topdown.c's setup_counters() already use for
 * IBS -- sample_period is generic per-counter plumbing, not sampling-mode-
 * specific). Call before perf_event_open(); perf_ring_mmap()'s expectations
 * depend on sample_type being set this way first. */
void symbol_sample_attr_init(struct perf_event_attr *pe);

/* mmap()s fd (already perf_event_open()'d via symbol_sample_attr_init()'s
 * attr) and returns a freshly allocated, zeroed state, or NULL on mmap()
 * failure (logged via warning(), never fatal). */
struct symbol_sample_state *symbol_sample_mmap(int fd);

void symbol_sample_free(struct symbol_sample_state *state);

/* Walks every unread record in state's ring buffer exactly once (idempotent
 * after the first call -- see .drained), accumulating each record's IP into
 * the address histogram and counting PERF_RECORD_LOST, then advances the
 * consumer (data_tail) past what was read. Safe to call only outside a
 * signal handler -- see file comment. No-op if state is NULL (mmap()
 * failed earlier). */
void symbol_sample_drain(struct symbol_sample_state *state);

/* Pure accumulation: find-or-insert addr in state->addrs, incrementing its
 * count (or appending a new bucket, growing addrs[] by doubling if full).
 * No I/O; unit-testable directly. Linear scan rather than a hash table --
 * deliberate for this scale (see SYMBOL_SAMPLE_INITIAL_CAPACITY's comment):
 * a hot loop's sample set is small enough that a hash table's constant
 * overhead wouldn't pay for itself, and this keeps the code a straight
 * find-or-append with no collision handling to get wrong. No-op if state
 * is NULL. */
void symbol_sample_record_addr(struct symbol_sample_state *state,uint64_t addr);

/* struct counter_group/counter_info come from cpu_info.h, enum
 * symbol_sample_event from wspy.h -- forward-declared/assumed-included here
 * per ibs_sample.h's own pattern (this header doesn't include cpu_info.h,
 * and relies on wspy.h having been included first by any .c file that
 * includes this one -- see the top-of-file comment). Builds a single-
 * counter group for --symbol-sample: a generic PERF_TYPE_HARDWARE event
 * (cycles/instructions/cache-misses/branch-misses -- see wspy.h's curated
 * enum symbol_sample_event) opened in sampling mode, is_symbol_sample=1,
 * with a first-cut fixed sample_period (not frequency-based -- see
 * symbol_sample.c's comment on why). Always succeeds structurally (unlike
 * ibs_sample_counter_group(), there's no hardware-support probe to fail --
 * generic hardware events are assumed present); the real fd open still
 * happens later, in setup_counters(), and can fail there like any other
 * counter. */
struct counter_group;
struct counter_group *symbol_sample_counter_group(char *name,enum symbol_sample_event event);

#endif
