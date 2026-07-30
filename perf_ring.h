/*
 * perf_ring.h - Generic perf_event_open() sampling-mode mmap ring-buffer
 * plumbing, shared by every sampling-mode capture in wspy (AMD IBS's
 * ibs_sample.c; PERF_SAMPLE_IP-based symbol profiling's symbol_sample.c,
 * 4.4 priorities item 9). Owns exactly the parts that are identical
 * regardless of what a sample's payload means: the perf mmap ABI's
 * header-page/data-region layout, the data_head/data_tail barrier
 * ordering, and the wraparound-safe byte copy out of the ring. What a
 * PERF_RECORD_SAMPLE's payload bytes decode to (IBS: a u32 raw_size prefix
 * + caps + regs[] MSR words; symbol sampling: a bare u64 instruction
 * pointer) is entirely capture-specific and stays in each caller's own
 * file, reached via a callback.
 *
 * Draining is caller-triggered only, never from a signal handler --
 * walking the ring and decoding records isn't async-signal-safe (same
 * constraint ibs_sample.c's own file comment already documented before
 * this file existed). Idempotency (guarding a second drain from
 * double-counting) is each caller's own responsibility via its own
 * "drained" flag -- this file has no state of its own between calls.
 */
#ifndef _WSPY_PERF_RING_H
#define _WSPY_PERF_RING_H 1

#include <stdint.h>
#include <stddef.h>

/* Longest PERF_RECORD_SAMPLE payload perf_ring_drain() hands a callback in
 * one piece; a record whose payload is longer gets truncated to this many
 * bytes for decode purposes (the ring's own consumer pointer still advances
 * by the record's real, unclamped size, so no later record is
 * misaligned). 160 bytes covers IBS's own worst case (4-byte raw_size +
 * 4-byte caps + 16 regs[] words = 136 bytes, see ibs_sample.h) with
 * headroom; symbol sampling only ever needs 8 (one u64 IP). */
#define PERF_RING_MAX_PAYLOAD 160

/* mmap()s fd (already perf_event_open()'d in sampling mode) as 1 mandatory
 * header page plus data_pages data pages (must be a power of 2, perf mmap
 * ABI requirement); returns the mmap() base and, via *ring_len_out, the
 * total mapped length (needed later for perf_ring_unmap()). Returns NULL on
 * mmap() failure -- logged via warning() using label to identify which ring
 * failed (e.g. "ibs_op", "symbol_sample"), matching every other counter's
 * degrade-don't-abort convention; *ring_len_out is left unmodified on
 * failure. */
void *perf_ring_mmap(int fd,int data_pages,const char *label,size_t *ring_len_out);

void perf_ring_unmap(void *ring_base,size_t ring_len);

/* Receives one PERF_RECORD_SAMPLE's payload -- the bytes immediately after
 * that record's perf_event_header, copied into a bounded scratch buffer (up
 * to PERF_RING_MAX_PAYLOAD bytes; payload_len is the actual, possibly
 * truncated, copied length). ctx is passed through unchanged from
 * perf_ring_drain(). */
typedef void (*perf_ring_sample_cb)(const uint8_t *payload,uint32_t payload_len,void *ctx);

/* Walks every unread record in ring_base's ring buffer exactly once,
 * calling cb for each PERF_RECORD_SAMPLE (returns the count seen) and
 * accumulating PERF_RECORD_LOST .lost fields into *lost_count (left
 * untouched if NULL), then advances the consumer (data_tail) past what was
 * read so a second call on the same ring sees nothing new. No-op (returns
 * 0) if ring_base is NULL -- caller's own mmap() may have failed earlier. */
uint64_t perf_ring_drain(void *ring_base,size_t ring_len,perf_ring_sample_cb cb,void *ctx,uint64_t *lost_count);

#endif
