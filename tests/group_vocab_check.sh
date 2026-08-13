#!/bin/bash
# tests/group_vocab_check.sh - counter-group vocabulary drift check.
#
# INVESTIGATION.md's 4.4(a) "Preset/Configuration/Option vocabulary
# refactor" item: web/joblib.py's ALL_GROUPS is a separately-typed Python
# copy of multipass.c's multipass_group_names[] (the --counters=<list>/
# --passes=<list> token vocabulary), kept in sync by hand rather than
# queried live (querying `wspy --list-groups` at import time would turn a
# plain module-level constant into a runtime dependency on a built wspy
# binary -- see ALL_GROUPS' own comment for why that trade-off was
# declined). This script is the drift detector that trade-off relies on:
# it asserts every name in ALL_GROUPS is a real group `wspy --list-groups`
# actually knows about.
#
# Deliberately a subset check, not equality: multipass_group_names[] has 3
# ARM-only entries (arm-dcache-mem/arm-icache-tlb/arm-mem-align-tlb) the
# web checklist doesn't expose yet -- a real, separate, not-yet-scoped
# web-UI gap, not drift this check should flag.
#
# Usage: ./tests/group_vocab_check.sh (run from repo root; needs a built
# ./wspy and python3).

set -u
cd "$(dirname "$0")/.." || exit 1

FAILURES=0

if [ ! -x ./wspy ]; then
  echo "FAIL: ./wspy not built -- run 'make wspy' first"
  exit 1
fi

WSPY_GROUPS=$(./wspy --list-groups | awk '{print $1}' | sort -u)
if [ -z "$WSPY_GROUPS" ]; then
  echo "FAIL: 'wspy --list-groups' produced no output"
  exit 1
fi

PY_GROUPS=$(python3 -c "
import sys
sys.path.insert(0, 'web')
import joblib
print('\n'.join(sorted(joblib.GROUP_NAMES)))
")

MISSING=0
while IFS= read -r name; do
  [ -n "$name" ] || continue
  if ! grep -qx -- "$name" <<< "$WSPY_GROUPS"; then
    echo "FAIL: web/joblib.py's ALL_GROUPS names '$name', but 'wspy --list-groups' doesn't know it"
    MISSING=$((MISSING + 1))
  fi
done <<< "$PY_GROUPS"

if [ "$MISSING" -gt 0 ]; then
  FAILURES=$((FAILURES + 1))
else
  echo "  ALL_GROUPS <= wspy --list-groups: OK ($(echo "$PY_GROUPS" | wc -l) names checked)"
fi

echo ""
echo "=== group_vocab_check: $FAILURES failed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
