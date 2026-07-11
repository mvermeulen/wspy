/*
 * phase.c - interval automatic phase-boundary detection, described in phase.h
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "wspy.h"
#include "phase.h"

struct phase_detector phase_state;

const char *phase_name(enum wspy_phase phase){
  switch(phase){
  case PHASE_WARMUP:   return "warmup";
  case PHASE_STEADY:   return "steady";
  case PHASE_DEGRADED: return "degraded";
  }
  return "unknown";
}

int phase_detect_is_available(void){
  /* --per-core counters live on each core's own list, which interval
   * mode's timer_callback() never reads (see phase.h) -- disable rather
   * than silently reading nothing. */
  return interval && phase_flag && (counter_mask & COUNTER_IPC) && !aflag;
}

void phase_detector_init(struct phase_detector *pd,int enabled){
  memset(pd,0,sizeof(*pd));
  pd->enabled = enabled;
  pd->phase = PHASE_WARMUP;
  pd->candidate_phase = PHASE_WARMUP;
}

void phase_detector_free(struct phase_detector *pd){
  struct phase_boundary *b;

  while (pd->boundaries){
    b = pd->boundaries;
    pd->boundaries = b->next;
    free(b);
  }
  pd->boundaries_tail = NULL;
}

static void phase_record_boundary(struct phase_detector *pd,enum wspy_phase from,enum wspy_phase to,
				   double elapsed_seconds){
  struct phase_boundary *b = calloc(1,sizeof(*b));

  b->elapsed_seconds = elapsed_seconds;
  b->from = from;
  b->to = to;
  if (pd->boundaries_tail){
    pd->boundaries_tail->next = b;
  } else {
    pd->boundaries = b;
  }
  pd->boundaries_tail = b;
}

static void phase_commit_transition(struct phase_detector *pd,enum wspy_phase to,double elapsed_seconds){
  phase_record_boundary(pd,pd->phase,to,elapsed_seconds);
  pd->phase = to;
  pd->candidate_streak = 0;
  pd->candidate_phase = to;
}

double phase_current_ipc(struct counter_group *counter_group_list){
  struct counter_group *cgroup;
  struct counter_info *cycles = NULL,*instructions = NULL;

  for (cgroup = counter_group_list; cgroup; cgroup = cgroup->next){
    if (!(cgroup->mask & COUNTER_IPC)) continue;
    if (!cycles) cycles = find_ci_label(cgroup,"cpu-cycles");
    if (!instructions) instructions = find_ci_label(cgroup,"instructions");
    if (cycles && instructions) break;
  }
  if (!cycles || !instructions) return -1.0;
  // time_running == 0 means this tick's counter was never scheduled on the
  // PMU at all -- no usable sample. read_counters() already scaled .value
  // by the multiplex ratio as it read it, so no rescaling belongs here;
  // redoing it would double-count the correction.
  if (cycles->time_running == 0 || instructions->time_running == 0) return -1.0;
  if (cycles->value == 0) return -1.0;
  return (double) instructions->value / (double) cycles->value;
}

enum wspy_phase phase_detector_update(struct phase_detector *pd,double ipc,double elapsed_seconds){
  if (!pd->enabled || ipc < 0.0) return pd->phase;

  pd->nsamples++;

  if (pd->phase == PHASE_WARMUP){
    if (pd->window_count < PHASE_WARMUP_WINDOW){
      pd->window[pd->window_count++] = ipc;
    } else {
      memmove(&pd->window[0],&pd->window[1],(PHASE_WARMUP_WINDOW-1)*sizeof(double));
      pd->window[PHASE_WARMUP_WINDOW-1] = ipc;
    }
    if (pd->window_count == PHASE_WARMUP_WINDOW){
      double mean = 0.0,variance = 0.0,cv;
      int i;

      for (i=0;i<PHASE_WARMUP_WINDOW;i++) mean += pd->window[i];
      mean /= PHASE_WARMUP_WINDOW;
      if (mean > 0.0){
	for (i=0;i<PHASE_WARMUP_WINDOW;i++){
	  double d = pd->window[i] - mean;
	  variance += d*d;
	}
	variance /= PHASE_WARMUP_WINDOW;
	cv = sqrt(variance) / mean;
	if (cv <= PHASE_WARMUP_CV_MAX){
	  pd->baseline = mean;
	  phase_commit_transition(pd,PHASE_STEADY,elapsed_seconds);
	}
      }
    }
    return pd->phase;
  }

  {
    double ratio = (pd->baseline > 0.0) ? (ipc / pd->baseline) : 1.0;
    enum wspy_phase target = pd->phase;

    if (pd->phase == PHASE_STEADY && ratio <= (1.0 - PHASE_DEGRADED_DROP)){
      target = PHASE_DEGRADED;
    } else if (pd->phase == PHASE_DEGRADED &&
	       ratio >= (1.0 - PHASE_DEGRADED_DROP + PHASE_RECOVER_MARGIN)){
      target = PHASE_STEADY;
    }

    if (target != pd->phase){
      if (pd->candidate_phase == target){
	pd->candidate_streak++;
      } else {
	pd->candidate_phase = target;
	pd->candidate_streak = 1;
      }
      if (pd->candidate_streak >= PHASE_PERSIST_SAMPLES){
	phase_commit_transition(pd,target,elapsed_seconds);
	if (target == PHASE_STEADY){
	  /* re-baseline on recovery rather than trusting a baseline
	   * that predates the degraded episode */
	  pd->baseline = ipc;
	}
      }
    } else {
      pd->candidate_streak = 0;
      pd->candidate_phase = pd->phase;
      if (pd->phase == PHASE_STEADY){
	pd->baseline = (1.0-PHASE_BASELINE_EMA)*pd->baseline + PHASE_BASELINE_EMA*ipc;
      }
    }
  }
  return pd->phase;
}

void phase_print_boundaries(struct phase_detector *pd){
  struct phase_boundary *b;

  if (!pd->enabled || !pd->boundaries) return;

  fprintf(outfile,"phase boundaries:\n");
  for (b = pd->boundaries; b; b = b->next){
    fprintf(outfile,"  %6.1fs  %-8s -> %s\n",b->elapsed_seconds,phase_name(b->from),phase_name(b->to));
  }
}
