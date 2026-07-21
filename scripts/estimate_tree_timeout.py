#!/usr/bin/env python3
"""
scripts/estimate_tree_timeout.py - sizes wspy-run's --tree pass timeout from
an actual per-workload run-time estimate, instead of the historical fixed
3600s constant (INVESTIGATION.md's 4.2 "Size wspy-run's --tree pass timeout
from an actual run-time estimate" item).

Reuses web/joblib.py's Phoronix runtime-estimation logic (originally built
for the web launcher's "Estimated runtime display" Check button) rather than
reimplementing the same phoronix-test-suite-info text parsing a second time
in bash -- wspy-run has never shelled out to an external tool/parsed its
text output, and a second, independently-drifting copy of already-validated
parsing logic is worse than one shared implementation with two callers.

Usage: estimate_tree_timeout.py <workload argv...>
(the exact command wspy-run would launch, e.g.
 phoronix-test-suite batch-run coremark)

Prints one integer (seconds) to stdout and exits 0 if an estimate could be
derived; prints nothing and exits 1 otherwise (non-Phoronix workload,
phoronix-test-suite not installed/reachable, or its output didn't parse) --
wspy-run's own caller falls back to the historical 3600s constant in that
case, so this script failing is never fatal to a real run.

Why the --tree timeout exists at all (not what this script changes): losing
a key ptrace event for a traced process can leave wspy hung waiting to clean
up -- the timeout is a hang backstop, not primarily a data-volume cap. Real
Phoronix runs legitimately exceed the historical 3600s constant, so this
script's floor is that same 3600s (this can only raise the cap for a
workload confirmed to legitimately need longer, never lower it below what's
already proven safe) -- see MIN_TIMEOUT_SECONDS/MAX_TIMEOUT_SECONDS below.

batch-run's own estimate is a floor, not a target: `phoronix-test-suite
info <test>` reports a single test's own typical runtime, but a real
batch-run sweep runs every configured option combination (see
INVESTIGATION.md's "Phoronix per-test option-combination count" item for
the concrete blender example -- 5 blend files x 2 compute backends = 10
full renders for one test) -- something `info` doesn't account for.
BATCH_RUN_MULTIPLIER is deliberately more generous than RUN_MULTIPLIER for
exactly this reason, not because batch-run itself runs slower.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web"))
import joblib  # noqa: E402

# Deliberately simple, documented starting points (same "adjust here if real
# usage says otherwise" spirit as this codebase's other heuristic constants,
# e.g. preflight.c's available-PMU-slot estimate) -- not derived from a
# formal study of real batch-run/run variance.
RUN_MULTIPLIER = 2.0
BATCH_RUN_MULTIPLIER = 5.0
MIN_TIMEOUT_SECONDS = 3600  # never below the historical constant every --tree pass used before this
MAX_TIMEOUT_SECONDS = 21600  # 6 hours -- a true hang backstop, not a normal-operation limit


def main(argv):
    if len(argv) < 2:
        return 1
    workload_tokens = argv[1:]
    workload = joblib.shlex.join(workload_tokens)

    is_batch_run = len(workload_tokens) >= 2 and workload_tokens[1] == "batch-run"

    result = joblib.estimate_phoronix_workload_seconds(workload)
    total_seconds = result.get("total_seconds")
    if total_seconds is None:
        return 1

    multiplier = BATCH_RUN_MULTIPLIER if is_batch_run else RUN_MULTIPLIER
    sized = int(total_seconds * multiplier)
    sized = max(MIN_TIMEOUT_SECONDS, min(MAX_TIMEOUT_SECONDS, sized))
    print(sized)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
