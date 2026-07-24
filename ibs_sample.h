/*
 * ibs_sample.h - AMD IBS *sampling*-mode capture: mmap's the perf ring
 * buffer for ibs_fetch/ibs_op events with sample_type=PERF_SAMPLE_RAW so
 * each individual sample's tagged register data (dc_miss, tlb misses,
 * branch mispredict, icache miss, ...) is available, rather than just a
 * count of how many fired. Distinct from ibs.c's counting-mode
 * --ibs-basic/--ibs-memory-deep profiles (a plain periodic read() of an
 * incrementing count) -- see INVESTIGATION.md's 4.3 "AMD IBS sampling-mode
 * support" for why this is a separate capability, not an extension of
 * those.
 *
 * Ring-buffer wire format (confirmed against the current Linux kernel,
 * arch/x86/events/amd/ibs.c + arch/x86/include/asm/amd/ibs.h): a
 * PERF_RECORD_SAMPLE record's raw payload is a u32 byte-count followed by
 * that many bytes of `struct perf_ibs_data` content -- a u32 `caps`
 * snapshot immediately followed by a run of u64 MSR-value words (`regs[]`).
 * For ibs_op: regs[0]=IbsOpCtl, regs[1]=IbsOpRip, regs[2]=IbsOpData,
 * regs[3]=IbsOpData2, regs[4]=IbsOpData3, then optional cap-gated words
 * (IbsBrTarget/IbsOpData4/IbsDcLinAd/IbsDcPhysAd) whose presence/position
 * varies by CPU. For ibs_fetch: regs[0]=IbsFetchCtl, regs[1]=IbsFetchLinAd,
 * regs[2]=IbsFetchPhysAd, then optional IcIbsExtdCtl.
 *
 * Decode scope is intentionally partial this cycle (see INVESTIGATION.md):
 * only the caps-independent fixed-offset prefix each record always has --
 * op-side op_brn_ret/op_brn_misp (IbsOpData, bits 37/36),
 * dc_miss/dc_l1tlb_miss/dc_l2tlb_miss (IbsOpData3, bits 7/2/3), and
 * IbsOpData2's memory-data-source field (rmt_node bit 4; data_src_lo bits
 * 0-2 + data_src_hi bits 6-7 combined into a 0-31 index); fetch-side
 * ic_miss/l1tlb_miss/l2tlb_miss (IbsFetchCtl, bits 51/55/56). Bit positions
 * verified against the kernel's union ibs_fetch_ctl/ibs_op_data/
 * ibs_op_data2/ibs_op_data3 definitions (arch/x86/include/asm/amd/ibs.h),
 * not guessed. The optional/variable-position words (IbsBrTarget/
 * IbsOpData4/IbsDcLinAd/IbsDcPhysAd) are still deferred to a follow-up
 * (feeds the separate "IBS-derived memory-path bottleneck decomposition"
 * backlog item).
 *
 * IbsOpData2's data-source index means different things on pre-Zen4 vs
 * Zen4+ (zen4_ibs_extensions cap) hardware -- confirmed against the
 * kernel's own decoder (tools/perf/util/amd-sample-raw.c): e.g. index 2 is
 * "Local node cache" on the default table but "another CCX cache in the
 * same NUMA node" on the zen4_ibs_extensions table. To avoid baking a
 * cross-scheme category mapping into a permanent CSV schema, only two
 * scheme-independent signals reach CSV (ibs_sample_dram_rate -- data_src==3
 * means "DRAM" identically on both tables; ibs_sample_remote_node_rate --
 * the rmt_node bit, printed unconditionally by the kernel's own decoder on
 * both tables, never cap-gated). The full named breakdown is decoded as a
 * raw index histogram (scheme-agnostic at decode time) and only named at
 * print time, in the human-readable (non-CSV) output -- see
 * print_ibs_sample() in ibs_sample.c.
 *
 * Draining only happens once, at end-of-run (read_counters()'s final
 * stop_counters==1 call in topdown.c) -- never from timer_callback()'s
 * SIGALRM handler, since walking the ring and decoding records isn't
 * async-signal-safe and there's no poll()/epoll loop anywhere in wspy to
 * hang a real-time drain off of. Consequence: --ibs-sample combined with
 * --interval produces zeroed periodic rows and one populated tail row
 * covering the whole run, not a genuine per-tick rate -- documented, not
 * silent. The ring is sized generously (IBS_SAMPLE_MMAP_DATA_PAGES) since
 * it tolerates not being drained until the run ends; PERF_RECORD_LOST
 * events are counted and surfaced (ibs_sample_state.samples_lost) rather
 * than silently producing an artificially low rate.
 */
#ifndef _WSPY_IBS_SAMPLE_H
#define _WSPY_IBS_SAMPLE_H 1

#include <stdint.h>

/* Uses FILE *outfile and enum output_format from wspy.h -- like ibs.h/
 * coverage.h, this header doesn't include wspy.h itself (see ibs.h's own
 * comment: wspy.h/cpu_info.h have no include guards, so each .c file
 * includes wspy.h exactly once, before any header depending on its types). */

/* Ring size: data pages only (a mandatory extra header page is added by
 * ibs_sample_mmap() itself). 64 data pages = 256 KiB on a 4 KiB-page host --
 * generous on purpose, since nothing drains the ring until end-of-run (see
 * file comment); must be a power of 2 per the perf mmap ABI. */
#define IBS_SAMPLE_MMAP_DATA_PAGES 64

/* IbsOpData2 data-source histogram width -- covers the documented index
 * range (0-12) of the richer zen4_ibs_extensions table; the older default
 * table only ever populates 0-7, a strict subset. Any index >= this
 * (reserved/undocumented on both known schemes) folds into
 * op_data_src_other_count rather than growing the array unbounded. */
#define IBS_SAMPLE_DATA_SRC_TABLE_SIZE 13

/* One mmap'd IBS sampling event's ring buffer + running decode stats.
 * Opaque to every caller except ibs_sample.c itself; cpu_info.h forward-
 * declares this exact type for counter_info.ibs_sample_state. */
struct ibs_sample_state {
  void *ring_base;   /* mmap() base: 1 header page + IBS_SAMPLE_MMAP_DATA_PAGES data pages */
  size_t ring_len;   /* total mmap length in bytes, for munmap() */
  int is_op;         /* 1 for ibs_op, 0 for ibs_fetch -- selects decode path */
  int drained;       /* 1 once ibs_sample_drain() has run for this state --
                       * guards against a second call double-counting. */

  unsigned long samples_seen;    /* PERF_RECORD_SAMPLE records decoded */
  unsigned long samples_lost;    /* summed PERF_RECORD_LOST .lost counts */
  unsigned long decode_skipped;  /* records shorter than this type's fixed prefix */

  /* Op-side aggregate counts (only touched when is_op==1) */
  unsigned long op_count;              /* denominator: total op samples decoded */
  unsigned long op_brn_ret_count;
  unsigned long op_brn_misp_count;
  unsigned long op_dc_miss_count;
  unsigned long op_dc_l1tlb_miss_count;
  unsigned long op_dc_l2tlb_miss_count;
  unsigned long op_dram_count;          /* IbsOpData2.data_src == 3 -- "DRAM", scheme-independent */
  unsigned long op_remote_node_count;   /* IbsOpData2.rmt_node bit -- scheme-independent */
  /* Raw data_src index histogram (0..IBS_SAMPLE_DATA_SRC_TABLE_SIZE-1) --
   * decode-time counting is scheme-agnostic; index 0 means "no source
   * recorded" (not a real category, see print_ibs_sample()), 1-12 are
   * named per-scheme only at print time. */
  unsigned long op_data_src_count[IBS_SAMPLE_DATA_SRC_TABLE_SIZE];
  unsigned long op_data_src_other_count; /* data_src >= table size (reserved/undocumented) */

  /* Fetch-side aggregate counts (only touched when is_op==0) */
  unsigned long fetch_count;           /* denominator: total fetch samples decoded */
  unsigned long fetch_ic_miss_count;
  unsigned long fetch_l1tlb_miss_count;
  unsigned long fetch_l2tlb_miss_count;
};

struct perf_event_attr;
/* Sets the fields that make an already-built counting-mode perf_event_attr
 * (from ibs_build_fetch_event()/ibs_build_op_unfiltered_event() in ibs.c)
 * into a sampling-mode one instead: sample_type=PERF_SAMPLE_RAW plus a
 * wakeup_events value. Call before perf_event_open(); mmap()'s expectations
 * (below) depend on sample_type being set this way first. */
void ibs_sample_attr_init(struct perf_event_attr *pe);

/* mmap()s fd (already perf_event_open()'d via ibs_sample_attr_init()'s
 * attr) and returns a freshly allocated, zeroed state, or NULL on mmap()
 * failure (logged via warning(), never fatal -- matches every other
 * counter's degrade-don't-abort convention). is_op selects which decode
 * path ibs_sample_drain() will use for this fd's records. */
struct ibs_sample_state *ibs_sample_mmap(int fd,int is_op);

void ibs_sample_free(struct ibs_sample_state *state);

/* Walks every unread record in state's ring buffer exactly once (idempotent
 * after the first call -- see .drained), decoding samples and counting
 * PERF_RECORD_LOST, then advances the consumer (data_tail) past what was
 * read. Safe to call only outside a signal handler -- see file comment.
 * No-op if state is NULL (mmap() failed earlier). */
void ibs_sample_drain(struct ibs_sample_state *state);

/* Pure decode: given nwords raw u64s starting at regs[0] (IbsOpCtl/
 * IbsFetchCtl) for one already-extracted record, with nwords the *actual*
 * word count present (may be less than the full known-field prefix for a
 * short/malformed record -- counted into decode_skipped, never read past
 * nwords), updates state's aggregate counts. No I/O; unit-testable with a
 * hand-built word array. */
void ibs_sample_decode_op(const uint64_t *words,int nwords,struct ibs_sample_state *state);
void ibs_sample_decode_fetch(const uint64_t *words,int nwords,struct ibs_sample_state *state);

/* struct counter_group/perf_type_id come from cpu_info.h; forward-declared
 * here per ibs.h's own pattern (this header doesn't include cpu_info.h).
 * struct ibs_profile_params is ibs.h's existing --ibs-maxcnt/--ibs-ldlat/
 * --ibs-fetchlat knob bundle, reused as-is (only .maxcnt applies here). */
struct counter_group;
struct ibs_profile_params;
/* Builds the counter_group for --ibs-sample: opens ibs_fetch+ibs_op as
 * sampling (not counting) events and mmaps their ring buffers. Returns NULL
 * (with a warning) if IBS isn't supported on this host/kernel, matching
 * ibs_counter_group()'s (ibs.c) degrade-don't-fail convention. */
struct counter_group *ibs_sample_counter_group(char *name,const struct ibs_profile_params *params);

void print_ibs_sample(struct counter_group *cgroup,enum output_format oformat);

#endif
