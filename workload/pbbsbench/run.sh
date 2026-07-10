#!/bin/bash
PBBSBENCH=${PBBSBENCH:="/home/mev/source/pbbsbench"}
WSPY=${WSPY:="/home/mev/source/wspy/wspy"}
WSPY_RUN=${WSPY_RUN:="/home/mev/source/wspy/wspy-run"}
BENCHMARK=${BENCHMARK:="all-small"}
OUTROOT=${OUTROOT:="."}
# Resolve to an absolute path before `cd ${PBBSBENCH}` below, which otherwise
# permanently changes the shell's cwd -- a relative OUTROOT (the default ".")
# would then be interpreted against $PBBSBENCH instead of the directory this
# script was invoked from.
OUTROOT="$(cd "$OUTROOT" && pwd)"

# One directory for this run: <OUTROOT>/pbbsbench/<BENCHMARK>/<RUN_ID>, via
# wspy-run's unified output layout (INVESTIGATION_4.0.md "Run artifact
# foundation"). Computed here so the raw baseline run below lands in the same
# directory as the instrumented passes.
STAMP="$(date -u +%Y%m%dT%H%M%S)"
NS="$(date -u +%N)"
RUN_ID="${STAMP}.${NS:0:3}-$$"
RUNDIR="${OUTROOT}/pbbsbench/${BENCHMARK}/${RUN_ID}"
mkdir -p "$RUNDIR"

cd ${PBBSBENCH}

# uninstrumented baseline run, for comparison against the instrumented passes below
./runall -small 1> "${RUNDIR}/bench.out" 2> "${RUNDIR}/bench.err"

# deep-cpu,tree-heavy is the same 8-pass sweep (software/branch, ipc/topdown,
# cache/float, topdown csv, frontend, opcache, --tree) this script used to
# hand-roll as 8 separate $WSPY invocations. Only the --tree pass carries a
# timeout (3600s, wspy-run's tree-heavy default) -- not because it runs
# slower, but because an hour of process-tree records is already more than
# is practical to publish/browse. This script never bounded any pass before,
# so the tree pass capping out at 3600s here is a new behavior, not just a
# migration of existing behavior -- the counter passes remain unbounded.
"$WSPY_RUN" --wspy "$WSPY" --suite pbbsbench --benchmark "$BENCHMARK" \
    --run-id "$RUN_ID" -o "$OUTROOT" --run-index "${OUTROOT}/pbbsbench/run-index.jsonl" \
    deep-cpu,tree-heavy -- ./runall -small
