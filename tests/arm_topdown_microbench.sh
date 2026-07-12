#!/bin/bash
# tests/arm_topdown_microbench.sh
#
# ARM-focused microbench sanity checks for the topdown-equivalent path.
# Requires perf counter access.

set -euo pipefail
cd "$(dirname "$0")/.." || exit 1

WSPY=./wspy
CPU_INFO=./cpu_info
TMPDIR=$(mktemp -d /tmp/wspy_armbench.XXXXXX)
trap 'rm -rf "$TMPDIR"' EXIT

if ! "$CPU_INFO" 2>/dev/null | grep -Eq '^[[:space:]]*ARM family '; then
  echo "SKIP: arm_topdown_microbench.sh only runs on ARM hosts"
  exit 0
fi

cat > "$TMPDIR/compute.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
int main(void){
  volatile uint64_t x = 1;
  for (uint64_t i=0;i<350000000ULL;i++) x = x * 1664525ULL + 1013904223ULL;
  printf("%llu\n", (unsigned long long)x);
  return 0;
}
EOF

cat > "$TMPDIR/memory.c" <<'EOF'
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(void){
  const size_t n = 1u << 26; /* 64 MiB */
  uint64_t *a = (uint64_t*)malloc(n * sizeof(uint64_t));
  if (!a) return 1;
  for (size_t i=0;i<n;i++) a[i] = (uint64_t)((i * 1315423911u) & (n - 1));
  volatile uint64_t idx = 1;
  volatile uint64_t sum = 0;
  for (uint64_t i=0;i<120000000ULL;i++) {
    idx = a[idx];
    sum += idx;
  }
  printf("%llu\n", (unsigned long long)sum);
  free(a);
  return 0;
}
EOF

gcc -O2 "$TMPDIR/compute.c" -o "$TMPDIR/compute"
gcc -O2 "$TMPDIR/memory.c" -o "$TMPDIR/memory"

extract_metric() {
  local csv="$1"
  local metric="$2"
  local header row idx
  header=$(echo "$csv" | grep '^elapsed,' | head -1)
  row=$(echo "$csv" | grep -v '^elapsed,' | grep -v '^warning:' | grep -v '^error:' | tail -1)
  idx=$(echo "$header" | awk -F',' -v m="$metric" '{for(i=1;i<=NF;i++) if($i==m){print i; exit}}')
  if [[ -z "${idx:-}" ]]; then
    echo ""
    return
  fi
  echo "$row" | awk -F',' -v i="$idx" '{print $i}'
}

run_case() {
  local label="$1"
  local bin="$2"
  local out
  out=$($WSPY --csv --topdown -- "$bin" 2>&1)

  if echo "$out" | grep -q 'Permission denied'; then
    echo "SKIP: perf counters not accessible for $label"
    exit 0
  fi

  local retire frontend backend speculate
  retire=$(extract_metric "$out" "retire")
  frontend=$(extract_metric "$out" "frontend")
  backend=$(extract_metric "$out" "backend")
  speculate=$(extract_metric "$out" "speculate")

  if [[ -z "$retire" || -z "$frontend" || -z "$backend" || -z "$speculate" ]]; then
    echo "FAIL: missing topdown columns in $label output"
    echo "$out"
    exit 1
  fi

  echo "$label: retire=$retire frontend=$frontend backend=$backend speculate=$speculate"
}

compute_csv=$($WSPY --csv --topdown -- "$TMPDIR/compute" 2>&1)
if echo "$compute_csv" | grep -q 'Permission denied'; then
  echo "SKIP: perf counters not accessible for compute run"
  exit 0
fi
memory_csv=$($WSPY --csv --topdown -- "$TMPDIR/memory" 2>&1)

compute_retire=$(extract_metric "$compute_csv" "retire")
compute_backend=$(extract_metric "$compute_csv" "backend")
memory_retire=$(extract_metric "$memory_csv" "retire")
memory_backend=$(extract_metric "$memory_csv" "backend")

if [[ -z "$compute_retire" || -z "$compute_backend" || -z "$memory_retire" || -z "$memory_backend" ]]; then
  echo "FAIL: missing topdown columns in microbench outputs"
  echo "compute:\n$compute_csv"
  echo "memory:\n$memory_csv"
  exit 1
fi

echo "compute: retire=$compute_retire backend=$compute_backend"
echo "memory:  retire=$memory_retire backend=$memory_backend"

# Sanity expectation: workloads should show a meaningfully different topdown mix.
awk -v cr="$compute_retire" -v mr="$memory_retire" -v cb="$compute_backend" -v mb="$memory_backend" '
BEGIN {
  dr = cr - mr;
  if (dr < 0) dr = -dr;
  db = cb - mb;
  if (db < 0) db = -db;
  if (dr < 8.0 && db < 8.0) exit 1;
}
' || {
  echo "FAIL: expected a clear topdown difference between compute and memory workloads"
  exit 1
}

echo "PASS: ARM topdown microbench sanity checks"
