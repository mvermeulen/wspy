/*
 * ibs_sample.c - AMD IBS sampling-mode capture, described in ibs_sample.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <linux/perf_event.h>
#include "wspy.h"
#include "error.h"
#include "ibs.h"
#include "ibs_sample.h"
#include "perf_ring.h"

void ibs_sample_attr_init(struct perf_event_attr *pe){
  pe->sample_type = PERF_SAMPLE_RAW;
  // Draining only ever happens at end-of-run (see ibs_sample.h's file
  // comment), so there's no real-time wakeup to tune -- 1 just keeps the
  // kernel from silently defaulting to "never wake up" on some configs.
  pe->wakeup_events = 1;
}

struct ibs_sample_state *ibs_sample_mmap(int fd,int is_op){
  struct ibs_sample_state *state;
  void *base;
  size_t ring_len;

  base = perf_ring_mmap(fd,IBS_SAMPLE_MMAP_DATA_PAGES,is_op ? "ibs_op" : "ibs_fetch",&ring_len);
  if (!base) return NULL;

  state = calloc(1,sizeof(*state));
  state->ring_base = base;
  state->ring_len = ring_len;
  state->is_op = is_op;
  return state;
}

void ibs_sample_free(struct ibs_sample_state *state){
  if (!state) return;
  perf_ring_unmap(state->ring_base,state->ring_len);
  free(state);
}

// Fixed-size scratch buffer for one record's regs[] words. 16 words (128
// bytes) comfortably covers every known IBS op/fetch record layout
// (including every optional cap-gated word AMD has documented to date) even
// though this PR's decode functions only read the first few -- see
// ibs_sample.h's "minimal decode scope" comment. A record with more words
// than this (a future, wider IBS generation) just gets truncated to the
// fields this code already knows how to read; perf_ring_drain()'s own
// consumer pointer still advances by the record's real, unclamped size.
#define IBS_SAMPLE_MAX_WORDS 16

// perf_ring_drain() callback: ctx is the struct ibs_sample_state being
// drained. payload is [raw_size:4][caps:4][regs[]...] per the wire format
// documented in ibs_sample.h -- raw_size covers caps+regs (not itself), so
// regs[] starts 8 bytes into payload, not 4.
static void ibs_sample_record_cb(const uint8_t *payload,uint32_t payload_len,void *ctx){
  struct ibs_sample_state *state = (struct ibs_sample_state *)ctx;
  uint32_t raw_size = 0;
  uint32_t regs_offset = 2*sizeof(uint32_t); // skip the raw_size field itself + the 4-byte caps snapshot

  if (payload_len >= sizeof(raw_size))
    memcpy(&raw_size,payload,sizeof(raw_size));

  if (raw_size > sizeof(uint32_t) && payload_len > regs_offset){
    uint32_t regs_bytes = raw_size - sizeof(uint32_t); // raw_size covers caps(4 bytes)+regs[]; strip caps
    int nwords = (int)(regs_bytes / sizeof(uint64_t));
    int avail_words = (int)((payload_len - regs_offset) / sizeof(uint64_t));

    if (nwords > avail_words) nwords = avail_words; // truncated by PERF_RING_MAX_PAYLOAD, or a short record
    if (nwords > 0){
      uint64_t words[IBS_SAMPLE_MAX_WORDS];
      int copy_words = nwords > IBS_SAMPLE_MAX_WORDS ? IBS_SAMPLE_MAX_WORDS : nwords;
      memcpy(words,payload+regs_offset,(size_t)copy_words*sizeof(uint64_t));
      if (state->is_op) ibs_sample_decode_op(words,copy_words,state);
      else ibs_sample_decode_fetch(words,copy_words,state);
      return;
    }
  }
  state->decode_skipped++;
}

void ibs_sample_drain(struct ibs_sample_state *state){
  if (!state || !state->ring_base || state->drained) return;
  state->drained = 1;
  state->samples_seen = perf_ring_drain(state->ring_base,state->ring_len,
					 ibs_sample_record_cb,state,&state->samples_lost);
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

  if (nwords > 3){ // words[3] = IbsOpData2
    uint64_t op_data2 = words[3];
    unsigned int data_src_lo = op_data2 & 0x7;
    unsigned int data_src_hi = (op_data2 >> 6) & 0x3;
    unsigned int rmt_node = (op_data2 >> 4) & 0x1;
    // Combined 5-bit data-source index -- scheme-dependent naming (see
    // ibs_sample.h's file comment), but the combination itself and the two
    // signals extracted below (data_src==3, rmt_node) are not.
    unsigned int data_src = (data_src_hi << 3) | data_src_lo;

    if (rmt_node) state->op_remote_node_count++;
    if (data_src == 3) state->op_dram_count++;
    if (data_src < IBS_SAMPLE_DATA_SRC_TABLE_SIZE) state->op_data_src_count[data_src]++;
    else state->op_data_src_other_count++;
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

// IbsOpData2 data-source category names, transcribed verbatim from the
// kernel's own decoder (tools/perf/util/amd-sample-raw.c's
// pr_ibs_op_data2_default()/pr_ibs_op_data2_extended() string tables) --
// not re-derived. NULL entries are reserved/undocumented indices, folded
// into "(reserved/unrecognized)" at print time; index 0 ("no source
// recorded") is skipped entirely rather than treated as reserved -- see
// ibs_sample.h's file comment on why cross-scheme category names never
// reach the CSV schema, only this human-readable breakdown.
static const char *const ibs_data_src_names_default[IBS_SAMPLE_DATA_SRC_TABLE_SIZE] = {
  NULL,                  // 0: no source recorded
  NULL,                  // 1: reserved
  "Local node cache",    // 2
  "DRAM",                // 3
  "Remote node cache",   // 4
  NULL,                  // 5: reserved
  NULL,                  // 6: reserved
  "Other",               // 7
  NULL,NULL,NULL,NULL,NULL, // 8-12: not defined in the default (pre-Zen4) scheme
};

static const char *const ibs_data_src_names_zen4_ext[IBS_SAMPLE_DATA_SRC_TABLE_SIZE] = {
  NULL,                                             // 0: no source recorded
  "Local L3 or other L1/L2 in CCX",                 // 1
  "Another CCX cache in the same NUMA node",        // 2
  "DRAM",                                           // 3
  NULL,                                             // 4: reserved
  "Another CCX cache in a different NUMA node",     // 5
  "Long-latency DIMM",                              // 6
  "MMIO/Config/PCI/APIC",                           // 7
  "Extension Memory",                               // 8
  NULL,NULL,NULL,                                   // 9-11: reserved
  "Coherent Memory of a different processor type",  // 12
};

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

// Prints the full named IbsOpData2 data-source breakdown -- human-readable
// output only, never CSV (see ibs_sample.h's file comment on why). Picks
// the default vs zen4_ibs_extensions name table by recomputing ibs_probe()
// fresh, the same "keeps this print path independent of any extra state"
// precedent topdown.c's print_ibs() already established for its own
// requested-vs-applied annotations.
static void print_ibs_sample_data_src_breakdown(struct ibs_sample_state *os){
  struct ibs_capabilities caps;
  const struct ibs_cap *ext_cap;
  int zen4_ext;
  const char *const *names;
  unsigned long other_total;
  int i;

  caps = ibs_probe();
  ext_cap = ibs_pmu_cap(&caps.op,"zen4_ibs_extensions");
  zen4_ext = ext_cap && ext_cap->enabled;
  names = zen4_ext ? ibs_data_src_names_zen4_ext : ibs_data_src_names_default;

  fprintf(outfile,"ibs_sample_data_src_breakdown (scheme: %s):\n",zen4_ext ? "zen4_ibs_extensions" : "default");
  other_total = os->op_data_src_other_count;
  for (i = 1; i < IBS_SAMPLE_DATA_SRC_TABLE_SIZE; i++){ // skip index 0 -- "no source recorded", not a category
    if (!os->op_data_src_count[i]) continue;
    if (!names[i]){
      other_total += os->op_data_src_count[i];
      continue;
    }
    fprintf(outfile,"  %-46s %5.1f%%\n",names[i],sample_rate(os->op_data_src_count[i],os->op_count)*100.0);
  }
  if (other_total)
    fprintf(outfile,"  %-46s %5.1f%%\n","(reserved/unrecognized)",sample_rate(other_total,os->op_count)*100.0);
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
	    "ibs_sample_lost,ibs_sample_dram_rate,ibs_sample_remote_node_rate,");
    return;
  }

  if (oformat == PRINT_CSV){
    fprintf(outfile,"%lu,%.4f,%.4f,%.4f,%lu,%.4f,%.4f,%.4f,%.4f,%lu,%.4f,%.4f,",
	    fs ? fs->fetch_count : 0,
	    fs ? sample_rate(fs->fetch_ic_miss_count,fs->fetch_count) : 0.0,
	    fs ? sample_rate(fs->fetch_l1tlb_miss_count,fs->fetch_count) : 0.0,
	    fs ? sample_rate(fs->fetch_l2tlb_miss_count,fs->fetch_count) : 0.0,
	    os ? os->op_count : 0,
	    os ? sample_rate(os->op_dc_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_dc_l1tlb_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_dc_l2tlb_miss_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_brn_misp_count,os->op_brn_ret_count) : 0.0,
	    samples_lost,
	    os ? sample_rate(os->op_dram_count,os->op_count) : 0.0,
	    os ? sample_rate(os->op_remote_node_count,os->op_count) : 0.0);
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
    fprintf(outfile,"ibs_sample_dram_rate          %5.1f%%\n",sample_rate(os->op_dram_count,os->op_count)*100.0);
    fprintf(outfile,"ibs_sample_remote_node_rate   %5.1f%%\n",sample_rate(os->op_remote_node_count,os->op_count)*100.0);
    print_ibs_sample_data_src_breakdown(os);
  }
  if (samples_lost)
    fprintf(outfile,"# warning: %lu IBS sample(s) lost (ring buffer overrun) -- rates above are a lower bound\n",samples_lost);
  fprintf(outfile,"# note: --ibs-sample rates are computed once at end-of-run; combined with --interval, periodic rows show 0 here and only the final row is populated (see INVESTIGATION.md's 4.3 \"AMD IBS sampling-mode support\")\n");
}
