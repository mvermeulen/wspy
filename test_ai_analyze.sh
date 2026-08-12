#!/bin/bash
# Smoke test for wspy-analyze (INVESTIGATION.md's "Local LLM (Ollama)
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

# IBS counting-mode CSV (--ibs-basic/--ibs-memory-deep, wspy-run's
# "ibs"-named pass with --csv): the gap this test is guarding against --
# collect_raw_text() never sees this data (it's a CSV, not summary.txt/
# *.txt), so without collect_ibs_csv_summaries() it would be completely
# invisible to the model despite being real collected data.
cat > "$RUNDIR/ibs.csv" <<'EOF'
time,ibs_fetch,ibs_op
0.0,1000,2000
1.0,3000,4000
EOF

# A second run directory (run B) for --compare-rundir mode -- a materially
# different topdown split (backend-bound rather than retire-bound) so the
# comparative prompt actually has something to describe changing.
RUNDIR_B="$WORKDIR/manual/sleep/test-run-2"
mkdir -p "$RUNDIR_B"

cat > "$RUNDIR_B/manifest.json" <<'EOF'
{
  "suite": "manual",
  "benchmark": "sleep",
  "run_id": "test-run-2",
  "command": ["sleep", "1"],
  "passes": [
    {"name": "amdtopdown", "output": "amdtopdown.csv", "manifest": "amdtopdown.manifest.json", "status": "ok"}
  ]
}
EOF

cat > "$RUNDIR_B/amdtopdown.manifest.json" <<'EOF'
{
  "schema_version": "1.6.0",
  "output_files": {"output": "amdtopdown.csv"},
  "output": "amdtopdown.csv",
  "exit_status": {"known": true, "code": 0, "signaled": false},
  "counter_coverage": {"measured": 4, "requested": 4},
  "timing": {"elapsed_seconds": 1.10}
}
EOF

cat > "$RUNDIR_B/amdtopdown.csv" <<'EOF'
time,retire,frontend,backend,speculate
0.0,40.10,10.00,45.10,4.80
1.0,38.50,11.00,46.00,4.50
EOF

cat > "$RUNDIR_B/summary.txt" <<'EOF'
elapsed: 1.10s
ipc: 1.10
retire: 39.3%  frontend: 10.5%  backend: 45.6%  speculate: 4.7%
EOF

cat > "$RUNDIR_B/ibs.csv" <<'EOF'
time,ibs_fetch,ibs_op
0.0,5000,6000
1.0,7000,8000
EOF

# --image mode fixtures (INVESTIGATION.md's "Vision-based topdown-chart
# analysis deep-dive"): a real, if trivial, decodable PNG -- --dry-run
# itself never decodes it (base64'd and shipped to Ollama as opaque bytes,
# so a handful of fake bytes would pass every structural check below just
# as well), but the live-call section does send it to a real Ollama
# daemon, which rejects a non-decodable file with its own 400 Bad Request
# before ever reaching the model (confirmed live). Built with stdlib
# zlib/struct rather than a placeholder, so one fixture covers both
# sections without adding a PIL/Pillow dependency to a test that otherwise
# needs none.
mkdir -p "$RUNDIR/plots"
python3 -c '
import struct, zlib
def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
sig = b"\x89PNG\r\n\x1a\n"
ihdr = chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 2, 0, 0, 0))  # 1x1, 8-bit, RGB
idat = chunk(b"IDAT", zlib.compress(b"\x00\xff\x00\x00"))  # filter byte + one RGB pixel
iend = chunk(b"IEND", b"")
with open("'"$RUNDIR"'/plots/amdtopdown.topdown.png", "wb") as f:
    f.write(sig + ihdr + idat + iend)
'

echo ""
echo "=== Testing wspy-analyze --dry-run (prompt rendering, no Ollama needed) ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --dry-run 2>&1)"
echo "$OUT" | grep -q "PERF_ANALYSIS_TEMPLATE_VERSION" || { echo "FAIL: template version marker missing"; exit 1; }
echo "$OUT" | grep -q "command: sleep 1" || { echo "FAIL: workload command missing from rendered prompt"; exit 1; }
echo "$OUT" | grep -q "top-down pipeline-slot breakdown" || { echo "FAIL: topdown group note missing"; exit 1; }
echo "$OUT" | grep -q "retire: 60.2%" || { echo "FAIL: raw counter text not inlined verbatim"; exit 1; }
[ -f "$RUNDIR/aiprompt.md" ] || { echo "FAIL: aiprompt.md not written"; exit 1; }
echo "dry-run prompt rendering OK"

echo ""
echo "=== Testing IBS CSV summarization (the gap this fix addresses) ==="
echo "$OUT" | grep -q "AMD IBS" || { echo "FAIL: ibs group note missing (ibs.csv header not detected)"; exit 1; }
echo "$OUT" | grep -q "### ibs.csv" || { echo "FAIL: ibs.csv summary block missing from prompt"; exit 1; }
echo "$OUT" | grep -qE '\| ibs_fetch \| 2 \| 1000 \| 3000 \| 2000 \| 1000 \|' || {
    echo "FAIL: ibs_fetch summary stats wrong or missing"; exit 1; }
echo "$OUT" | grep -qE '\| ibs_op \| 2 \| 2000 \| 4000 \| 3000 \| 1000 \|' || {
    echo "FAIL: ibs_op summary stats wrong or missing"; exit 1; }
echo "IBS CSV summarization OK"

echo ""
echo "=== Testing --csv-summary-max-bytes skip path (degrade, don't fail, on an oversized CSV) ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --dry-run --csv-summary-max-bytes 10 2>&1)"
echo "$OUT" | grep -q "### ibs.csv" || { echo "FAIL: ibs.csv block missing entirely when oversized"; exit 1; }
echo "$OUT" | grep -q "skipped -- .* bytes exceeds the 10-byte summarization cap" || {
    echo "FAIL: oversized ibs.csv did not degrade to a skip placeholder"; exit 1; }
echo "$OUT" | grep -q '| ibs_fetch |' && { echo "FAIL: oversized ibs.csv still got summarized"; exit 1; }
echo "--csv-summary-max-bytes skip path OK"

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

echo ""
echo "=== Testing --compare-rundir (comparative mode, design decision #8) ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --compare-rundir "$RUNDIR_B" --dry-run 2>&1)"
echo "$OUT" | grep -q "PERF_COMPARE_TEMPLATE_VERSION" || { echo "FAIL: compare template version marker missing"; exit 1; }
echo "$OUT" | grep -q "run_id=test-run-1" || { echo "FAIL: run A identity missing from compare prompt"; exit 1; }
echo "$OUT" | grep -q "run_id=test-run-2" || { echo "FAIL: run B identity missing from compare prompt"; exit 1; }
echo "$OUT" | grep -q "retire: 60.2%" || { echo "FAIL: run A raw counter text missing from compare prompt"; exit 1; }
echo "$OUT" | grep -q "retire: 39.3%" || { echo "FAIL: run B raw counter text missing from compare prompt"; exit 1; }
echo "$OUT" | grep -qE '\| ibs_fetch \| 2 \| 1000 \| 3000 \| 2000 \| 1000 \|' || {
    echo "FAIL: run A IBS CSV summary missing from compare prompt"; exit 1; }
echo "$OUT" | grep -qE '\| ibs_fetch \| 2 \| 5000 \| 7000 \| 6000 \| 1000 \|' || {
    echo "FAIL: run B IBS CSV summary missing from compare prompt"; exit 1; }
[ -f "$RUNDIR/aiprompt.compare.manual-sleep-test-run-2.md" ] || {
    echo "FAIL: aiprompt.compare.manual-sleep-test-run-2.md not written into run A's directory"; exit 1; }
echo "--compare-rundir dry-run OK"

echo ""
echo "=== Testing --default-model fallback: not installed falls through to explicit-choice error ==="
if OUT="$(./wspy-analyze --rundir "$RUNDIR" --default-model "definitely-not-a-real-model:latest" 2>&1)"; then
    echo "FAIL: expected a nonzero exit when the default model isn't installed"
    exit 1
fi
echo "$OUT" | grep -q "default model 'definitely-not-a-real-model:latest' is not installed" || {
    echo "FAIL: expected error message mentioning the unavailable default model"; exit 1; }
echo "--default-model not-installed fallback OK"

echo ""
echo "=== Testing --image auto-discovery + dry-run (vision deep-dive) ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --image --dry-run 2>&1)"
echo "$OUT" | grep -q "VISION_TOPDOWN_TEMPLATE_VERSION" || {
    echo "FAIL: vision template version marker missing"; exit 1; }
echo "$OUT" | grep -q "command: sleep 1" || { echo "FAIL: workload command missing from vision prompt"; exit 1; }
echo "$OUT" | grep -q "### amdtopdown.csv" || {
    echo "FAIL: numeric grounding table (source CSV summary) missing from vision prompt"; exit 1; }
echo "$OUT" | grep -qE '\| retire \| 2 \| 58.1 \| 62.3 \| 60.2 \|' || {
    echo "FAIL: grounding table stats wrong or missing for retire column"; exit 1; }
[ -f "$RUNDIR/aiprompt.vision.amdtopdown.topdown.md" ] || {
    echo "FAIL: aiprompt.vision.amdtopdown.topdown.md not written"; exit 1; }
echo "--image auto-discovery dry-run OK"

echo ""
echo "=== Testing --image with an explicit path ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --image plots/amdtopdown.topdown.png --dry-run 2>&1)"
echo "$OUT" | grep -q "VISION_TOPDOWN_TEMPLATE_VERSION" || {
    echo "FAIL: explicit --image path did not render the vision template"; exit 1; }
echo "--image explicit-path dry-run OK"

echo ""
echo "=== Testing --image auto-discovery error paths ==="
BAREDIR2="$WORKDIR/bare-no-plots"
mkdir -p "$BAREDIR2"
if OUT="$(./wspy-analyze --rundir "$BAREDIR2" --image --dry-run 2>&1)"; then
    echo "FAIL: expected a nonzero exit with no plots/*.topdown.png present"; exit 1
fi
echo "$OUT" | grep -q "auto-discovery found no plots" || {
    echo "FAIL: expected 'found no plots' error with no plots present"; exit 1; }

mkdir -p "$RUNDIR/plots"
cp "$RUNDIR/plots/amdtopdown.topdown.png" "$RUNDIR/plots/second.topdown.png"
if OUT="$(./wspy-analyze --rundir "$RUNDIR" --image --dry-run 2>&1)"; then
    echo "FAIL: expected a nonzero exit with more than one plots/*.topdown.png present"; exit 1
fi
echo "$OUT" | grep -q "auto-discovery found more than one plots" || {
    echo "FAIL: expected 'found more than one' error with two topdown plots present"; exit 1; }
rm "$RUNDIR/plots/second.topdown.png"
echo "--image auto-discovery error paths OK"

echo ""
echo "=== Testing --image / --compare-rundir mutual exclusion ==="
if OUT="$(./wspy-analyze --rundir "$RUNDIR" --image --compare-rundir "$RUNDIR_B" --dry-run 2>&1)"; then
    echo "FAIL: expected a nonzero exit combining --image and --compare-rundir"; exit 1
fi
echo "$OUT" | grep -q "mutually exclusive" || {
    echo "FAIL: expected a mutual-exclusion error combining --image and --compare-rundir"; exit 1; }
echo "--image/--compare-rundir mutual exclusion OK"

if ! command -v ollama >/dev/null 2>&1; then
    echo ""
    echo "=== ollama not on PATH -- skipping live-call section ==="
    echo "=== All structural tests passed ==="
    exit 0
fi

# Pick the smallest non-embedding, non-base model actually installed, via
# the same /api/tags endpoint wspy-analyze itself uses -- a smoke test
# should be fast, and "first row of `ollama list`" isn't size-ordered (it
# can land on a multi-GB model and blow past any reasonable timeout). Also
# excludes "base" (non-instruction-tuned) models: confirmed live that one
# (a 1.5b base checkpoint, otherwise the smallest model on a real dev host)
# doesn't reliably follow the "write N sentences" task at all -- it free-
# associates new document sections instead of stopping, so instead of being
# merely a low-quality analysis it can run for a very long time before
# hitting a stop condition, which is exactly wrong for a smoke test that
# needs to finish quickly and deterministically.
MODEL="$(python3 -c '
import json, urllib.request
try:
    with urllib.request.urlopen("http://localhost:11434/api/tags", timeout=5) as r:
        models = json.load(r).get("models", [])
except Exception:
    models = []
models = [m for m in models
          if "embed" not in m.get("name", "").lower()
          and "base" not in m.get("name", "").lower()]
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
ANALYSIS="$RUNDIR/aianalysis.$SLUG.md"
[ -s "$ANALYSIS" ] || { echo "FAIL: $ANALYSIS missing or empty"; exit 1; }
echo "live call OK ($(wc -c < "$ANALYSIS") bytes from $MODEL)"

echo ""
echo "=== Testing --default-model fallback: installed model is used automatically ==="
OUT="$(./wspy-analyze --rundir "$RUNDIR" --default-model "$MODEL" --timeout 120 2>&1)"
echo "$OUT" | grep -qF "no --model given -- defaulting to $MODEL" || {
    echo "FAIL: expected a 'defaulting to' message when --default-model is installed"; exit 1; }
[ -s "$ANALYSIS" ] || { echo "FAIL: $ANALYSIS missing or empty after default-model run"; exit 1; }
echo "--default-model installed fallback OK"

echo ""
echo "=== Testing a real Ollama call against $MODEL (comparative mode) ==="
./wspy-analyze --rundir "$RUNDIR" --compare-rundir "$RUNDIR_B" --model "$MODEL" --timeout 120
COMPARE_ANALYSIS="$RUNDIR/aianalysis.compare.manual-sleep-test-run-2.$SLUG.md"
[ -s "$COMPARE_ANALYSIS" ] || { echo "FAIL: $COMPARE_ANALYSIS missing or empty"; exit 1; }
echo "comparative live call OK ($(wc -c < "$COMPARE_ANALYSIS") bytes from $MODEL)"

# --image mode's live-call section is separately gated (own model pick,
# skips independently of the text-mode section above): a text-only model
# can't do this task at all, so the same $MODEL picked above is very
# possibly not usable here, and vice versa. Uses a longer --timeout than
# the text-mode calls above -- this codebase's own live comparison
# (INVESTIGATION.md's vision deep-dive) found multi-minute vision calls
# normal on CPU-bound-mmproj hardware, not exceptional; ollama_generate()'s
# per-chunk-idle timeout still bounds a genuinely stuck model (confirmed
# live against exactly that failure mode), it just needs more slack than a
# text call before declaring one.
VISION_MODEL="$(python3 -c '
import json, urllib.request
try:
    with urllib.request.urlopen("http://localhost:11434/api/tags", timeout=5) as r:
        models = json.load(r).get("models", [])
except Exception:
    models = []
models = [m for m in models if "vision" in m.get("capabilities", [])]
if models:
    print(min(models, key=lambda m: m.get("size", 0))["name"])
')"
if [ -z "$VISION_MODEL" ]; then
    echo ""
    echo "=== no vision-capable models installed -- skipping --image live-call section ==="
else
    echo ""
    echo "=== Testing a real Ollama call against $VISION_MODEL (--image mode) ==="
    ./wspy-analyze --rundir "$RUNDIR" --image --model "$VISION_MODEL" --timeout 240
    VISION_SLUG="$(printf '%s' "$VISION_MODEL" | tr -c 'A-Za-z0-9._-' '_')"
    VISION_ANALYSIS="$RUNDIR/aivision.amdtopdown.topdown.$VISION_SLUG.md"
    [ -s "$VISION_ANALYSIS" ] || { echo "FAIL: $VISION_ANALYSIS missing or empty"; exit 1; }
    echo "--image live call OK ($(wc -c < "$VISION_ANALYSIS") bytes from $VISION_MODEL)"
fi

echo ""
echo "=== All tests completed successfully ==="
