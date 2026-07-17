#!/bin/bash
#
# Run a cpu2017 test
TESTNAME=${TESTNAME:="503.bwaves_r"}
SPECDIR=${SPECDIR:="/home/mev/cpu2017"}
SPECCONFIG=${SPECCONFIG:="mev-aocc-7840.cfg"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}
WSPY_RUN=${WSPY_RUN:="/home/mev/source/wspy/wspy-run"}
WSPY_PLOT=${WSPY_PLOT:="/home/mev/source/wspy/wspy-plot"}
PROCTREE=${PROCTREE:="/home/mev/source/wspy/proctree"}
OUTROOT=${OUTROOT:="."}

# One directory for this whole invocation: <OUTROOT>/cpu2017/<TESTNAME>/<RUN_ID>,
# via wspy-run's unified output layout (INVESTIGATION_4.0.md "Run artifact
# foundation" -- suite/benchmark/run_id/{...,manifest.json}). The run id is
# computed here, not left to wspy-run's own default, so the build log below
# and the gnuplot step at the end can both find the directory without
# scraping wspy-run's stderr for it.
STAMP="$(date -u +%Y%m%dT%H%M%S)"
NS="$(date -u +%N)"
RUN_ID="${STAMP}.${NS:0:3}-$$"
RUNDIR="${OUTROOT}/cpu2017/${TESTNAME}/${RUN_ID}"
mkdir -p "$RUNDIR"

pushd $SPECDIR
source shrc
ulimit -s unlimited
popd

# build
(cd "$RUNDIR" && runcpu --config ${SPECCONFIG} --action=build --tune base ${TESTNAME} 2>&1 | tee amd.${TESTNAME}.build.out)

if [ $(grep -c Intel /proc/cpuinfo) -gt 0 ]; then
    echo "Intel not supported"
    exit 0
else
    # deep-cpu,tree-heavy is the same 8-pass sweep (software/branch, ipc/topdown,
    # cache/float, topdown csv, frontend, opcache, --tree) this script used to
    # hand-roll as 8 separate $WSPY invocations; wspy-run runs them all into
    # $RUNDIR. Only the --tree pass carries a timeout (3600s, wspy-run's
    # tree-heavy default) -- not because it runs slower than the others, but
    # because an hour of process-tree records is already more than is
    # practical to publish/browse. The counter passes have no such cap and
    # are fine to run long if a slow benchmark's iterations take a while.
    "$WSPY_RUN" --wspy "$WSPY" --suite cpu2017 --benchmark "$TESTNAME" \
        --run-id "$RUN_ID" -o "$OUTROOT" --run-index "${OUTROOT}/cpu2017/run-index.jsonl" \
        deep-cpu,tree-heavy -- \
        runcpu --config ${SPECCONFIG} --action=validate --tune base --iterations 3 $TESTNAME \
        2>&1 | tee "${RUNDIR}/amd.${TESTNAME}.out"
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
