/*
 * symbol_sample.c - see symbol_sample.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <linux/perf_event.h>
#include "wspy.h"
#include "error.h"
#include "symbol_sample.h"
#include "perf_ring.h"

void symbol_sample_attr_init(struct perf_event_attr *pe){
  pe->sample_type = PERF_SAMPLE_IP;
  // Draining only ever happens at this pid's own exit (see file comment),
  // so there's no real-time wakeup to tune -- 1 just keeps the kernel from
  // silently defaulting to "never wake up" on some configs. Same rationale
  // ibs_sample_attr_init() already uses.
  pe->wakeup_events = 1;
}

struct symbol_sample_state *symbol_sample_mmap(int fd){
  struct symbol_sample_state *state;
  void *base;
  size_t ring_len;

  base = perf_ring_mmap(fd,SYMBOL_SAMPLE_MMAP_DATA_PAGES,"symbol_sample",&ring_len);
  if (!base) return NULL;

  state = calloc(1,sizeof(*state));
  state->ring_base = base;
  state->ring_len = ring_len;
  state->addrs_cap = SYMBOL_SAMPLE_INITIAL_CAPACITY;
  state->addrs = calloc((size_t)state->addrs_cap,sizeof(*state->addrs));
  return state;
}

void symbol_sample_free(struct symbol_sample_state *state){
  if (!state) return;
  perf_ring_unmap(state->ring_base,state->ring_len);
  free(state->addrs);
  free(state);
}

void symbol_sample_record_addr(struct symbol_sample_state *state,uint64_t addr){
  int i;

  if (!state) return;

  for (i=0;i<state->naddrs;i++){
    if (state->addrs[i].addr == addr){
      state->addrs[i].count++;
      return;
    }
  }

  if (state->naddrs == state->addrs_cap){
    int new_cap = state->addrs_cap > 0 ? state->addrs_cap*2 : SYMBOL_SAMPLE_INITIAL_CAPACITY;
    struct symbol_sample_addr_count *grown = realloc(state->addrs,(size_t)new_cap*sizeof(*grown));
    if (!grown) return; // out of memory -- drop this sample rather than crash; samples_seen still counts it
    state->addrs = grown;
    state->addrs_cap = new_cap;
  }

  state->addrs[state->naddrs].addr = addr;
  state->addrs[state->naddrs].count = 1;
  state->naddrs++;
}

// perf_ring_drain() callback: ctx is the struct symbol_sample_state being
// drained. A PERF_SAMPLE_IP-only record's payload is exactly one u64 (the
// interrupted instruction pointer) -- no raw_size prefix or other fields,
// unlike ibs_sample.c's PERF_SAMPLE_RAW payloads.
static void symbol_sample_record_cb(const uint8_t *payload,uint32_t payload_len,void *ctx){
  struct symbol_sample_state *state = (struct symbol_sample_state *)ctx;
  uint64_t ip;

  if (payload_len < sizeof(ip)){
    state->decode_skipped++;
    return;
  }
  memcpy(&ip,payload,sizeof(ip));
  symbol_sample_record_addr(state,ip);
}

void symbol_sample_drain(struct symbol_sample_state *state){
  if (!state || !state->ring_base || state->drained) return;
  state->drained = 1;
  state->samples_seen = perf_ring_drain(state->ring_base,state->ring_len,
					 symbol_sample_record_cb,state,&state->samples_lost);
}

// event -> (PERF_TYPE_HARDWARE config, human-readable name, default
// sample_period). Generic PERF_TYPE_HARDWARE events rather than vendor raw
// events -- portable across AMD/Intel/ARM with no per-vendor table, the
// right tradeoff for a small curated first-cut list (see wspy.h's enum
// symbol_sample_event comment). Periods are a first-cut heuristic, not
// frequency-normalized (this project has no per-counter frequency-based
// sampling plumbing yet, only the fixed sample_period AMD IBS already
// uses -- see topdown.c's setup_counters()): cycles/instructions fire on
// nearly every cycle, so a period in the ~10^6 range still samples several
// hundred to a few thousand times/sec at typical clocks; cache-misses/
// branch-misses are far rarer events per cycle, so a much smaller period
// is needed to get any samples at all from a short-lived process.
struct symbol_sample_event_info {
  unsigned int config;
  const char *name;
  unsigned long default_period;
};

static const struct symbol_sample_event_info symbol_sample_events[] = {
  [SYMBOL_SAMPLE_EVENT_CYCLES]        = { PERF_COUNT_HW_CPU_CYCLES,    "cycles",        1000000 },
  [SYMBOL_SAMPLE_EVENT_INSTRUCTIONS]  = { PERF_COUNT_HW_INSTRUCTIONS,  "instructions",  1000000 },
  [SYMBOL_SAMPLE_EVENT_CACHE_MISSES]  = { PERF_COUNT_HW_CACHE_MISSES,  "cache-misses",  10000 },
  [SYMBOL_SAMPLE_EVENT_BRANCH_MISSES] = { PERF_COUNT_HW_BRANCH_MISSES, "branch-misses", 10000 },
};
#define SYMBOL_SAMPLE_NEVENTS (sizeof(symbol_sample_events)/sizeof(symbol_sample_events[0]))

int symbol_sample_parse_event(const char *arg,enum symbol_sample_event *out){
  size_t i;

  if (!arg || !*arg){
    *out = SYMBOL_SAMPLE_EVENT_CYCLES;
    return 0;
  }
  for (i=0;i<SYMBOL_SAMPLE_NEVENTS;i++){
    if (!strcmp(arg,symbol_sample_events[i].name)){
      *out = (enum symbol_sample_event)i;
      return 0;
    }
  }
  return -1;
}

const char *symbol_sample_event_name(enum symbol_sample_event event){
  if ((size_t)event >= SYMBOL_SAMPLE_NEVENTS) return "unknown";
  return symbol_sample_events[event].name;
}

struct counter_group *symbol_sample_counter_group(char *name,enum symbol_sample_event event){
  struct counter_group *cgroup;
  const struct symbol_sample_event_info *ev;

  if ((size_t)event >= SYMBOL_SAMPLE_NEVENTS) return NULL;
  ev = &symbol_sample_events[event];

  cgroup = calloc(1,sizeof(struct counter_group));
  cgroup->label = strdup(name);
  cgroup->type_id = PERF_TYPE_HARDWARE;
  cgroup->target_cpu = -1;
  cgroup->ncounters = 1;
  cgroup->cinfo = calloc(1,sizeof(struct counter_info));
  cgroup->cinfo[0].label = strdup(ev->name);
  cgroup->cinfo[0].config = ev->config;
  cgroup->cinfo[0].sample_period = ev->default_period;
  cgroup->cinfo[0].is_group_leader = 1;
  cgroup->cinfo[0].is_symbol_sample = 1;
  return cgroup;
}
