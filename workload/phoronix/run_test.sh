#!/bin/bash
#
# Run a phoronix test suite test..
TESTNAME=${TESTNAME:="coremark"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}
WSPY_RUN=${WSPY_RUN:="/home/mev/source/wspy/wspy-run"}
WSPY_PLOT=${WSPY_PLOT:="/home/mev/source/wspy/wspy-plot"}
PROCTREE=${PROCTREE:="/home/mev/source/wspy/proctree"}
OUTROOT=${OUTROOT:="."}
# Fixed staging path scripts/pts_hooks/{pre,post}_test_run.sh write to, if
# registered (phoronix-test-suite's result_notifier module) -- must match
# those scripts' own default exactly, since PTS invokes them with a
# replaced environment that can't carry this value down from here (see
# doc/phoronix_hook_investigation.md and pts_hooks/pre_test_run.sh's own
# comment for why). Not present at all if the hooks aren't registered on
# this host (scripts/setup_phoronix_hooks.sh) -- degrades to no
# pts_hooks.log artifact, same measured-vs-unavailable idiom as everywhere
# else in this codebase.
WSPY_PTS_HOOK_LOG=${WSPY_PTS_HOOK_LOG:="/tmp/wspy_pts_hooks.log"}

# One directory for this run: <OUTROOT>/phoronix/<TESTNAME>/<RUN_ID>, via
# wspy-run's unified output layout (INVESTIGATION_4.0.md "Run artifact
# foundation"). Computed here, not left to wspy-run's own default, so the
# gnuplot step at the end can find it without scraping wspy-run's stderr.
STAMP="$(date -u +%Y%m%dT%H%M%S)"
NS="$(date -u +%N)"
RUN_ID="${STAMP}.${NS:0:3}-$$"
RUNDIR="${OUTROOT}/phoronix/${TESTNAME}/${RUN_ID}"

RUN_INDEX="${OUTROOT}/phoronix/run-index.jsonl"

# A stale staging file left over from an interrupted previous run (one that
# never reached the relocation step below) would otherwise get silently
# misattributed to this run -- archive it out of the way rather than lose
# or misattribute it. Not expected in normal operation (phoronix runs are
# not run concurrently on this host, matching wspy-queue's own
# one-at-a-time assumption elsewhere).
if [ -s "$WSPY_PTS_HOOK_LOG" ]; then
    mv "$WSPY_PTS_HOOK_LOG" "${WSPY_PTS_HOOK_LOG}.stale-$$"
    echo "run_test.sh: found a stale $WSPY_PTS_HOOK_LOG from an earlier run, moved aside to ${WSPY_PTS_HOOK_LOG}.stale-$$" >&2
fi

if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    # software/branch, topdown, ipc/l2, backend -- Intel doesn't have the
    # AMD-specific opcache/frontend/l3 groups deep-cpu also sweeps. No --tree
    # pass in this profile, so no timeout (matches this script's original
    # behavior -- it never bounded the Intel passes either).
    "$WSPY_RUN" --wspy "$WSPY" --suite phoronix --benchmark "$TESTNAME" \
        --run-id "$RUN_ID" -o "$OUTROOT" --run-index "$RUN_INDEX" \
        deep-cpu-intel -- \
        phoronix-test-suite batch-run $TESTNAME
else
    # deep-cpu,tree-heavy is the same 8-pass sweep (software/branch,
    # ipc/topdown, cache/float, topdown csv, frontend, opcache, --tree) this
    # script used to hand-roll as 8 separate $WSPY invocations. Only the
    # --tree pass carries a timeout (3600s, wspy-run's tree-heavy default) --
    # not because it runs slower, but because an hour of process-tree records
    # is already more than is practical to publish/browse. The counter passes
    # have no such cap -- some phoronix tests legitimately take a while, and
    # that's fine to just wait out. PROFILE lets a caller pick a different
    # wspy-run profile instead (e.g. "gpu-compute" for a GPU-bound/latency-
    # driven workload -- see wspy-run --list) while keeping today's sweep as
    # the default.
    PROFILE=${PROFILE:-"deep-cpu,tree-heavy"}
    "$WSPY_RUN" --wspy "$WSPY" --suite phoronix --benchmark "$TESTNAME" \
        --run-id "$RUN_ID" -o "$OUTROOT" --run-index "$RUN_INDEX" \
        "$PROFILE" -- \
        phoronix-test-suite batch-run $TESTNAME
    # Renders every *.csv in $RUNDIR that has a "time" column against the
    # shared plot templates (wspy-plot, INVESTIGATION_4.0.md's "What shipped
    # in 4.1"), replacing the old gnuplot.sh's two hardcoded filenames -- output
    # lands in $RUNDIR/plots/, wspy-run's own unified-output-layout convention.
    "$WSPY_PLOT" --rundir "$RUNDIR"
    # A --tree pass (tree-heavy or gpu-compute) writes the raw
    # process.tree.txt record; render it into two human-readable views (same
    # "run the tool automatically" treatment wspy-plot just got for CSVs,
    # replacing this script's old habit of only running proctree by hand).
    # Guarded on the file existing/non-empty since a --tree pass can time out
    # or fail without producing one.
    if [ -s "${RUNDIR}/process.tree.txt" ]; then
        # Which proctree flags match this run's own --tree pass depends on
        # which profile produced it -- tree-heavy captures --tree-cmdline
        # only; gpu-compute captures the syscall-latency set (futex/io-wait/
        # connect/wait/poll/nanosleep) instead, no cmdline. Neither is
        # discoverable from here without parsing wspy-run's own bash config
        # (same reasoning web/joblib.py's execute_profile_run() documents for
        # its own equivalent lookup), so this mirrors those fixed choices
        # directly -- update alongside wspy-run's load_builtin_profile() if
        # either ever changes, or a future profile adds its own --tree pass.
        # -M/-N/-P (vmsize+rss/thread count/ppid) are unconditional on the
        # summary view since that data is always in the raw file regardless
        # of any flag (see proctree.c's parse_stat()), so there's nothing to
        # gate them on; the simple view deliberately omits them (and
        # everything else) -- just cpu=/start=/finish= per process, easier to
        # read as a pure process hierarchy than the fully-annotated summary.
        case ",$PROFILE," in
            *,gpu-compute,*) PROCTREE_FLAGS="-X -B -K -J -L -Z" ;;
            *,tree-heavy,*)  PROCTREE_FLAGS="-C" ;;
            *)               PROCTREE_FLAGS="" ;;
        esac
        "$PROCTREE" $PROCTREE_FLAGS -M -N -P "${RUNDIR}/process.tree.txt" > "${RUNDIR}/process.tree.summary.txt"
        "$PROCTREE" "${RUNDIR}/process.tree.txt" > "${RUNDIR}/process.tree.simple.txt"
    fi
fi

# Relocate the pts_hooks staging log (if the result_notifier hooks are
# registered on this host -- scripts/setup_phoronix_hooks.sh) into the run
# directory as a real artifact, and clear the staging path for the next
# invocation. By the time control returns here, phoronix-test-suite (and
# every hook subprocess PTS spawned) has fully exited, so this is not a
# race against anything still writing. Nothing to do, silently, if the
# hooks aren't registered -- same degrade-don't-fail idiom as the
# wspy-plot/proctree steps above.
if [ -s "$WSPY_PTS_HOOK_LOG" ]; then
    mv "$WSPY_PTS_HOOK_LOG" "${RUNDIR}/pts_hooks.log"
fi
