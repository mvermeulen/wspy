#!/bin/bash
# tests/capability_matrix.sh - capability-matrix smoke tests.
#
# INVESTIGATION_4.0.md, "Testing and documentation" track: "Capability-matrix
# smoke tests (CPU vendor/family x GPU build x key option bundles,
# graceful-degradation paths) -- run_tests.sh already does a version of this
# (AMDGPU-build vs not); formalize it as new tracks add more axes."
#
# Three axes:
#   - CPU vendor/family: detected via ./cpu_info, not switchable on a given
#     host -- this script reports what it detected and which vendor-specific
#     bundles it could/couldn't meaningfully exercise, rather than faking a
#     vendor it isn't running on.
#   - GPU build: whichever of AMDGPU=0/AMDGPU=1 ./wspy was built with,
#     auto-detected below (mirrors run_tests.sh's existing "GPU support not
#     built" check). run_tests.sh calls this script once per build it
#     produces (default, and again after `make AMDGPU=1` when ROCm is
#     available) so both ends of this axis get covered across a CI run.
#   - Key option bundles: a broad-but-not-exhaustive set of flag
#     combinations below, one per counter group/subsystem plus a few
#     combined "kitchen sink" cases.
#
# For every bundle, the graceful-degradation contract under test is: wspy
# exits 0 (workload ran even if some/all requested counters could not be
# opened -- e.g. no perf permissions, unsupported hardware, missing sysfs
# path), never prints "fatal error" or a crash indication, and (for --csv
# bundles) produces a CSV header/value row with matching column counts. The
# one intentional exception is --exit-with-child, whose whole point is to
# propagate the *workload's* exit status -- tested separately with its own
# expected exit codes.
#
# Usage: ./tests/capability_matrix.sh (run from repo root; expects ./wspy
# and ./cpu_info already built, e.g. via `make wspy cpu_info`).

set -u
cd "$(dirname "$0")/.." || exit 1

WSPY=./wspy
FAILURES=0
CHECKS=0

fail() {
  echo "FAIL: $1"
  FAILURES=$((FAILURES + 1))
}

vendor="unknown"
if [ -x ./cpu_info ]; then
  if ./cpu_info 2>/dev/null | grep -q '^	AMD'; then
    vendor="AMD"
  elif ./cpu_info 2>/dev/null | grep -q '^	Intel'; then
    vendor="Intel"
  elif ./cpu_info 2>/dev/null | grep -q '^	ARM'; then
    vendor="ARM"
  fi
fi

gpu_build="AMDGPU=0"
if ! "$WSPY" --gpu-busy -- /bin/true 2>&1 | grep -q "GPU support not built"; then
  gpu_build="AMDGPU=1"
fi

echo "Capability matrix: CPU vendor=$vendor, GPU build=$gpu_build"

# run_bundle <label> <expected_exit_code> <flags...> -- <workload...>
# Asserts the process exit code, absence of "fatal error"/crash indications,
# and (only for --csv bundles) that the CSV header and value row have the
# same column count -- the graceful-degradation contract described above.
run_bundle() {
  local label="$1"; shift
  local expect_exit="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do
    flags+=("$1"); shift
  done
  shift # consume "--"
  local workload=("$@")
  CHECKS=$((CHECKS + 1))

  local out rc
  out=$("$WSPY" "${flags[@]}" -- "${workload[@]}" 2>&1)
  rc=$?

  local bundle_ok=1
  if [ "$rc" != "$expect_exit" ]; then
    fail "bundle [$label]: exit code $rc, expected $expect_exit
  flags: ${flags[*]}
  output (last 5 lines):
$(echo "$out" | tail -5)"
    bundle_ok=0
  fi
  if echo "$out" | grep -qi "fatal error"; then
    fail "bundle [$label]: printed a fatal error instead of degrading gracefully
  flags: ${flags[*]}
  $(echo "$out" | grep -i "fatal error")"
    bundle_ok=0
  fi
  if echo "$out" | grep -qiE "segmentation fault|core dumped|assertion.*failed"; then
    fail "bundle [$label]: crash indication in output
  flags: ${flags[*]}"
    bundle_ok=0
  fi

  local csv_note=""
  for f in "${flags[@]}"; do
    if [ "$f" = "--csv" ]; then
      local header value hcols vcols
      header=$(echo "$out" | grep "^elapsed," | head -1)
      value=$(echo "$out" | grep -v "^elapsed," | grep -v "^warning:" | grep -v "^error:" | tail -1)
      if [ -n "$header" ]; then
        hcols=$(echo "$header" | tr ',' '\n' | wc -l)
        vcols=$(echo "$value" | tr ',' '\n' | wc -l)
        if [ "$hcols" != "$vcols" ]; then
          fail "bundle [$label]: csv header/value column mismatch ($hcols vs $vcols)
  flags: ${flags[*]}
  header: $header
  value:  $value"
          bundle_ok=0
        fi
        csv_note=" ($hcols csv columns)"
      fi
      break
    fi
  done

  if [ "$bundle_ok" = "1" ]; then
    echo "  bundle [$label]: OK (exit=$rc)$csv_note"
  fi
}

# run_expected_fatal_bundle <label> <expected_exit_code> <flags...> -- <workload...>
# For the small set of bundles that are *supposed* to be rejected outright
# (--passes' incompatibility checks, wspy.c) -- unlike run_bundle() above,
# this asserts a "fatal error" WAS printed (a clear, deliberate usage
# rejection) rather than treating any fatal-error text as a failure; it still
# checks for a crash indication, since a deliberate rejection should never
# also look like one.
run_expected_fatal_bundle() {
  local label="$1"; shift
  local expect_exit="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do
    flags+=("$1"); shift
  done
  shift # consume "--"
  local workload=("$@")
  CHECKS=$((CHECKS + 1))

  local out rc
  out=$("$WSPY" "${flags[@]}" -- "${workload[@]}" 2>&1)
  rc=$?

  local bundle_ok=1
  if [ "$rc" != "$expect_exit" ]; then
    fail "bundle [$label]: exit code $rc, expected $expect_exit
  flags: ${flags[*]}
  output: $out"
    bundle_ok=0
  fi
  if ! echo "$out" | grep -qi "fatal error"; then
    fail "bundle [$label]: expected a fatal error rejecting this flag combination, got none
  flags: ${flags[*]}
  output: $out"
    bundle_ok=0
  fi
  if echo "$out" | grep -qiE "segmentation fault|core dumped|assertion.*failed"; then
    fail "bundle [$label]: crash indication in output
  flags: ${flags[*]}"
    bundle_ok=0
  fi

  if [ "$bundle_ok" = "1" ]; then
    echo "  bundle [$label]: OK (exit=$rc, rejected as expected)"
  fi
}

echo ""
echo "=== Counter-group bundles (--no-ipc baseline + one group at a time) ==="
run_bundle "no-counters"           0 --no-ipc                       -- /bin/true
run_bundle "default-ipc"           0                                -- /bin/true
run_bundle "topdown"               0 --csv --no-ipc --topdown       -- /bin/true
run_bundle "topdown2"              0 --csv --no-ipc --topdown2      -- /bin/true
run_bundle "topdown-decomposed"    0 --csv --no-ipc --topdown-frontend --topdown-backend --topdown-optlb -- /bin/true
run_bundle "branch"                0 --csv --no-ipc --branch        -- /bin/true
run_bundle "cache-suite"           0 --csv --no-ipc --cache1 --cache2 --cache3 --dcache --icache --tlb -- /bin/true
run_bundle "opcache-float-memory"  0 --csv --no-ipc --opcache --float --memory -- /bin/true
run_bundle "software"              0 --csv --no-ipc --software      -- /bin/true
run_bundle "system"                0 --csv --no-ipc --system        -- /bin/true
run_bundle "ibs-basic"             0 --csv --no-ipc --ibs-basic     -- /bin/true
run_bundle "ibs-memory-deep"       0 --csv --no-ipc --ibs-memory-deep -- /bin/true

echo ""
echo "=== GPU bundles (build=$gpu_build; must degrade gracefully either way) ==="
run_bundle "gpu-busy"    0 --no-ipc --gpu-busy    -- /bin/true
run_bundle "gpu-metrics" 0 --no-ipc --gpu-metrics -- /bin/true
run_bundle "gpu-smi"     0 --no-ipc --gpu-smi     -- /bin/true
run_bundle "gpu-all-with-system" 0 --csv --no-ipc --system --gpu-busy --gpu-metrics --gpu-smi -- /bin/true
# --gpu-device selects among multiple AMD cards for the bundles above; an
# out-of-range index (999) must degrade gracefully (no sysfs/SMI data for that
# run) rather than fatal, same contract as a permission-denied counter.
run_bundle "gpu-device-select"      0 --no-ipc --gpu-device=0   --gpu-busy -- /bin/true
run_bundle "gpu-device-out-of-range" 0 --no-ipc --gpu-device=999 --gpu-busy -- /bin/true

echo ""
echo "=== Modifier bundles (per-core, interval, tree variants) ==="
# --per-core combined with any counter group used to produce a CSV header
# with only the base/coverage columns while each per-core row appended that
# group's values as extra, unheaded columns (INVESTIGATION_4.0.md's
# "Fix the known --per-core CSV column-count mismatch" item). wspy.c's
# per_core_csv now re-architects the aflag/csv print flow into one row per
# active core, tagged with a "core" column, so header and row column counts
# match -- see tests/golden_output.sh's "per-core-topdown"
# assert_csv_columns_match case for the column-count-parity check itself;
# this bundle still just checks the exit-code/no-fatal/no-crash contract.
run_bundle "per-core-topdown" 0 --csv --no-ipc --per-core --topdown -- /bin/true
run_bundle "interval"         0 --csv --no-ipc --interval 1         -- sleep 1
# Interval + IPC (default) engages phase.c's automatic phase-boundary
# detection: exercises the "phase" CSV column's own graceful-degradation
# paths (no perf permissions -> phase_current_ipc() returns no usable
# sample every tick, detector just never leaves "warmup"; see phase.h).
run_bundle "interval-phase-detect"    0 --csv --interval 1                  -- sleep 1
run_bundle "interval-no-phase-detect" 0 --csv --interval 1 --no-phase-detect -- sleep 1
# --per-core disables phase detection outright (phase_detect_is_available()) --
# it reads cpu_info->systemwide_counters, which is empty of per-core groups
# under aflag. --per-core + --interval also still has the pre-existing,
# separate CSV column-count mismatch above: timer_callback() only ever reads
# cpu_info->systemwide_counters, so per_core_csv's new row shape deliberately
# doesn't apply while --interval is active (see wspy.c's per_core_csv
# comment) -- this combination isn't fixed by this bundle's sibling above.
run_bundle "interval-per-core"        0 --csv --per-core --interval 1        -- sleep 1

TREE_OUT=$(mktemp /tmp/wspy_capmatrix_tree.XXXXXX)
trap 'rm -f "$TREE_OUT"' EXIT
run_bundle "tree-basic"                0 --no-ipc --tree "$TREE_OUT" -- /bin/true
run_bundle "tree-cmdline-open-vmsize"  0 --no-ipc --tree "$TREE_OUT" --tree-cmdline --tree-open --tree-vmsize -- /bin/true

echo ""
echo "=== Run-artifact bundles (manifest, run-index, capabilities) ==="
MANIFEST_OUT=$(mktemp /tmp/wspy_capmatrix_manifest.XXXXXX)
RUNINDEX_OUT=$(mktemp /tmp/wspy_capmatrix_runindex.XXXXXX)
CSV_OUT=$(mktemp /tmp/wspy_capmatrix_out.XXXXXX)
trap 'rm -f "$TREE_OUT" "$MANIFEST_OUT" "$RUNINDEX_OUT" "$CSV_OUT"' EXIT
run_bundle "manifest-and-run-index" 0 --no-ipc --csv -o "$CSV_OUT" --manifest "$MANIFEST_OUT" --run-index "$RUNINDEX_OUT" -- /bin/true
run_bundle "capabilities" 0 --capabilities -- /bin/true

echo ""
echo "=== Core/thread affinity control (--affinity, --list-affinity) ==="
run_bundle "affinity-nosmt"    0 --no-ipc --affinity=nosmt     -- /bin/true
run_bundle "affinity-domain0"  0 --no-ipc --affinity=domain=0  -- /bin/true
run_bundle "affinity-thread0"  0 --no-ipc --affinity=thread=0  -- /bin/true
run_bundle "affinity-cpuset"   0 --no-ipc --affinity=cpuset=0  -- /bin/true
run_bundle "list-affinity"     0 --list-affinity -- /bin/true
# An out-of-range domain/thread id is a real placement error (nothing to pin
# to), not a flag-combination incompatibility -- still expected to fail
# loudly (run_expected_fatal_bundle), same graceful-degradation-doesn't-mean-
# never-fail idiom as --passes' own incompatibility checks below.
run_expected_fatal_bundle "affinity-bad-domain-id" 1 --no-ipc --affinity=domain=99999 -- /bin/true
# coretype=<id> (ARM MIDR-based big.LITTLE grouping, e.g. Cortex-A7xx "big" vs
# Cortex-A5xx "little" cores) has nothing to resolve on this codebase's own
# x86 test hosts (no midr_el1 at all) -- expected to fail loudly here, same
# as any other zero-core-types host would; see test_affinity.c's dedicated
# fake-ARM-sysfs fixture for the actual grouping-logic coverage.
run_expected_fatal_bundle "affinity-coretype-unavailable" 1 --no-ipc --affinity=coretype=0 -- /bin/true

echo ""
echo "=== Kitchen-sink bundle (most counter groups + system + rusage at once) ==="
run_bundle "kitchen-sink" 0 --csv --topdown --topdown-frontend --topdown-backend --topdown-optlb \
  --branch --dcache --icache --tlb --cache1 --cache2 --cache3 --opcache --float --memory --software \
  --system --gpu-busy --gpu-metrics --gpu-smi -- /bin/true

echo ""
echo "=== Native multi-pass counter execution (--passes) ==="
# A multi-group request that (on most hosts) exceeds the general-purpose PMU
# budget in a single pass: wspy internally re-launches the workload once per
# automatically-sized pass and merges the result into one CSV row/manifest --
# the graceful-degradation contract here is the same as every other bundle
# (exit 0, no fatal error, matching CSV column counts), it just now spans N
# child re-executions instead of one.
run_bundle "passes-multi-group" 0 --csv --no-ipc \
  --passes=ipc,topdown,cache2,cache3,branch,memory,tlb,opcache,software -- /bin/true
# --multiplex: same request, but collapsed into a single (likely
# multiplexed) pass instead of bin-packed into N -- still graceful
# degradation (exit 0, no fatal error, matching CSV column counts), now
# relying on read_counters()'s time_running/time_enabled scaling
# (INVESTIGATION_4.0.md's "What shipped in 4.1", the multiplex-scaling
# correctness fix) to keep the values correct.
run_bundle "passes-multiplex" 0 --csv --no-ipc \
  --passes=ipc,topdown,cache2,cache3,branch,memory,tlb,opcache,software --multiplex -- /bin/true
# --passes' merge semantics only cover the aggregate case -- each of these is
# an intentional fatal incompatibility (see wspy.c's incompatibility checks),
# not a graceful-degradation case, so a nonzero exit *with* a fatal error is
# the expected/correct outcome here (run_expected_fatal_bundle, not
# run_bundle -- see its comment above).
run_expected_fatal_bundle "passes-interval-incompatible" 1 --no-ipc --passes=ipc --interval 1 -- /bin/true
run_expected_fatal_bundle "passes-per-core-incompatible" 1 --no-ipc --passes=ipc --per-core -- /bin/true
run_expected_fatal_bundle "passes-tree-incompatible"     1 --no-ipc --passes=ipc --tree "$TREE_OUT" -- /bin/true
run_expected_fatal_bundle "passes-ibs-incompatible"      1 --no-ipc --passes=ipc --ibs-basic -- /bin/true
# --multiplex only means something alongside --passes -- without it, it's
# rejected the same way the other --passes-only modifiers would be if they
# had no other meaning (mirrors the checks just above).
run_expected_fatal_bundle "multiplex-without-passes-incompatible" 1 --no-ipc --multiplex -- /bin/true

echo ""
echo "=== --exit-with-child: the one bundle where a nonzero exit is correct ==="
run_bundle "exit-with-child-success" 0 --no-ipc --exit-with-child -- /bin/true
run_bundle "exit-with-child-failure" 1 --no-ipc --exit-with-child -- /bin/false

if [ "$vendor" != "AMD" ] && [ "$vendor" != "Intel" ] && [ "$vendor" != "ARM" ]; then
  echo ""
  echo "NOTE: CPU vendor detected as '$vendor' -- vendor-specific raw-event"
  echo "bundles above ran, but their pass/fail only confirms graceful"
  echo "degradation, not that any AMD/Intel/ARM-specific counter actually"
  echo "measured anything (see tests/golden_output.sh for vendor-gated"
  echo "exact-value contract checks)."
fi

echo ""
echo "=== $CHECKS bundles run, $FAILURES failed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
