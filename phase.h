/*
 * phase.h - interval automatic phase-boundary detection: basic marker
 * detection over --interval's periodic samples, classifying each interval
 * tick as warmup/steady/degraded.
 *
 * INVESTIGATION.md's "Interval (--interval) -> automatic phase-boundary
 * detection (warmup/steady/degraded)" item -- named prerequisite for
 * phase-aware topdown (4.2) and phase-aware IBS. This is the basic-marker-
 * detection half only: a per-tick phase label (report-layer addition to
 * data --interval already collects), not the downstream topdown/IBS
 * segmentation that will eventually consume it.
 *
 * Signal: per-interval IPC (instructions/cpu-cycles, using the delta each
 * read_counters() call already leaves in .value -- see topdown.c). Only
 * meaningful when --interval, IPC counters, and system-wide counters are
 * all in play: --per-core (aflag) counters live on each core's own list,
 * which interval mode's timer_callback() never reads at all (a pre-existing
 * --per-core + --interval gap -- see tests/capability_matrix.sh's
 * per-core-topdown bundle comment), so phase detection simply disables
 * itself under --per-core rather than reading a list it was never wired to.
 *
 * Algorithm (deliberately simple -- "basic marker detection", not a
 * changepoint-detection library):
 *  - warmup: collect the first PHASE_WARMUP_WINDOW samples; once the window
 *    is full, compute the coefficient of variation (stddev/mean) across it.
 *    Below PHASE_WARMUP_CV_MAX, the run is judged to have reached a stable
 *    operating point and transitions to steady, with the window mean
 *    recorded as the steady-state IPC baseline.
 *  - steady: while samples stay within PHASE_DEGRADED_DROP of baseline, the
 *    baseline is nudged toward each new sample (PHASE_BASELINE_EMA) so slow
 *    legitimate drift doesn't itself look like degradation. A sample
 *    landing more than PHASE_DEGRADED_DROP below baseline is a *candidate*
 *    degraded transition; it only commits after PHASE_PERSIST_SAMPLES
 *    consecutive candidate samples, so one noisy tick can't flip the label.
 *  - degraded: symmetric recovery back to steady, with a
 *    PHASE_RECOVER_MARGIN hysteresis gap above the drop threshold so a
 *    sample sitting right at the boundary doesn't flap the phase every
 *    tick. Recovery re-baselines to the recovering sample rather than
 *    trusting a baseline that predates the degraded episode.
 *
 * Deliberately not implemented here: persisting phase boundaries into
 * --manifest/--run-index. The per-tick CSV "phase" column already lets a
 * downstream reader reconstruct boundaries by diffing adjacent rows; a
 * second, redundant representation in the manifest is deferred until an
 * actual consumer (phase-aware topdown, 4.2) needs it directly instead of
 * via the CSV.
 *
 * Like coverage.h/preflight.h, this header doesn't include wspy.h itself --
 * each .c file that uses it includes wspy.h first (for FILE *outfile,
 * enum output_format, struct counter_group, and the globals
 * phase_detect_is_available() reads).
 */
#ifndef _WSPY_PHASE_H
#define _WSPY_PHASE_H 1

enum wspy_phase { PHASE_WARMUP = 0, PHASE_STEADY, PHASE_DEGRADED };

#define PHASE_WARMUP_WINDOW    3     /* interval samples needed before warmup can end */
#define PHASE_WARMUP_CV_MAX    0.15  /* stddev/mean over the window, below which warmup ends */
#define PHASE_DEGRADED_DROP    0.20  /* fractional drop below baseline that flags a candidate degraded sample */
#define PHASE_RECOVER_MARGIN   0.10  /* extra fraction above the drop threshold required to recover (hysteresis) */
#define PHASE_PERSIST_SAMPLES  2     /* consecutive candidate samples required before committing a transition */
#define PHASE_BASELINE_EMA     0.10  /* weight given to each new sample nudging the steady-state baseline */

struct phase_boundary {
  double elapsed_seconds;
  enum wspy_phase from;
  enum wspy_phase to;
  struct phase_boundary *next;
};

struct phase_detector {
  int enabled;              /* 0 if this run has no usable IPC signal to classify (--no-ipc, --per-core, no --interval) */
  enum wspy_phase phase;
  double window[PHASE_WARMUP_WINDOW];
  int window_count;
  double baseline;
  enum wspy_phase candidate_phase;
  int candidate_streak;
  int nsamples;
  struct phase_boundary *boundaries;
  struct phase_boundary *boundaries_tail;
};

/* The single detector instance for this run, fed once per interval tick by
 * both topdown.c's timer_callback() and wspy.c's final-row read. Its
 * .enabled is set once in wspy.c:main() via phase_detector_init(), from
 * phase_detect_is_available(). */
extern struct phase_detector phase_state;

const char *phase_name(enum wspy_phase phase);

/* True when this run's flags (interval active, IPC counters requested,
 * phase detection not disabled, --per-core not active) make phase
 * detection meaningful. Pure predicate, no side effects. */
int phase_detect_is_available(void);

void phase_detector_init(struct phase_detector *pd,int enabled);
void phase_detector_free(struct phase_detector *pd);

/* Feeds one interval sample. ipc < 0 means "no valid sample this tick"
 * (e.g. a counter read with time_running == 0) and is ignored rather than
 * corrupting the rolling window/baseline. Returns the phase after this
 * update -- a no-op call (disabled detector, or ipc < 0) just returns the
 * unchanged current phase. */
enum wspy_phase phase_detector_update(struct phase_detector *pd,double ipc,double elapsed_seconds);

/* Extracts this tick's instantaneous IPC from the "ipc" counter group in
 * counter_group_list (instructions/cpu-cycles, scaled by time_running/
 * time_enabled the same way topdown.c's print_ipc() does) -- or -1.0 if no
 * usable sample is available this tick (group/counters not found, or
 * time_running == 0). */
double phase_current_ipc(struct counter_group *counter_group_list);

/* End-of-run boundary summary, human output only (see this header's
 * comment on why CSV doesn't get a redundant second representation).
 * Silent if the detector is disabled or there were no transitions. */
void phase_print_boundaries(struct phase_detector *pd);

#endif
