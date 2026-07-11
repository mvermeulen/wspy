/*
 * multipass.c - native multi-pass counter execution: pure bin-packing logic
 * described in multipass.h. See wspy.c's run_multipass() for the actual
 * per-pass execution loop that consumes this.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wspy.h"
#include "error.h"
#include "preflight.h"
#include "multipass.h"

const struct multipass_group_name multipass_group_names[] = {
  { "ipc",             COUNTER_IPC },
  { "topdown",         COUNTER_TOPDOWN },
  { "topdown2",        COUNTER_TOPDOWN2 },
  { "topdown-frontend", COUNTER_TOPDOWN_FE },
  { "topdown-backend",  COUNTER_TOPDOWN_BE },
  { "topdown-optlb",    COUNTER_TOPDOWN_OP },
  { "branch",          COUNTER_BRANCH },
  { "dcache",          COUNTER_DCACHE },
  { "icache",          COUNTER_ICACHE },
  { "cache1",          COUNTER_L1CACHE },
  { "cache2",          COUNTER_L2CACHE },
  { "cache3",          COUNTER_L3CACHE },
  { "memory",          COUNTER_MEMORY },
  { "tlb",             COUNTER_TLB },
  { "opcache",         COUNTER_OPCACHE },
  { "software",        COUNTER_SOFTWARE },
  { "float",           COUNTER_FLOAT },
};
const int multipass_n_group_names = sizeof(multipass_group_names)/sizeof(multipass_group_names[0]);

int multipass_lookup_group_name(const char *name,unsigned int *bit_out){
  int i;

  for (i = 0; i < multipass_n_group_names; i++){
    if (!strcmp(multipass_group_names[i].name,name)){
      *bit_out = multipass_group_names[i].bit;
      return 1;
    }
  }
  return 0;
}

struct multipass_plan multipass_plan_build(unsigned int requested_mask){
  struct multipass_plan plan;
  unsigned int current_mask = 0;
  int pass_idx = 0,i;
  int cap = __builtin_popcount(requested_mask); /* worst case: one group per pass */

  memset(&plan,0,sizeof(plan));
  plan.pass_mask = calloc(cap ? cap : 1,sizeof(unsigned int));

  for (i = 0; i < multipass_n_group_names; i++){
    unsigned int bit = multipass_group_names[i].bit;
    struct preflight_result pf;

    if (!(requested_mask & bit)) continue;

    if (bit == COUNTER_SOFTWARE){
      /* Software counters (PERF_TYPE_SOFTWARE) never compete for the
       * general-purpose hardware PMU budget preflight_evaluate() checks --
       * setup_counter_groups() doesn't even build a group for this bit
       * itself (software_counter_group() is added separately, by both
       * main()'s single-pass path and run_multipass()). Fold it into
       * whichever pass is already accumulating (or start a trivial pass of
       * its own if none is open) instead of letting a preflight_evaluate()
       * call that can't see this bit's true (zero) cost bounce it into a
       * spurious extra re-execution of the workload. */
      if (current_mask == 0) plan.pass_mask[pass_idx++] = bit;
      else current_mask |= bit;
      continue;
    }

    pf = preflight_evaluate(current_mask | bit);
    if (current_mask != 0 && !pf.fits){
      /* doesn't fit alongside what's already in this pass -- close this
       * pass out and start a fresh one with just this group */
      plan.pass_mask[pass_idx++] = current_mask;
      preflight_result_free(&pf);
      current_mask = bit;
      pf = preflight_evaluate(current_mask);
    } else {
      current_mask |= bit;
    }
    if (!pf.fits){
      /* this pass's sole/first group alone exceeds the budget -- it still
       * becomes its own (multiplexing) pass rather than looping or
       * blocking the whole feature, same as a tight single-pass run today. */
      warning("--passes: pass %d ('%s') alone requests more counters than fit -- it will multiplex\n",
              pass_idx+1,multipass_group_names[i].name);
      preflight_warn_if_tight(&pf);
    }
    preflight_result_free(&pf);
  }
  if (current_mask != 0) plan.pass_mask[pass_idx++] = current_mask;
  plan.npasses = pass_idx;
  return plan;
}

void multipass_plan_free(struct multipass_plan *plan){
  free(plan->pass_mask);
  plan->pass_mask = NULL;
  plan->npasses = 0;
}
