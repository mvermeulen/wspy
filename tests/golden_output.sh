#!/bin/bash
# tests/golden_output.sh - golden output-contract tests.
#
# INVESTIGATION.md, "Testing and documentation" track: "Golden
# output-contract tests (CSV header/order, summary fragments, tree format) --
# cheapest regression guard; run_tests.sh already checks CSV column order for
# existing fields -- extend the same pattern as new fields land." This file
# formalizes that into one place instead of a handful of ad hoc greps
# scattered through run_tests.sh, and covers far more of the flag matrix than
# run_tests.sh's spot checks did.
#
# Deliberately runs without root/perf access, like run_tests.sh's other
# integration checks: every counter group still emits its full, fixed set of
# CSV columns even when every perf_event_open() call in it fails with EACCES
# (coverage.c's "measured vs unavailable" graceful degradation) -- what this
# file checks is the *shape* of that output (column count/order, label text,
# tree line grammar), not the numeric values inside it. Exact-string CSV
# header checks are vendor-specific (AMD raw event labels differ from
# Intel's) and only run when they match the host's detected vendor; the
# header/value column-count check is vendor-neutral and runs everywhere.
#
# Usage: ./tests/golden_output.sh (run from repo root; expects ./wspy and
# ./cpu_info already built, e.g. via `make wspy cpu_info`).

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
echo "Detected CPU vendor: $vendor"

nvidia_build="no"
if ! "$WSPY" --gpu-nvidia -- /bin/true 2>&1 | grep -q "GPU support not built"; then
  nvidia_build="yes"
fi
echo "NVIDIA build: $nvidia_build"

# memory's local/remote raw events (topdown.c's amd_raw_events[]) carry a
# requires=/sys/devices/system/node/node1 gate -- meaningless on a
# single-NUMA-node host, since "remote" can only ever be 0 there -- so the
# group's CSV header shape genuinely differs by host topology, not just
# vendor. See topdown.c's amd_raw_events[] comment on those entries.
has_remote_numa="no"
if [ -d /sys/devices/system/node/node1 ]; then
  has_remote_numa="yes"
fi
echo "Detected second NUMA node (node1): $has_remote_numa"

# power/energy-pkg (power.c, RAPL-equivalent) presence is a sysfs/kernel
# fact independent of CPU vendor (Intel RAPL has reported it for years; AMD
# Family 19h+ support is newer) -- confirmed unsupported (yet) on real Intel
# hardware in practice, so this is gated on sysfs presence directly rather
# than assumed from vendor the way the AMD raw-event block below is.
has_power_pmu="no"
if [ -e /sys/bus/event_source/devices/power/events/energy-pkg ]; then
  has_power_pmu="yes"
fi
echo "Detected CPU power/energy-pkg PMU: $has_power_pmu"

# power_core/energy-core (per-core RAPL-equivalent, INVESTIGATION.md's 4.2
# Tier 1 "Per-core energy support" item) is checked independently of
# has_power_pmu above -- power_core_counter_group()'s own per-core wiring
# (wspy.c's main()) is gated purely on power_core's own present/event_present
# state, not on whether the package event also succeeded, so on a
# hypothetical host with one but not the other these two flags genuinely
# diverge (real hardware so far always has both together, but the check
# shouldn't assume that).
has_power_core_pmu="no"
if [ -e /sys/bus/event_source/devices/power_core/events/energy-core ]; then
  has_power_core_pmu="yes"
fi
echo "Detected CPU per-core power/energy-core PMU: $has_power_core_pmu"

# assert_csv_header <label> <flags...> -- <expected exact header line>
assert_csv_header() {
  local label="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do
    flags+=("$1"); shift
  done
  shift # consume the "--" separator
  local expected="$1"
  CHECKS=$((CHECKS + 1))
  local actual
  actual=$("$WSPY" --csv "${flags[@]}" -- /bin/true 2>/dev/null | head -1)
  if [ "$actual" != "$expected" ]; then
    fail "csv header [$label]: flags='${flags[*]}'
  expected: $expected
  actual:   $actual"
  else
    echo "  csv header [$label]: OK"
  fi
}

# assert_csv_header_regex <label> <flags...> -- <expected ERE, full-line anchored>
# For headers with host-specific content (network interface names).
assert_csv_header_regex() {
  local label="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do
    flags+=("$1"); shift
  done
  shift
  local expected_re="$1"
  CHECKS=$((CHECKS + 1))
  local actual
  actual=$("$WSPY" --csv "${flags[@]}" -- /bin/true 2>/dev/null | head -1)
  if ! echo "$actual" | grep -qE "^${expected_re}$"; then
    fail "csv header (regex) [$label]: flags='${flags[*]}'
  expected pattern: $expected_re
  actual:            $actual"
  else
    echo "  csv header (regex) [$label]: OK"
  fi
}

# assert_csv_columns_match <label> <flags...>
# Generic, vendor-neutral property: header column count == value-row column
# count. This is the check that would have caught every column-shape defect
# fixed alongside this test file going in (missing trailing commas, value
# rows gated behind "if (counter_available)" that silently dropped columns
# the header still claimed, an empty raw-event group that still claimed
# header columns, --software's CSV corruption, --memory's missing print
# wiring) -- run across the flag matrix, not just the exact-string cases.
assert_csv_columns_match() {
  local label="$1"; shift
  CHECKS=$((CHECKS + 1))
  local out header value hcols vcols
  out=$("$WSPY" --csv "$@" -- /bin/true 2>/dev/null)
  header=$(echo "$out" | head -1)
  value=$(echo "$out" | tail -1)
  hcols=$(echo "$header" | tr ',' '\n' | wc -l)
  vcols=$(echo "$value" | tr ',' '\n' | wc -l)
  if [ -z "$header" ] || [ "$hcols" != "$vcols" ]; then
    fail "csv column count [$label]: flags='$*' header_cols=$hcols value_cols=$vcols
  header: $header
  value:  $value"
  else
    echo "  csv column count [$label]: OK ($hcols columns)"
  fi
}

# assert_normal_contains <label> <flags...> -- <grep -E pattern>
assert_normal_contains() {
  local label="$1"; shift
  local flags=()
  while [ "$1" != "--" ]; do
    flags+=("$1"); shift
  done
  shift
  local pattern="$1"
  CHECKS=$((CHECKS + 1))
  local out
  out=$("$WSPY" "${flags[@]}" -- /bin/true 2>&1)
  if ! echo "$out" | grep -qE "$pattern"; then
    fail "normal output [$label]: flags='${flags[*]}' missing pattern: $pattern"
  else
    echo "  normal output [$label]: OK"
  fi
}

echo ""
echo "=== CSV header contract (exact match, base/rusage) ==="
BASE="elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,"
assert_csv_header "no-ipc"      --no-ipc    -- "${BASE}counters_measured,counters_requested,"
assert_csv_header "default-ipc"             -- "${BASE}ipc,counters_measured,counters_requested,"
assert_csv_header "no-rusage"   --no-rusage -- "ipc,counters_measured,counters_requested,"
# INVESTIGATION.md's 4.2 "Hierarchical topdown schema" item: the 9 trailing
# columns after "sanity," are the new L1->L2 hierarchy (contention_pct, then
# 8 L2 columns each named "<parent>_<child>_pct") -- see topdown.c's
# print_topdown() comment for the full naming/denominator convention.
TOPDOWN_L2="contention_pct,retire_ucode_pct,retire_fastpath_pct,frontend_latency_pct,frontend_bandwidth_pct,backend_cpu_pct,backend_memory_pct,spec_branch_pct,spec_pipeline_pct,"
assert_csv_header "topdown"     --no-ipc --topdown  -- "${BASE}retire,frontend,backend,speculate,confidence,sanity,${TOPDOWN_L2}counters_measured,counters_requested,"
assert_csv_header "topdown2"    --no-ipc --topdown2 -- "${BASE}retire,frontend,backend,speculate,confidence,sanity,${TOPDOWN_L2}counters_measured,counters_requested,"
# power/energy-pkg's column names are vendor-neutral (unlike the AMD raw
# event labels below), so this is checked here rather than inside the
# vendor-gated block -- gated on sysfs presence instead: an unsupported
# host/kernel correctly contributes zero extra columns (power_counter_group()
# returns NULL), same shape as "topdown-backend-unsupported-on-amd"/
# "cache1-not-yet-implemented" below.
if [ "$has_power_pmu" = "yes" ]; then
  assert_csv_header "power"          --no-ipc --power -- "${BASE}pkg_joules,pkg_watts,counters_measured,counters_requested,"
else
  assert_csv_header "power-unsupported" --no-ipc --power -- "${BASE}counters_measured,counters_requested,"
fi
# INVESTIGATION.md's 4.2 Tier 1 "Per-core energy support" item: power_core's
# new core_joules/core_watts columns land on a per-core "core," row (before
# the systemwide pkg_joules/pkg_watts power already prints), but -- unlike
# the plain "power"/"power-unsupported" case above -- the "core," column's
# very presence is gated on has_power_core_pmu alone, independent of
# has_power_pmu: wspy.c's per-core wiring only ever populates
# core_specific_counters via power_core_counter_group() succeeding, which
# needs power_core's own present/event_present state, not the package
# event's. --power --per-core with *no* other counter group and power_core
# unsupported therefore falls all the way back to the plain (non-per-core)
# aggregate row shape -- per_core_csv (wspy.c) never triggers at all, since
# nothing populated any core's core_specific_counters -- rather than a
# per-core row with just a bare "core," column. Real hardware so far always
# has power_core exactly when it has power (both flags move together), but
# this covers all four combinations correctly rather than assuming that.
if [ "$has_power_core_pmu" = "yes" ]; then
  if [ "$has_power_pmu" = "yes" ]; then
    assert_csv_header "per-core-power" --no-ipc --per-core --power -- \
      "${BASE}core,core_joules,core_watts,pkg_joules,pkg_watts,counters_measured,counters_requested,"
  else
    assert_csv_header "per-core-power-core-only" --no-ipc --per-core --power -- \
      "${BASE}core,core_joules,core_watts,counters_measured,counters_requested,"
  fi
elif [ "$has_power_pmu" = "yes" ]; then
  # power_core unsupported -- falls back to the plain aggregate "power" shape,
  # --per-core has no effect at all for this combination (see comment above).
  assert_csv_header "per-core-power-pkg-only-no-per-core-effect" --no-ipc --per-core --power -- \
    "${BASE}pkg_joules,pkg_watts,counters_measured,counters_requested,"
else
  assert_csv_header "per-core-power-unsupported" --no-ipc --per-core --power -- \
    "${BASE}counters_measured,counters_requested,"
fi

echo ""
echo "=== CSV header contract (exact match, --interval phase-boundary detection) ==="
# phase.c's "phase" column sits right after "time," and is present only when
# phase_detect_is_available() (interval + IPC counters + phase_flag + not
# --per-core) -- see phase.h. --interval mode also skips the base/rusage
# columns entirely (xflag is only printed when !interval), so these don't use
# BASE.
assert_csv_header "interval-default-ipc"    --interval 1                    -- "time,phase,ipc,counters_measured,counters_requested,"
assert_csv_header "interval-no-ipc"         --no-ipc --interval 1           -- "time,counters_measured,counters_requested,"
assert_csv_header "interval-no-phase-detect" --interval 1 --no-phase-detect -- "time,ipc,counters_measured,counters_requested,"
assert_csv_header "interval-per-core"       --per-core --interval 1         -- "time,core,ipc,counters_measured,counters_requested,"

echo ""
echo "=== CSV header contract (host-specific, regex) ==="
assert_csv_header_regex "system" --no-ipc --system -- \
  'load,runnable,cpu,idle,iowait,irq,freq,cpu_temp,(net [^,]+,)+(disk [^,]+ read,disk [^,]+ write,disk [^,]+ time,)*mem_free_mb,mem_cached_mb,mem_dirty_mb,mem_writeback_mb,swap_free_mb,committed_as_mb,elapsed,utime,stime,nvcsw,nivcsw,inblock,oublock,maxrss,minflt,majflt,nswap,counters_measured,counters_requested,'

if [ "$vendor" = "AMD" ]; then
  echo ""
  echo "=== CSV header contract (exact match, AMD-specific raw event labels) ==="
  assert_csv_header "branch"           --no-ipc --branch           -- "${BASE}branch miss,near_return,near_return_mispredicted,indirect_branch_mispredicted,counters_measured,counters_requested,"
  assert_csv_header "cache2-l2"        --no-ipc --cache2           -- "${BASE}l2miss,counters_measured,counters_requested,"
  assert_csv_header "cache3-l3"        --no-ipc --cache3           -- "${BASE}l3miss,counters_measured,counters_requested,"
  assert_csv_header "dcache"           --no-ipc --dcache           -- "${BASE}L1-dcache miss,counters_measured,counters_requested,"
  assert_csv_header "icache"           --no-ipc --icache           -- "${BASE}L1-icache miss,counters_measured,counters_requested,"
  assert_csv_header "tlb"              --no-ipc --tlb              -- "${BASE}dTLB miss,iTLB miss,counters_measured,counters_requested,"
  if [ "$has_remote_numa" = "yes" ]; then
    assert_csv_header "memory"         --no-ipc --memory           -- "${BASE}bandwidth,counters_measured,counters_requested,"
  else
    # single NUMA node: every memory raw event's requires= gate clears its
    # .use bit before setup_counter_groups() runs, so raw_counter_group()
    # returns NULL (num_counters == 0) and the group never links in --
    # same "zero extra columns" shape as topdown-backend/cache1 below.
    assert_csv_header "memory-no-remote-numa" --no-ipc --memory      -- "${BASE}counters_measured,counters_requested,"
  fi
  assert_csv_header "opcache"          --no-ipc --opcache          -- "${BASE}opcache miss,counters_measured,counters_requested,"
  assert_csv_header "float"            --no-ipc --float            -- "${BASE}float,counters_measured,counters_requested,"
  assert_csv_header "software"         --no-ipc --software         -- "${BASE}cpu-clock,task-clock,page faults,context switches,cpu migrations,major page faults,minor page faults,alignment faults,emulation faults,counters_measured,counters_requested,"
  assert_csv_header "topdown-frontend" --no-ipc --topdown-frontend -- "${BASE}icache,itlb1,itlb2,tlbflush,counters_measured,counters_requested,"
  assert_csv_header "topdown-optlb"    --no-ipc --topdown-optlb    -- "${BASE}opcache,dtlb1,dtlb2,counters_measured,counters_requested,"
  # AMD has no topdown-be raw events (that decomposition is Intel-only), and
  # --cache1/COUNTER_L1CACHE has no backing raw events or print dispatch on
  # either vendor yet (a reserved-but-unimplemented counter group) -- both
  # correctly contribute zero extra columns rather than an empty/mismatched
  # group. See raw_counter_group()'s "return NULL for zero counters" fix.
  assert_csv_header "topdown-backend-unsupported-on-amd" --no-ipc --topdown-backend -- "${BASE}counters_measured,counters_requested,"
  assert_csv_header "cache1-not-yet-implemented"         --no-ipc --cache1          -- "${BASE}counters_measured,counters_requested,"
  assert_csv_header "ibs-basic"        --no-ipc --ibs-basic       -- "${BASE}ibs_fetch,ibs_op,counters_measured,counters_requested,"
  assert_csv_header "ibs-memory-deep"  --no-ipc --ibs-memory-deep -- "${BASE}ibs_fetch,ibs_op,ibs_op_unfiltered,ibs_op_accepted_ratio,ibs_l3missonly,ibs_ldlat_threshold,ibs_fetchlat_threshold,counters_measured,counters_requested,"
  # Native multi-pass counter execution (--passes): ipc(3 counters)+cache2(6)
  # combined (9) exceeds the 6-slot budget, so this splits into exactly 2
  # passes (neither individually multiplexes) -- base/rusage columns once,
  # then each pass's own group columns concatenated in pass order via
  # print_metrics(), same column labels ("ipc"/"l2miss") as the single-pass
  # bundles above since it's the same print_metrics() call underneath.
  assert_csv_header "passes-ipc-cache2" --no-ipc --passes=ipc,cache2 -- "${BASE}ipc,l2miss,counters_measured,counters_requested,"
  # --multiplex collapses the same request into a single pass instead of 2,
  # but print_metrics() dispatches purely off the merged group list's own
  # mask bits (fixed dispatch order), independent of how many passes built
  # it -- same column shape as the bin-packed case above.
  assert_csv_header "passes-ipc-cache2-multiplex" --no-ipc --passes=ipc,cache2 --multiplex -- "${BASE}ipc,l2miss,counters_measured,counters_requested,"
else
  echo ""
  echo "SKIP: AMD-specific exact-header golden cases (host vendor: $vendor)"
fi

echo ""
echo "=== CSV header/value column-count contract (vendor-neutral) ==="
assert_csv_columns_match "no-ipc" --no-ipc
assert_csv_columns_match "default-ipc"
assert_csv_columns_match "topdown" --topdown
assert_csv_columns_match "topdown2" --topdown2
assert_csv_columns_match "topdown-frontend" --topdown-frontend
assert_csv_columns_match "topdown-backend" --topdown-backend
assert_csv_columns_match "topdown-optlb" --topdown-optlb
assert_csv_columns_match "branch" --branch
assert_csv_columns_match "cache1" --cache1
assert_csv_columns_match "cache2" --cache2
assert_csv_columns_match "cache3" --cache3
assert_csv_columns_match "dcache" --dcache
assert_csv_columns_match "icache" --icache
assert_csv_columns_match "tlb" --tlb
assert_csv_columns_match "memory" --memory
assert_csv_columns_match "opcache" --opcache
assert_csv_columns_match "float" --float
assert_csv_columns_match "software" --software
assert_csv_columns_match "system" --system
assert_csv_columns_match "rusage" --rusage
assert_csv_columns_match "no-rusage" --no-rusage
assert_csv_columns_match "ibs-basic" --ibs-basic
assert_csv_columns_match "ibs-memory-deep" --ibs-memory-deep
assert_csv_columns_match "power" --power
assert_csv_columns_match "arm-dcache-mem" --arm-dcache-mem
assert_csv_columns_match "arm-icache-tlb" --arm-icache-tlb
assert_csv_columns_match "arm-mem-align-tlb" --arm-mem-align-tlb
assert_csv_columns_match "kitchen-sink-combined" \
  --topdown --topdown-frontend --topdown-backend --topdown-optlb \
  --branch --dcache --icache --tlb --opcache --float --memory --software --system
# --per-core used to be the one documented exception to this check (see
# doc/ARTIFACT_CONTRACT.md's former "known gap" note and
# tests/capability_matrix.sh's per-core-topdown bundle comment): the header
# only ever had base/coverage columns while per-core rows appended a group's
# columns as unheaded extras. wspy.c's per_core_csv now emits one row per
# active core (tagged with a "core" column) whose columns match the header,
# so this combination is checked here like any other flag bundle.
assert_csv_columns_match "per-core-topdown" --no-ipc --per-core --topdown
assert_csv_columns_match "per-core-software" --no-ipc --per-core --software
# INVESTIGATION.md's 4.2 Tier 1 "Per-core energy support" item: power_core's
# new core_joules/core_watts columns land on every per-core row regardless
# of host power_core support (power_core_counter_group() returning NULL for
# every core degrades to zero extra columns, same "measured vs unavailable"
# shape as the other per-core bundles above) -- checked here generically
# rather than with an exact-string pin, same reasoning as those two.
assert_csv_columns_match "per-core-power" --no-ipc --per-core --power

echo ""
echo "=== CSV interval periodic-row column-count contract (vendor-neutral) ==="
# assert_csv_interval_rows_match checks EVERY row of a multi-tick --interval
# run against the header's column count, not just the header and the final
# (tail) row assert_csv_columns_match above checks. wspy.c's tail row and
# topdown.c:timer_callback()'s periodic-tick row are two separate print call
# sites that must independently stay in sync with the CSV header; a bug
# shipped where timer_callback() never called print_counter_coverage(), so
# every periodic row was missing the trailing counters_measured/
# counters_requested columns while the header and the one tail row both had
# them -- assert_csv_columns_match couldn't catch this because it only ever
# looks at the last row, and capability_matrix.sh's "interval" bundle uses
# `sleep 1` against `--interval 1`, too short for the alarm-driven tick to
# fire even once before the child exits.
assert_csv_interval_rows_match() {
  local label="$1"; shift
  local sleep_s="$1"; shift
  CHECKS=$((CHECKS + 1))
  local out header hcols nrows bad
  out=$("$WSPY" --csv --interval 1 "$@" -- sleep "$sleep_s" 2>/dev/null)
  header=$(echo "$out" | head -1)
  hcols=$(echo "$header" | tr ',' '\n' | wc -l)
  nrows=0
  bad=""
  while IFS= read -r row; do
    [ -z "$row" ] && continue
    nrows=$((nrows + 1))
    local vcols
    vcols=$(echo "$row" | tr ',' '\n' | wc -l)
    if [ "$vcols" != "$hcols" ]; then
      bad="row $nrows has $vcols column(s), expected $hcols: $row"
      break
    fi
  done < <(echo "$out" | tail -n +2)
  if [ -z "$header" ] || [ "$nrows" -lt 2 ] || [ -n "$bad" ]; then
    fail "csv interval row column count [$label]: flags='$*' rows=$nrows ${bad:-(fewer than 2 data rows -- periodic tick never fired)}
  full output:
$out"
  else
    echo "  csv interval row column count [$label]: OK ($hcols columns, $nrows rows)"
  fi
}
assert_csv_interval_rows_match "topdown" 3 --topdown --no-rusage --no-software --no-ipc
assert_csv_interval_rows_match "system"  3 --system --no-rusage --no-software --no-ipc
assert_csv_interval_rows_match "default-ipc" 3
# --per-core --interval: topdown.c:timer_callback()'s per-core branch
# (INVESTIGATION.md's "fix the --per-core + --interval CSV shape gap")
# emits one row per active core per tick instead of one systemwide row --
# this helper's per-row column-count check catches drift there exactly
# like it does for the non-per-core cases above, and its nrows>=2 floor
# is trivially satisfied (num_ticks * num_cores rows, not just num_ticks).
assert_csv_interval_rows_match "per-core-topdown" 3 --topdown --no-rusage --no-software --no-ipc --per-core
# --gpu-nvidia's periodic-tick columns are populated by topdown.c's own
# timer_callback(), a separate print call site from wspy.c's aggregate/tail
# row -- exactly the kind of drift this helper exists to catch (see the
# comment above it re: the pre-existing counters_measured/counters_requested
# bug). Only meaningful on an NVIDIA=1 build with a real GPU/driver present.
if [ "$nvidia_build" = "yes" ]; then
  assert_csv_interval_rows_match "gpu-nvidia" 3 --gpu-nvidia --no-rusage --no-software --no-ipc
fi

echo ""
echo "=== Normal (human-readable) output summary-fragment contract ==="
# These lines are printed unconditionally by print_usage()/print_system()
# regardless of perf permissions (they come from getrusage()/rusage/proc,
# not perf_event_open), so they're safe to check without root.
assert_normal_contains "base-rusage-labels" --no-ipc -- \
  '^elapsed +[0-9]'
assert_normal_contains "base-rusage-labels-oncpu" --no-ipc -- \
  '^on_cpu +[0-9]'
assert_normal_contains "base-rusage-labels-maxrss" --no-ipc -- \
  '^maxrss +[0-9]+ +# .* MB$'
assert_normal_contains "counter-coverage-line" --topdown -- \
  '^counter coverage +[0-9]+/[0-9]+ measured$'
assert_normal_contains "system-load-label" --no-ipc --system -- \
  '^load +[0-9]'
assert_normal_contains "system-freq-label" --no-ipc --system -- \
  '^freq +[0-9]+ MHz$'
# Degrades to 0.0 (not a failure) on a host/container with no matching hwmon
# driver, same "measured vs unavailable" tolerance as freq's 0 MHz above.
assert_normal_contains "system-temp-label" --no-ipc --system -- \
  '^cpu temp +[0-9]+\.[0-9]+ C$'
assert_normal_contains "system-loopback-iface" --no-ipc --system -- \
  '^lo +[0-9]'
assert_normal_contains "system-mem-free-label" --no-ipc --system -- \
  '^mem free +[0-9]+\.[0-9]+ MB$'
assert_normal_contains "system-swap-free-label" --no-ipc --system -- \
  '^swap free +[0-9]+\.[0-9]+ MB$'

echo ""
echo "=== Tree format contract ==="
# The line kinds documented in proctree.c's header comment (root/fork/exec/
# exit, plus the summary-checked "unknown" catch-all) plus the
# "# ptrace-summary" footer whose counters must match what was actually
# observed in the body -- a lighter, more general version of run_tests.sh's
# large stress-scale awk check (which stays there as an integrity/stress
# test at 2000 processes; this one pins the *grammar* itself at a scale
# small enough to read by eye when it fails).
check_tree_grammar() {
  local file="$1"
  local desc="$2"
  CHECKS=$((CHECKS + 1))
  if ! awk -v desc="$desc" '
    /^#/ {
      if ($2 == "ptrace-summary") {
        saw_summary = 1;
        for (i = 3; i <= NF; i++) {
          split($i, kv, "=");
          summary[kv[1]] = kv[2] + 0;
        }
      }
      next;
    }
    NF < 3 { next }
    {
      event = $3;
      if (event == "root") { root_count++; next }
      if (event == "fork") { fork_lines++; next }
      if (event == "exec") { exec_lines++; next }
      if (event == "exit") { exit_lines++; next }
      if (event == "unknown") { unknown_lines++; next }
    }
    END {
      errors = 0;
      if (root_count != 1) {
        printf("%s: expected exactly one root line, got %d\n", desc, root_count) > "/dev/stderr";
        errors++;
      }
      if (!saw_summary) {
        printf("%s: missing # ptrace-summary footer\n", desc) > "/dev/stderr";
        errors++;
      } else {
        if (summary["fork_events"] != fork_lines) {
          printf("%s: fork_events footer=%d actual=%d\n", desc, summary["fork_events"], fork_lines) > "/dev/stderr";
          errors++;
        }
        if (summary["exit_events"] != exit_lines) {
          printf("%s: exit_events footer=%d actual=%d\n", desc, summary["exit_events"], exit_lines) > "/dev/stderr";
          errors++;
        }
        if (summary["exec_events"] != exec_lines) {
          printf("%s: exec_events footer=%d actual=%d\n", desc, summary["exec_events"], exec_lines) > "/dev/stderr";
          errors++;
        }
        if (summary["unknown_traps"] != unknown_lines) {
          printf("%s: unknown_traps footer=%d actual=%d\n", desc, summary["unknown_traps"], unknown_lines) > "/dev/stderr";
          errors++;
        }
        if (!("wait_eintr" in summary)) {
          printf("%s: wait_eintr missing from footer\n", desc) > "/dev/stderr";
          errors++;
        }
      }
      exit (errors > 0) ? 1 : 0;
    }
  ' "$file"; then
    fail "tree grammar [$desc]: see stderr above; raw output:
$(cat "$file")"
  else
    echo "  tree grammar [$desc]: OK"
  fi
}

TMPDIR=$(mktemp -d /tmp/wspy_golden_tree.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT
TREE_SIMPLE="$TMPDIR/simple"
TREE_FORK="$TMPDIR/fork"
TREE_SYSCALLS="$TMPDIR/syscalls"

"$WSPY" --no-ipc --tree "$TREE_SIMPLE" -- /bin/true >/dev/null 2>&1
check_tree_grammar "$TREE_SIMPLE" "single-process"

"$WSPY" --no-ipc --tree "$TREE_FORK" -- /bin/sh -c '/bin/true & wait' >/dev/null 2>&1
check_tree_grammar "$TREE_FORK" "forking-workload"

echo "Testing tree syscall telemetry detail..."
cat > "$TMPDIR/syscalls_workload.c" <<'EOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <sys/syscall.h>
#include <linux/futex.h>

int main(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s >= 0) {
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9999);
        addr.sin_addr.s_addr = htonl(0x7F000001);
        connect(s, (struct sockaddr*)&addr, sizeof(addr));
        close(s);
    }
    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
    pid_t pid = fork();
    if (pid == 0) {
        exit(0);
    } else if (pid > 0) {
        int status;
        wait4(pid, &status, 0, NULL);
    }
    struct pollfd pfd;
    pfd.fd = 0;
    pfd.events = POLLIN;
    poll(&pfd, 1, 1);
    int futex_val = 0;
    struct timespec futex_ts = {0, 1000000};
    syscall(SYS_futex, &futex_val, FUTEX_WAIT_PRIVATE, 0, &futex_ts, NULL, 0);
    int pipefds[2];
    if (pipe(pipefds) == 0) {
        char buf[4] = "abc";
        if (write(pipefds[1], buf, 3) > 0) {}
        if (read(pipefds[0], buf, 3) > 0) {}
        close(pipefds[0]);
        close(pipefds[1]);
    }
    return 0;
}
EOF

if gcc -O2 "$TMPDIR/syscalls_workload.c" -o "$TMPDIR/syscalls_workload" >/dev/null 2>&1; then
  "$WSPY" --no-ipc --tree "$TREE_SYSCALLS" --tree-cmdline --tree-open --tree-futex --tree-vmsize --tree-io --tree-io-wait --tree-schedstat --tree-connect --tree-wait --tree-poll --tree-nanosleep -- "$TMPDIR/syscalls_workload" >/dev/null 2>&1
  check_tree_grammar "$TREE_SYSCALLS" "syscall-telemetry-grammar"
  
  for event in futex io_wait connect nanosleep wait poll io schedstat vmsize; do
    CHECKS=$((CHECKS + 1))
    if ! grep -q " $event " "$TREE_SYSCALLS"; then
      fail "tree telemetry [$event]: event '$event' not found in tree log"
    else
      echo "  tree telemetry [$event]: OK"
    fi
  done
else
  fail "tree telemetry [compilation]: failed to compile test workload"
fi

echo ""
echo "=== $CHECKS checks run, $FAILURES failed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
