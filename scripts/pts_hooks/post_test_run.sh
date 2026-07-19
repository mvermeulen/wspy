#!/bin/bash
# Phoronix Test Suite result_notifier hook: "Post-test run execution hook"
# (post_test_run_process). See pre_test_run.sh in this same directory for
# the full mechanism note (fixed staging path, environment-isolation
# caveat, relocation into the wspy run directory by
# workload/phoronix/run_test.sh) -- this is its FINISH-side counterpart,
# same TSV line shape plus the two fields only known once the trial has
# actually completed.
LOG="${WSPY_PTS_HOOK_LOG:-/tmp/wspy_pts_hooks.log}"

printf '%s\tFINISH\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
  "$(date -u +%s.%N)" \
  "${PTS_EXTERNAL_TEST_HASH:-}" \
  "${PTS_EXTERNAL_TEST_RUN_POSITION:-}" \
  "${PTS_EXTERNAL_TEST_RUN_COUNT:-}" \
  "${PTS_EXTERNAL_TEST_IDENTIFIER:-}" \
  "${PTS_EXTERNAL_TEST_ARGS:-}" \
  "${PTS_EXTERNAL_TEST_RESULT:-}" \
  "${PTS_EXTERNAL_TEST_STD_DEV_PERCENT:-}" \
  >> "$LOG"
