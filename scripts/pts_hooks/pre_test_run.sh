#!/bin/bash
# Phoronix Test Suite result_notifier hook: "Pre-test run execution hook"
# (pre_test_run_process). Registered via `phoronix-test-suite module-setup
# result_notifier` -- see scripts/setup_phoronix_hooks.sh for one-time
# registration help and doc/phoronix_hook_investigation.md for the full
# mechanism writeup, including why this can't just write into a wspy run
# directory directly (see the comment below).
#
# Logs a sub-millisecond-precision UTC checkpoint for the trial run about to
# start, keyed by PTS's own comparison hash and run position -- the raw
# material INVESTIGATION.md's 4.3 Tier 6 item 20 ("Phoronix-specific
# telemetry segmentation") needs to slice a wspy telemetry CSV into
# per-test-case/per-trial datasets without depending on composite.xml/
# test-log timestamp parsing. This script only captures the data; nothing
# in wspy or scripts/ consumes it yet -- that's the still-open remainder of
# item 20.
#
# IMPORTANT: verified against the real installed result_notifier.php
# (pts-core/modules/result_notifier.php): PTS invokes this hook via PHP's
# proc_open() with a *replaced* environment (its $env_vars argument is a
# small array built fresh, not merged with PTS's own environment) -- so
# this script only ever sees the PTS_EXTERNAL_TEST_* variables below, never
# anything exported by wspy/wspy-run/the shell that originally launched
# `phoronix-test-suite`. That means this hook cannot know which wspy run
# directory is "current" and must write to a fixed staging location
# instead; workload/phoronix/run_test.sh relocates that file into the run
# directory as a `pts_hooks.log` artifact once the whole phoronix-test-suite
# invocation (and therefore wspy) has exited. This assumes phoronix runs are
# not launched concurrently on the same host, matching wspy-queue's existing
# "no --parallel, one hardware PMU at a time" assumption elsewhere in this
# codebase.
#
# WSPY_PTS_HOOK_LOG below is honored if somehow already set in this
# process's environment (e.g. manual testing), but PTS itself cannot set or
# forward it per the above -- the real default must match
# workload/phoronix/run_test.sh's own WSPY_PTS_HOOK_LOG default exactly.
LOG="${WSPY_PTS_HOOK_LOG:-/tmp/wspy_pts_hooks.log}"

printf '%s\tSTART\t%s\t%s\t%s\t%s\t%s\t\t\n' \
  "$(date -u +%s.%N)" \
  "${PTS_EXTERNAL_TEST_HASH:-}" \
  "${PTS_EXTERNAL_TEST_RUN_POSITION:-}" \
  "${PTS_EXTERNAL_TEST_RUN_COUNT:-}" \
  "${PTS_EXTERNAL_TEST_IDENTIFIER:-}" \
  "${PTS_EXTERNAL_TEST_ARGS:-}" \
  >> "$LOG"
