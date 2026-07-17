#!/bin/bash
#
# Run a phoronix test suite test..
TESTNAME=${TESTNAME:="coremark"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}
WSPY_RUN=${WSPY_RUN:="/home/mev/source/wspy/wspy-run"}
WSPY_PLOT=${WSPY_PLOT:="/home/mev/source/wspy/wspy-plot"}
PROCTREE=${PROCTREE:="/home/mev/source/wspy/proctree"}
OUTROOT=${OUTROOT:="."}

# One directory for this run: <OUTROOT>/phoronix/<TESTNAME>/<RUN_ID>, via
# wspy-run's unified output layout (INVESTIGATION_4.0.md "Run artifact
# foundation"). Computed here, not left to wspy-run's own default, so the
# gnuplot step at the end can find it without scraping wspy-run's stderr.
STAMP="$(date -u +%Y%m%dT%H%M%S)"
NS="$(date -u +%N)"
RUN_ID="${STAMP}.${NS:0:3}-$$"

RUN_INDEX="${OUTROOT}/phoronix/run-index.jsonl"

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
    # that's fine to just wait out.
    "$WSPY_RUN" --wspy "$WSPY" --suite phoronix --benchmark "$TESTNAME" \
        --run-id "$RUN_ID" -o "$OUTROOT" --run-index "$RUN_INDEX" \
        deep-cpu,tree-heavy -- \
        phoronix-test-suite batch-run $TESTNAME
    RUNDIR="${OUTROOT}/phoronix/${TESTNAME}/${RUN_ID}"
    # Renders every *.csv in $RUNDIR that has a "time" column against the
    # shared plot templates (wspy-plot, INVESTIGATION_4.0.md's "What shipped
    # in 4.1"), replacing the old gnuplot.sh's two hardcoded filenames -- output
    # lands in $RUNDIR/plots/, wspy-run's own unified-output-layout convention.
    "$WSPY_PLOT" --rundir "$RUNDIR"
    # tree-heavy's --tree pass writes the raw process.tree.txt record; render it
    # into a human-readable reconstructed tree (same "run the tool automatically"
    # treatment wspy-plot just got for CSVs, replacing this script's old habit of
    # only running proctree by hand). Guarded on the file existing/non-empty since
    # a --tree pass can time out or fail without producing one. -C matches
    # tree-heavy's own --tree-cmdline; -M/-N/-P (vmsize+rss/thread count/ppid)
    # are unconditional since that data is always in the raw file regardless of
    # any flag (see proctree.c's parse_stat()), so there's nothing to gate them on.
    if [ -s "${RUNDIR}/process.tree.txt" ]; then
        "$PROCTREE" -C -M -N -P "${RUNDIR}/process.tree.txt" > "${RUNDIR}/process.tree.summary.txt"
    fi
fi
