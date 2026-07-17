#!/bin/bash
# Smoke test for wspy-analyze (INVESTIGATION_4.0.md's "Local LLM (Ollama)
# narrative-analysis deep-dive"). Not part of run_tests.sh/make test -- like
# test_amd_smi.sh, this needs external state (a real wspy-validate build,
# and for the live-call section, a running Ollama daemon with at least one
# model pulled) that the unprivileged unit-test suite doesn't assume.
#
# The structural (--dry-run) checks below need no daemon at all and always
# run; the live-call section self-skips (not fails) when `ollama` isn't on
# PATH or `ollama list` reports no models, same degrade-don't-fail idiom
# used throughout the C tools.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

echo "=== Building wspy-validate ==="
make wspy-validate

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT
RUNDIR="$WORKDIR/manual/sleep/test-run-1"
mkdir -p "$RUNDIR"

cat > "$RUNDIR/manifest.json" <<'EOF'
{
  "suite": "manual",
  "benchmark": "sleep",
  "run_id": "test-run-1",
  "command": ["sleep", "1"],
  "passes": [
    {"name": "amdtopdown", "output": "amdtopdown.csv", "manifest": "amdtopdown.manifest.json", "status": "ok"}
  ]
}
EOF

cat > "$RUNDIR/amdtopdown.manifest.json" <<'EOF'
{
  "schema_version": "1.6.0",
  "output_files": {"output": "amdtopdown.csv"},
  "output": "amdtopdown.csv",
  "exit_status": {"known": true, "code": 0, "signaled": false},
  "counter_coverage": {"measured": 4, "requested": 4},
  "timing": {"elapsed_seconds": 1.02}
}
EOF

cat > "$RUNDIR/amdtopdown.csv" <<'EOF'
time,retire,frontend,backend,speculate
0.0,62.30,10.10,25.10,2.50
1.0,58.10,12.40,27.00,2.50
EOF

cat > "$RUNDIR/summary.txt" <<'EOF'
elapsed: 1.02s
ipc: 1.83
retire: 60.2%  frontend: 11.3%  backend: 26.1%  speculate: 2.5%
EOF

echo ""
echo "=== Testing wspy-analyze --dry-run (prompt rendering, no Ollama needed) ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --dry-run 2>&1)"
echo "$OUT" | grep -q "PERF_ANALYSIS_TEMPLATE_VERSION" || { echo "FAIL: template version marker missing"; exit 1; }
echo "$OUT" | grep -q "command: sleep 1" || { echo "FAIL: workload command missing from rendered prompt"; exit 1; }
echo "$OUT" | grep -q "top-down pipeline-slot breakdown" || { echo "FAIL: topdown group note missing"; exit 1; }
echo "$OUT" | grep -q "retire: 60.2%" || { echo "FAIL: raw counter text not inlined verbatim"; exit 1; }
[ -f "$RUNDIR/aiprompt.txt" ] || { echo "FAIL: aiprompt.txt not written"; exit 1; }
echo "dry-run prompt rendering OK"

echo ""
echo "=== Testing --redact-command ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --dry-run --redact-command 2>&1)"
echo "$OUT" | grep -q "command redacted" || { echo "FAIL: --redact-command did not redact the command"; exit 1; }
echo "$OUT" | grep -q "command: sleep 1" && { echo "FAIL: --redact-command still leaked the command"; exit 1; }
echo "--redact-command OK"

echo ""
echo "=== Testing missing-manifest degrade-don't-fail path ==="
BAREDIR="$WORKDIR/bare"
mkdir -p "$BAREDIR"
cp "$RUNDIR/summary.txt" "$BAREDIR/summary.txt"
OUT="$(./wspy-analyze --rundir "$BAREDIR" --dry-run 2>&1)"
echo "$OUT" | grep -q "no per-pass manifest.json found" || { echo "FAIL: missing-manifest case did not degrade gracefully"; exit 1; }
echo "missing-manifest degrade OK"

if ! command -v ollama >/dev/null 2>&1; then
    echo ""
    echo "=== ollama not on PATH -- skipping live-call section ==="
    echo "=== All structural tests passed ==="
    exit 0
fi

# Pick the smallest non-embedding model actually installed, via the same
# /api/tags endpoint wspy-analyze itself uses -- a smoke test should be fast,
# and "first row of `ollama list`" isn't size-ordered (it can land on a
# multi-GB model and blow past any reasonable timeout).
MODEL="$(python3 -c '
import json, urllib.request
try:
    with urllib.request.urlopen("http://localhost:11434/api/tags", timeout=5) as r:
        models = json.load(r).get("models", [])
except Exception:
    models = []
models = [m for m in models if "embed" not in m.get("name", "").lower()]
if models:
    print(min(models, key=lambda m: m.get("size", 0))["name"])
')"
if [ -z "$MODEL" ]; then
    echo ""
    echo "=== no models installed ('ollama list' empty) -- skipping live-call section ==="
    echo "=== All structural tests passed ==="
    exit 0
fi

echo ""
echo "=== Testing a real Ollama call against $MODEL ==="
./wspy-analyze --rundir "$RUNDIR" --model "$MODEL" --timeout 120
SLUG="$(printf '%s' "$MODEL" | tr -c 'A-Za-z0-9._-' '_')"
ANALYSIS="$RUNDIR/aianalysis.$SLUG.txt"
[ -s "$ANALYSIS" ] || { echo "FAIL: $ANALYSIS missing or empty"; exit 1; }
echo "live call OK ($(wc -c < "$ANALYSIS") bytes from $MODEL)"

echo ""
echo "=== All tests completed successfully ==="
