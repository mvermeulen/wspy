/*
 * test_phase.c - unit tests for phase.c's interval phase-boundary detector.
 *
 * Follows test_ibs.c's pattern: #include the module directly (phase.c has
 * no main() to stub) and provide the handful of externs it expects from
 * wspy.c/topdown.c ourselves, rather than linking the whole program.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

/* Globals phase.c reads via wspy.h, normally defined in wspy.c. */
FILE *outfile;
int interval;
int phase_flag;
int aflag;
unsigned int counter_mask;

/* Don't include phase.h directly: its first use of "struct counter_group *"
 * would otherwise create that tag in this file's scope before phase.c's own
 * "wspy.h" include defines the real one (from cpu_info.h), producing two
 * distinct incompatible types. Including phase.c pulls in wspy.h/cpu_info.h
 * first, then phase.h in the right order -- and phase.h's include guard
 * means this is the only place it's processed either way. */
#include "phase.c"

/* find_ci_label() is topdown.c's, declared (not defined) in wspy.h, which
 * phase.c's phase_current_ipc() calls; defined here, after phase.c/wspy.h
 * have brought in cpu_info.h's struct counter_group/counter_info, so this
 * test doesn't need to pull in all of topdown.c just to link (mirrors
 * test_ibs.c stubbing outfile directly instead of linking wspy.c). */
struct counter_info *find_ci_label(struct counter_group *cgroup,char *label){
  int i;
  for (i=0;i<cgroup->ncounters;i++){
    if (!strcmp(cgroup->cinfo[i].label,label)){
      return &cgroup->cinfo[i];
    }
  }
  return NULL;
}

static int approx(double a,double b){
  return fabs(a-b) < 0.0001;
}

static void test_phase_name(void){
  printf("Testing phase_name...\n");
  assert(!strcmp(phase_name(PHASE_WARMUP),"warmup"));
  assert(!strcmp(phase_name(PHASE_STEADY),"steady"));
  assert(!strcmp(phase_name(PHASE_DEGRADED),"degraded"));
  assert(!strcmp(phase_name((enum wspy_phase)99),"unknown"));
  printf("PASS: phase_name\n");
}

static void test_phase_detect_is_available(void){
  printf("Testing phase_detect_is_available...\n");

  interval = 0; phase_flag = 1; counter_mask = COUNTER_IPC; aflag = 0;
  assert(phase_detect_is_available() == 0); /* no --interval */

  interval = 1; phase_flag = 1; counter_mask = COUNTER_IPC; aflag = 0;
  assert(phase_detect_is_available() != 0);

  interval = 1; phase_flag = 0; counter_mask = COUNTER_IPC; aflag = 0;
  assert(phase_detect_is_available() == 0); /* --no-phase-detect */

  interval = 1; phase_flag = 1; counter_mask = 0; aflag = 0;
  assert(phase_detect_is_available() == 0); /* --no-ipc */

  interval = 1; phase_flag = 1; counter_mask = COUNTER_IPC; aflag = 1;
  assert(phase_detect_is_available() == 0); /* --per-core */

  printf("PASS: phase_detect_is_available\n");
}

static struct counter_group *make_ipc_group(unsigned long cycles_value,unsigned long cycles_running,
					     unsigned long cycles_enabled,unsigned long instr_value,
					     unsigned long instr_running,unsigned long instr_enabled){
  struct counter_group *cgroup = calloc(1,sizeof(*cgroup));
  cgroup->label = "ipc";
  cgroup->mask = COUNTER_IPC;
  cgroup->ncounters = 2;
  cgroup->cinfo = calloc(2,sizeof(struct counter_info));
  cgroup->cinfo[0].label = "cpu-cycles";
  cgroup->cinfo[0].value = cycles_value;
  cgroup->cinfo[0].time_running = cycles_running;
  cgroup->cinfo[0].time_enabled = cycles_enabled;
  cgroup->cinfo[1].label = "instructions";
  cgroup->cinfo[1].value = instr_value;
  cgroup->cinfo[1].time_running = instr_running;
  cgroup->cinfo[1].time_enabled = instr_enabled;
  return cgroup;
}

static void test_phase_current_ipc(void){
  struct counter_group *cgroup;
  double ipc;

  printf("Testing phase_current_ipc...\n");

  /* simple case: fully scheduled (running == enabled), no multiplexing
   * correction needed -- 1500 instructions / 1000 cycles = 1.5 IPC */
  cgroup = make_ipc_group(1000,100,100,1500,100,100);
  ipc = phase_current_ipc(cgroup);
  assert(approx(ipc,1.5));

  /* multiplexed 50%: raw values double when scaled by enabled/running,
   * but the ratio between the two counters is unaffected */
  cgroup->cinfo[0].time_running = 50;
  cgroup->cinfo[1].time_running = 50;
  ipc = phase_current_ipc(cgroup);
  assert(approx(ipc,1.5));

  /* time_running == 0 (counter never scheduled) -- no usable sample */
  cgroup->cinfo[0].time_running = 0;
  ipc = phase_current_ipc(cgroup);
  assert(ipc < 0.0);

  /* no group carrying COUNTER_IPC at all */
  {
    struct counter_group empty;
    memset(&empty,0,sizeof(empty));
    empty.mask = 0;
    ipc = phase_current_ipc(&empty);
    assert(ipc < 0.0);
  }

  printf("PASS: phase_current_ipc\n");
}

static void test_phase_detector_warmup(void){
  struct phase_detector pd;

  printf("Testing phase_detector_update: warmup...\n");

  /* stable samples: warmup ends as soon as the window fills */
  phase_detector_init(&pd,1);
  assert(phase_detector_update(&pd,2.0,1.0) == PHASE_WARMUP);
  assert(phase_detector_update(&pd,2.0,2.0) == PHASE_WARMUP);
  assert(phase_detector_update(&pd,2.0,3.0) == PHASE_STEADY);
  assert(pd.nsamples == 3);
  assert(approx(pd.baseline,2.0));
  assert(pd.boundaries != NULL);
  assert(pd.boundaries->from == PHASE_WARMUP && pd.boundaries->to == PHASE_STEADY);
  assert(approx(pd.boundaries->elapsed_seconds,3.0));
  assert(pd.boundaries->next == NULL);
  phase_detector_free(&pd);
  assert(pd.boundaries == NULL);

  /* noisy samples: coefficient of variation stays too high to end warmup */
  phase_detector_init(&pd,1);
  assert(phase_detector_update(&pd,1.0,1.0) == PHASE_WARMUP);
  assert(phase_detector_update(&pd,3.0,2.0) == PHASE_WARMUP);
  assert(phase_detector_update(&pd,1.0,3.0) == PHASE_WARMUP);
  assert(phase_detector_update(&pd,1.0,4.0) == PHASE_WARMUP);
  assert(pd.boundaries == NULL);
  phase_detector_free(&pd);

  printf("PASS: phase_detector_update warmup\n");
}

static void test_phase_detector_degrade_and_recover(void){
  struct phase_detector pd;

  printf("Testing phase_detector_update: degrade/recover with hysteresis...\n");

  phase_detector_init(&pd,1);
  pd.phase = PHASE_STEADY;
  pd.candidate_phase = PHASE_STEADY;
  pd.baseline = 2.0;

  /* a single big drop is not enough -- must persist PHASE_PERSIST_SAMPLES ticks */
  assert(phase_detector_update(&pd,1.0,10.0) == PHASE_STEADY);
  assert(pd.boundaries == NULL);
  assert(phase_detector_update(&pd,1.0,11.0) == PHASE_DEGRADED);
  assert(pd.boundaries != NULL);
  assert(pd.boundaries->from == PHASE_STEADY && pd.boundaries->to == PHASE_DEGRADED);
  assert(approx(pd.boundaries->elapsed_seconds,11.0));

  /* recovery is likewise persistence-gated */
  assert(phase_detector_update(&pd,2.0,12.0) == PHASE_DEGRADED);
  assert(phase_detector_update(&pd,2.0,13.0) == PHASE_STEADY);
  assert(pd.boundaries->next != NULL);
  assert(pd.boundaries->next->from == PHASE_DEGRADED && pd.boundaries->next->to == PHASE_STEADY);
  assert(approx(pd.boundaries->next->elapsed_seconds,13.0));
  assert(pd.boundaries->next->next == NULL);
  assert(approx(pd.baseline,2.0)); /* re-baselined to the recovering sample */

  /* a sample inside the hysteresis band (between the drop and recover
   * thresholds) doesn't trigger a transition, and nudges the baseline
   * via the steady-state EMA instead */
  assert(phase_detector_update(&pd,1.7,14.0) == PHASE_STEADY);
  assert(pd.boundaries->next->next == NULL); /* no new boundary recorded */
  assert(approx(pd.baseline,0.9*2.0 + 0.1*1.7));

  phase_detector_free(&pd);
  printf("PASS: phase_detector_update degrade/recover\n");
}

static void test_phase_detector_disabled_and_invalid(void){
  struct phase_detector pd;

  printf("Testing phase_detector_update: disabled detector / invalid samples...\n");

  phase_detector_init(&pd,0);
  assert(phase_detector_update(&pd,5.0,1.0) == PHASE_WARMUP);
  assert(pd.nsamples == 0);
  assert(pd.boundaries == NULL);

  phase_detector_init(&pd,1);
  assert(phase_detector_update(&pd,-1.0,1.0) == PHASE_WARMUP);
  assert(pd.nsamples == 0);
  assert(pd.window_count == 0);

  printf("PASS: phase_detector_update disabled/invalid\n");
}

int main(void){
  test_phase_name();
  test_phase_detect_is_available();
  test_phase_current_ipc();
  test_phase_detector_warmup();
  test_phase_detector_degrade_and_recover();
  test_phase_detector_disabled_and_invalid();

  printf("\nAll test_phase tests passed.\n");
  return 0;
}
