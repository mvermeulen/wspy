/*
 * ibs_sample.c - AMD IBS sampling-mode capture, described in ibs_sample.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mman.h>
#include <linux/perf_event.h>
#include "wspy.h"
#include "error.h"
#include "ibs.h"
#include "ibs_sample.h"

void ibs_sample_attr_init(struct perf_event_attr *pe){
  pe->sample_type = PERF_SAMPLE_RAW;
  // Draining only ever happens at end-of-run (see ibs_sample.h's file
  // comment), so there's no real-time wakeup to tune -- 1 just keeps the
  // kernel from silently defaulting to "never wake up" on some configs.
  pe->wakeup_events = 1;
}

struct ibs_sample_state *ibs_sample_mmap(int fd,int is_op){
  struct ibs_sample_state *state;
  long page_size;
  size_t ring_len;
  void *base;

  page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0) page_size = 4096;
  ring_len = (size_t)page_size * (1 + IBS_SAMPLE_MMAP_DATA_PAGES);

  base = mmap(NULL,ring_len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
  if (base == MAP_FAILED){
    warning("unable to mmap IBS sampling ring buffer (%s), errno=%d - %s\n",
	    is_op ? "ibs_op" : "ibs_fetch",errno,strerror(errno));
    return NULL;
  }

  state = calloc(1,sizeof(*state));
  state->ring_base = base;
  state->ring_len = ring_len;
  state->is_op = is_op;
  return state;
}

void ibs_sample_free(struct ibs_sample_state *state){
  if (!state) return;
  if (state->ring_base) munmap(state->ring_base,state->ring_len);
  free(state);
}

// Copies len bytes starting at the ring-relative byte offset "offset"
// (a monotonically increasing stream position, per the perf mmap ABI --
// not yet reduced mod data_size) out of the ring's data region into dest,
// handling the wraparound case where the read straddles the end of the
// physical buffer.
static void ring_read(const uint8_t *data_base,uint64_t data_size,uint64_t offset,void *dest,uint64_t len){
  uint64_t pos = offset % data_size;
  uint64_t first = data_size - pos;

  if (first >= len){
    memcpy(dest,data_base+pos,len);
  } else {
    memcpy(dest,data_base+pos,first);
    memcpy((uint8_t *)dest+first,data_base,len-first);
  }
}

// Fixed-size scratch buffer for one record's regs[] words. 16 words (128
// bytes) comfortably covers every known IBS op/fetch record layout
// (including every optional cap-gated word AMD has documented to date) even
// though this PR's decode functions only read the first few -- see
// ibs_sample.h's "minimal decode scope" comment. A record with more words
// than this (a future, wider IBS generation) just gets truncated to the
// fields this code already knows how to read; the ring's own consumer
// pointer still advances by the record's real, unclamped size.
#define IBS_SAMPLE_MAX_WORDS 16

void ibs_sample_drain(struct ibs_sample_state *state){
  struct perf_event_mmap_page *meta;
  uint8_t *data_base;
  uint64_t data_size;
  uint64_t head,tail;
  long page_size;

  if (!state || !state->ring_base || state->drained) return;
  state->drained = 1;

  meta = (struct perf_event_mmap_page *)state->ring_base;
  if (meta->data_size){
    data_base = (uint8_t *)state->ring_base + meta->data_offset;
    data_size = meta->data_size;
  } else {
    // Pre-4.1 kernel fallback: data region is exactly the pages after the
    // one mandatory header page, in the same single mmap() call.
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    data_base = (uint8_t *)state->ring_base + page_size;
    data_size = (uint64_t)state->ring_len - (uint64_t)page_size;
  }

  head = meta->data_head;
  __sync_synchronize(); // perf mmap ABI: must read data_head before the data it bounds
  tail = meta->data_tail;

  while (tail < head){
    struct perf_event_header hdr;

    if (head - tail < sizeof(hdr)) break; // partial header at the end -- nothing more to read
    ring_read(data_base,data_size,tail,&hdr,sizeof(hdr));
    if (hdr.size < sizeof(hdr)) break; // malformed record -- stop rather than loop forever

    if (hdr.type == PERF_RECORD_SAMPLE){
      uint32_t raw_size = 0;
      state->samples_seen++;
      if (hdr.size >= sizeof(hdr)+sizeof(raw_size))
	ring_read(data_base,data_size,tail+sizeof(hdr),&raw_size,sizeof(raw_size));
      // raw_size covers a leading 4-byte "caps" snapshot (struct
      // perf_ibs_data, arch/x86/include/asm/amd/ibs.h) followed by the
      // regs[] words themselves -- strip it to get to regs[0].
      if (raw_size > sizeof(uint32_t)){
	uint32_t regs_bytes = raw_size - sizeof(uint32_t);
	int nwords = (int)(regs_bytes / sizeof(uint64_t));
	if (nwords > 0){
	  uint64_t words[IBS_SAMPLE_MAX_WORDS];
	  int copy_words = nwords > IBS_SAMPLE_MAX_WORDS ? IBS_SAMPLE_MAX_WORDS : nwords;
	  ring_read(data_base,data_size,
		    tail+sizeof(hdr)+sizeof(raw_size)+sizeof(uint32_t),
		    words,(uint64_t)copy_words*sizeof(uint64_t));
	  if (state->is_op) ibs_sample_decode_op(words,copy_words,state);
	  else ibs_sample_decode_fetch(words,copy_words,state);
	} else {
	  state->decode_skipped++;
	}
      } else {
	state->decode_skipped++;
      }
    } else if (hdr.type == PERF_RECORD_LOST){
      // struct { perf_event_header; u64 id; u64 lost; } -- see perf_event.h
      uint64_t lost_fields[2] = {0,0};
      if (hdr.size >= sizeof(hdr)+sizeof(lost_fields))
	ring_read(data_base,data_size,tail+sizeof(hdr),lost_fields,sizeof(lost_fields));
      state->samples_lost += lost_fields[1];
    }

    tail += hdr.size;
  }

  meta->data_tail = tail;
  __sync_synchronize(); // perf mmap ABI: must publish data_tail after we're done reading
}

void ibs_sample_decode_op(const uint64_t *words,int nwords,struct ibs_sample_state *state){
  if (!state || nwords <= 0) return;
  state->op_count++;

  if (nwords > 2){ // words[2] = IbsOpData
    uint64_t op_data = words[2];
    unsigned int op_brn_ret = (op_data >> 37) & 0x1;
    // op_brn_misp is only architecturally meaningful when op_brn_ret is
    // set (confirmed against the kernel's own amd-sample-raw.c decoder:
    // it gates OpBrnMisp display on op_brn_ret the same way) -- so the
    // branch-misprediction rate's denominator is op_brn_ret_count, not
    // op_count.
    if (op_brn_ret){
      unsigned int op_brn_misp = (op_data >> 36) & 0x1;
      state->op_brn_ret_count++;
      if (op_brn_misp) state->op_brn_misp_count++;
    }
  }

  if (nwords > 4){ // words[4] = IbsOpData3
    uint64_t op_data3 = words[4];
    if ((op_data3 >> 7) & 0x1) state->op_dc_miss_count++;
    if ((op_data3 >> 2) & 0x1) state->op_dc_l1tlb_miss_count++;
    if ((op_data3 >> 3) & 0x1) state->op_dc_l2tlb_miss_count++;
  }
}

void ibs_sample_decode_fetch(const uint64_t *words,int nwords,struct ibs_sample_state *state){
  uint64_t fetch_ctl;

  if (!state || nwords <= 0) return;
  state->fetch_count++;

  fetch_ctl = words[0]; // IbsFetchCtl
  if ((fetch_ctl >> 51) & 0x1) state->fetch_ic_miss_count++;
  if ((fetch_ctl >> 55) & 0x1) state->fetch_l1tlb_miss_count++;
  if ((fetch_ctl >> 56) & 0x1) state->fetch_l2tlb_miss_count++;
}

struct counter_group *ibs_sample_counter_group(char *name,const struct ibs_profile_params *params){
  struct ibs_capabilities caps;
  struct ibs_event fetch_ev,op_ev;
  struct counter_group *cgroup;
  int idx;

  caps = ibs_probe();
  if (!caps.supported){
    warning("AMD IBS not supported on this host/kernel -- --ibs-sample produces no counters\n");
    return NULL;
  }

  // Reuse ibs.c's existing type/config-word assembly (unfiltered --
  // sampling mode has no use for ibs-memory-deep's l3missonly/ldlat
  // counting-mode filters, which shape which counting events get opened,
  // not sampling records) rather than duplicating format-field parsing
  // here. IBS_PROFILE_NONE only skips the l3missonly/ldlat/fetchlat
  // filtering block inside ibs_build_fetch_event()/ibs_build_op_event() --
  // ev.valid/type/sample_period are still set unconditionally whenever the
  // PMU is present, so this is safe (ibs_counter_group()'s own
  // "profile==NONE means nothing at all" short-circuit lives one layer up,
  // in ibs.c's ibs_counter_group(), not in these builder functions).
  fetch_ev = ibs_build_fetch_event(&caps.fetch,IBS_PROFILE_NONE,params);
  op_ev = ibs_build_op_unfiltered_event(&caps.op,params);

  if (!fetch_ev.valid && !op_ev.valid) return NULL;

  cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  cgroup->type_id = PERF_TYPE_RAW;
  cgroup->mask = COUNTER_IBS_SAMPLE;
  cgroup->ncounters = fetch_ev.valid + op_ev.valid;
  cgroup->cinfo = calloc(cgroup->ncounters,sizeof(struct counter_info));

  idx = 0;
  if (fetch_ev.valid){
    cgroup->cinfo[idx].label = strdup("ibs_sample_fetch");
    cgroup->cinfo[idx].device_type = fetch_ev.type;
    cgroup->cinfo[idx].config = fetch_ev.config;
    cgroup->cinfo[idx].config1 = fetch_ev.config1;
    cgroup->cinfo[idx].config2 = fetch_ev.config2;
    cgroup->cinfo[idx].sample_period = fetch_ev.sample_period;
    cgroup->cinfo[idx].is_group_leader = 1;
    cgroup->cinfo[idx].requires_system_wide = 1; // IBS is system-wide only, same as counting-mode (ibs.c)
    cgroup->cinfo[idx].is_ibs_sample = 1;
    idx++;
  }
  if (op_ev.valid){
    cgroup->cinfo[idx].label = strdup("ibs_sample_op");
    cgroup->cinfo[idx].device_type = op_ev.type;
    cgroup->cinfo[idx].config = op_ev.config;
    cgroup->cinfo[idx].config1 = op_ev.config1;
    cgroup->cinfo[idx].config2 = op_ev.config2;
    cgroup->cinfo[idx].sample_period = op_ev.sample_period;
    cgroup->cinfo[idx].is_group_leader = 1;
    cgroup->cinfo[idx].requires_system_wide = 1;
    cgroup->cinfo[idx].is_ibs_sample = 1;
    idx++;
  }
  return cgroup;
}

static double sample_rate(unsigned long numerator,unsigned long denominator){
  if (denominator == 0) return 0.0;
  return (double)numerator / (double)denominator;
}

// Deliberately not topdown.c's find_ci_label() -- that would pull a
// cross-translation-unit dependency on topdown.c into ibs_sample.c purely
// for this one lookup, which would also drag test_ibs_sample's build (see
// Makefile, same minimal-link convention as test_ibs) into needing to stub
// or link topdown.c. A local linear scan by label is all this needs.
static struct counter_info *ibs_sample_find(struct counter_group *cgroup,const char *label){
  int i;
  for (i=0;i<cgroup->ncounters;i++)
    if (!strcmp(cgroup->cinfo[i].label,label)) return &cgroup->cinfo[i];
  return NULL;
}

// Prints per-sample-decoded rate estimates from AMD IBS *sampling* mode --
// see ibs_sample.h for the wire format/decode-scope background and for why
// these numbers only reflect the whole run (one drain at end-of-run, not a
// real per-tick rate): a --ibs-sample --interval run's periodic rows show
// every column below as 0; only the final tail row is populated.
void print_ibs_sample(struct counter_group *cgroup,enum output_format oformat){
  struct counter_info *fetch_ci = ibs_sample_find(cgroup,"ibs_sample_fetch");
  struct counter_info *op_ci = ibs_sample_find(cgroup,"ibs_sample_op");
  struct ibs_sample_state *fs = fetch_ci ? fetch_ci->ibs_sample_state : NULL;
  struct ibs_sample_state *os = op_ci ? op_ci->ibs_sample_state : NULL;
  unsigned long samples_lost = (fs ? fs->samples_lost : 0) + (os ? os->samples_lost : 0);

  if (oformat == PRINT_CSV_HEADER){
    fprintf(outfile,"ibs_sample_fetch_count,ibs_sample_ic_miss_rate,ibs_sample_l1tlb_miss_rate,"
	    "ibs_sample_l2tlb_miss_rate,ibs_sample_op_count,ibs_sample_dc_miss_rate,"
	    "ibs_sample_dc_l1tlb_miss_rate,ibs_sample_dc_l2tlb_miss_rate,ibs_sample_brn_misp_rate,"
	    "ibs_sample_lost,");
    return;
  }

  if (oformat == PRINT_CSV){
    fprintf(outfile,"%lu,%.4f,%.4f,%.4f,%lu,%.4f,%.4f,%.4f,%.4f,%lu,",
	    fs ? fs->fetch_count : 0,
	    fs ? sample_rate(fs->fetch_ic_miss_count,fs->fetch_count) : 0.0,
	    fs ? sample_rate(fs->fetch_l1tlb_miss_count,fs->fetch_count) : 0.0,
	    fs ? sample_rate(fs->fetch_l2tlb_miss_count,fs->fetch_count) : 0.0,
	    os ? os->op_count : 0,
	    os ? sample_rate(os->op_dc_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_dc_l1tlb_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_dc_l2tlb_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_brn_misp_count,os->op_brn_ret_count) : 0.0,
	    samples_lost);
    return;
  }

  fprintf(outfile,"ibs_sample_fetch_count     %-10lu\n",fs ? fs->fetch_count : 0);
  if (fs && fs->fetch_count){
    fprintf(outfile,"ibs_sample_ic_miss_rate    %5.1f%%\n",sample_rate(fs->fetch_ic_miss_count,fs->fetch_count)*100.0);
    fprintf(outfile,"ibs_sample_l1tlb_miss_rate %5.1f%%\n",sample_rate(fs->fetch_l1tlb_miss_count,fs->fetch_count)*100.0);
    fprintf(outfile,"ibs_sample_l2tlb_miss_rate %5.1f%%\n",sample_rate(fs->fetch_l2tlb_miss_count,fs->fetch_count)*100.0);
  }
  fprintf(outfile,"ibs_sample_op_count        %-10lu\n",os ? os->op_count : 0);
  if (os && os->op_count){
    fprintf(outfile,"ibs_sample_dc_miss_rate       %5.1f%%\n",sample_rate(os->op_dc_miss_count,os->op_count)*100.0);
    fprintf(outfile,"ibs_sample_dc_l1tlb_miss_rate %5.1f%%\n",sample_rate(os->op_dc_l1tlb_miss_count,os->op_count)*100.0);
    fprintf(outfile,"ibs_sample_dc_l2tlb_miss_rate %5.1f%%\n",sample_rate(os->op_dc_l2tlb_miss_count,os->op_count)*100.0);
    if (os->op_brn_ret_count)
      fprintf(outfile,"ibs_sample_brn_misp_rate      %5.1f%%          # of %lu branch-retiring ops\n",
	      sample_rate(os->op_brn_misp_count,os->op_brn_ret_count)*100.0,os->op_brn_ret_count);
  }
  if (samples_lost)
    fprintf(outfile,"# warning: %lu IBS sample(s) lost (ring buffer overrun) -- rates above are a lower bound\n",samples_lost);
  fprintf(outfile,"# note: --ibs-sample rates are computed once at end-of-run; combined with --interval, periodic rows show 0 here and only the final row is populated (see INVESTIGATION.md's 4.3 \"AMD IBS sampling-mode support\")\n");
}
