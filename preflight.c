/*
 * preflight.c - counter-fit preflight, described in preflight.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wspy.h"
#include "error.h"
#include "preflight.h"

/* Same "general purpose HW PMU slots" heuristic already used by topdown.c's
 * AMD raw/cache counter-group chunking (cache_counter_group()/
 * raw_counter_group()'s num_counters_available/available_counters) -- kept
 * in sync with that existing assumption rather than inventing a second
 * number that could disagree with it. Not per-model-precise (real
 * general-purpose counter counts vary by CPU generation), but it's the best
 * estimate this codebase already has. */
static int preflight_available_slots(void){
  return nmi_running ? 5 : 6;
}

/* --no-<flag> (or, where no such flag exists yet, a plain-language
 * equivalent) for each COUNTER_* bit a raw/cache counter group can carry.
 * Matches wspy.c:parse_options()'s long_options table -- if that table
 * changes, keep this in sync. */
static const struct { unsigned int bit; const char *hint; } counter_disable_hints[] = {
  { COUNTER_IPC,        "--no-ipc" },
  { COUNTER_TOPDOWN,     "--no-topdown" },
  { COUNTER_TOPDOWN2,    "--no-topdown2" },
  { COUNTER_TOPDOWN_FE,  "--no-topdown-frontend" },
  { COUNTER_TOPDOWN_BE,  "--no-topdown-backend" },
  { COUNTER_TOPDOWN_OP,  "--no-topdown-optlb" },
  { COUNTER_BRANCH,      "--no-branch" },
  { COUNTER_DCACHE,      "--no-cache" },
  { COUNTER_ICACHE,      "--no-icache" },
  { COUNTER_L1CACHE,     "--no-cache1" },
  { COUNTER_L2CACHE,     "--no-cache2" },
  { COUNTER_L3CACHE,     "--no-cache3" },
  { COUNTER_MEMORY,      "--no-memory" },
  { COUNTER_TLB,         "--no-tlb" },
  { COUNTER_OPCACHE,     "--no-opcache" },
  { COUNTER_FLOAT,       "(omit --float; no --no-float flag exists yet)" },
};
#define N_COUNTER_DISABLE_HINTS (sizeof(counter_disable_hints)/sizeof(counter_disable_hints[0]))

/* Joins the disable hint(s) for every bit set in mask into a static buffer
 * (single-threaded CLI, like the rest of this codebase's use of static
 * scratch buffers -- see e.g. amd_sysfs.c). */
static const char *disable_hints_for(unsigned int mask){
  static char buf[256];
  unsigned int i;
  size_t used = 0;

  buf[0] = 0;
  for (i=0;i<N_COUNTER_DISABLE_HINTS;i++){
    if (mask & counter_disable_hints[i].bit){
      int n = snprintf(buf+used,sizeof(buf)-used,"%s%s",
			used ? ", " : "",counter_disable_hints[i].hint);
      if (n > 0) used += (size_t)n;
      if (used >= sizeof(buf)) break;
    }
  }
  return buf;
}

struct preflight_result preflight_evaluate_groups(struct counter_group *list){
  struct preflight_result r;
  struct preflight_group_usage *u;
  struct preflight_group_usage *prev;
  struct counter_group *cgroup;
  int i,count;

  memset(&r,0,sizeof(r));
  r.nmi_watchdog_active = nmi_running;
  r.available = preflight_available_slots();

  for (cgroup = list; cgroup; cgroup = cgroup->next){
    count = 0;
    /* Only counters that land on the per-core general-purpose hardware
     * PMU compete for this budget. PERF_TYPE_SOFTWARE (software counters)
     * has its own separate accounting; dynamic PMU types (AMD IBS's
     * ibs_fetch/ibs_op, AMD L3's PERF_TYPE_L3 escape hatch inside a
     * PERF_TYPE_RAW group -- see topdown.c's raw_counter_group()) are
     * uncore/system PMUs with their own separate budget too, so they're
     * deliberately excluded here rather than double-counted against the
     * core budget. */
    if (cgroup->type_id == PERF_TYPE_HW_CACHE){
      count = cgroup->ncounters;
    } else if (cgroup->type_id == PERF_TYPE_RAW){
      for (i=0;i<cgroup->ncounters;i++){
	if (cgroup->cinfo[i].device_type == PERF_TYPE_RAW) count++;
      }
    }
    if (count == 0) continue;

    u = calloc(1,sizeof(*u));
    u->label = strdup(cgroup->label);
    u->mask = cgroup->mask;
    u->count = count;
    u->next = NULL;

    /* Insert in descending-count order so callers can walk r.groups and
     * greedily suggest dropping the largest contributor(s) first, without
     * needing to sort again themselves. */
    if (!r.groups || r.groups->count < count){
      u->next = r.groups;
      r.groups = u;
    } else {
      prev = r.groups;
      while (prev->next && prev->next->count >= count) prev = prev->next;
      u->next = prev->next;
      prev->next = u;
    }

    r.requested += count;
  }
  r.fits = (r.requested <= r.available);
  return r;
}

struct preflight_result preflight_evaluate(unsigned int mask){
  struct preflight_result r;
  struct counter_group *list = NULL;
  unsigned int saved_mask = counter_mask;

  counter_mask = mask;
  setup_counter_groups(&list);
  counter_mask = saved_mask;

  r = preflight_evaluate_groups(list);
  return r;
}

void preflight_result_free(struct preflight_result *result){
  struct preflight_group_usage *u;

  while (result->groups){
    u = result->groups;
    result->groups = u->next;
    free(u->label);
    free(u);
  }
}

void print_preflight_report(struct preflight_result *result){
  struct preflight_group_usage *u;

  fprintf(outfile,"counter-fit preflight: %d/%d general-purpose hardware PMU counter slot(s) requested%s\n",
	  result->requested,result->available,
	  result->nmi_watchdog_active ? " (nmi_watchdog reserves 1)" : "");
  for (u = result->groups; u; u = u->next){
    fprintf(outfile,"  group               %-12s %d counter(s), disable: %s\n",
	    u->label,u->count,disable_hints_for(u->mask));
  }
  if (result->fits){
    fprintf(outfile,"  fit                 OK -- should run without hardware-PMU multiplexing\n");
    return;
  }
  fprintf(outfile,"  fit                 WILL MULTIPLEX -- %d slot(s) over budget\n",
	  result->requested - result->available);
  if (cpu_info && cpu_info->vendor == VENDOR_INTEL){
    fprintf(outfile,"  note                these counters currently share a single perf event group on Intel"
	    " (topdown.c's setup_counters()); an oversized group can get little or no scheduled"
	    " time at all rather than degrading gracefully\n");
  }
  if (result->nmi_watchdog_active){
    fprintf(outfile,"  suggestion          stop the NMI watchdog to free 1 more slot (%d instead of %d):"
	    " scripts/setup_perf.sh or `sudo sysctl -w kernel.nmi_watchdog=0`\n",
	    result->available+1,result->available);
  }
  if (aflag){
    fprintf(outfile,"  note                --per-core is active -- this budget applies independently to"
	    " each core, not shared across cores\n");
  }
  {
    int remaining = result->requested - result->available;
    for (u = result->groups; u && remaining > 0; u = u->next){
      fprintf(outfile,"  suggestion          drop %s (%d counter(s)): %s\n",
	      u->label,u->count,disable_hints_for(u->mask));
      remaining -= u->count;
    }
  }
}

void preflight_warn_if_tight(struct preflight_result *result){
  struct preflight_group_usage *u;
  int remaining;

  if (result->fits) return;

  warning("counter-fit preflight: %d counter(s) requested but only %d general-purpose hardware PMU"
	  " slot(s) available%s -- this run will multiplex\n",
	  result->requested,result->available,
	  result->nmi_watchdog_active ? " (nmi_watchdog reserves 1)" : "");
  if (cpu_info && cpu_info->vendor == VENDOR_INTEL){
    notice("  note: these counters currently share a single perf event group on Intel -- an"
	   " oversized group can get little or no scheduled time at all rather than degrading"
	   " gracefully\n");
  }
  if (result->nmi_watchdog_active){
    notice("  tip: stop the NMI watchdog to free 1 more slot (%d instead of %d): scripts/setup_perf.sh"
	   " or `sudo sysctl -w kernel.nmi_watchdog=0`\n",
	   result->available+1,result->available);
  }
  if (aflag){
    notice("  note: --per-core is active -- this budget applies independently to each core, not"
	   " shared across cores\n");
  }
  remaining = result->requested - result->available;
  for (u = result->groups; u && remaining > 0; u = u->next){
    notice("  suggestion: drop %s (%d counter(s)): %s\n",u->label,u->count,disable_hints_for(u->mask));
    remaining -= u->count;
  }
}
