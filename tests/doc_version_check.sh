#!/bin/bash
# tests/doc_version_check.sh - doc/version consistency check.
#
# INVESTIGATION.md's 4.2 "Doc/version consistency check" item: catches the
# exact class of drift found during the v4.0 release audit --
# doc/ARTIFACT_CONTRACT.md's schema-version examples silently falling behind
# MANIFEST_SCHEMA_VERSION/RUN_INDEX_SCHEMA_VERSION, and README.md missing a
# whole tool's section. Both were real, live bugs in this repo (not just a
# hypothetical) when this script was written -- see this item's own
# INVESTIGATION.md entry and the PR that added this file for the exact
# stale values/missing sections found and fixed alongside it.
#
# Deliberately grep-based, like every other check in tests/ -- not a JSON/
# Markdown parser. Two independent check classes:
#   1. Schema-version drift: for each *_SCHEMA_VERSION-style constant that
#      doc/ARTIFACT_CONTRACT.md actually quotes an example value for,
#      confirm every quoted occurrence (scoped to that constant's own "## "
#      section, so a value that legitimately appears elsewhere in the file
#      for an unrelated reason can't produce a false match) equals the real
#      #define in its home header. A constant this script knows about but
#      that the doc never quotes a version number for (e.g. today,
#      CURATION_SCHEMA_VERSION/COMPARE_SCHEMA_VERSION are named but not
#      version-quoted) is a WARN, not a FAIL -- there's nothing to compare
#      against, and mandating every constant be quoted isn't this check's
#      job.
#   2. Tool-list drift: every binary the Makefile's `all:` target (plus
#      $(GPU_BINS)) builds should be mentioned somewhere in README.md, and
#      vice versa -- a README section naming a tool with no matching
#      Makefile target most likely means the tool was removed and its docs
#      weren't.
#
# Static text/build-list check, not build-variant-dependent -- run once,
# not per GPU-build axis like golden_output.sh/capability_matrix.sh.
#
# Usage: ./tests/doc_version_check.sh (run from repo root; no build required).

set -u
cd "$(dirname "$0")/.." || exit 1

CONTRACT=doc/ARTIFACT_CONTRACT.md
FAILURES=0
CHECKS=0

fail() {
  echo "FAIL: $1"
  FAILURES=$((FAILURES + 1))
}

# Prints the value of #define NAME "value" or #define NAME value in FILE.
extract_define() {
  local file="$1" name="$2"
  grep -m1 -E "^#define ${name}[[:space:]]" "$file" \
    | sed -E 's/^#define [A-Za-z_]+[[:space:]]+"?([^"[:space:]]+)"?.*/\1/'
}

# Prints the lines of $CONTRACT between a "^## <heading>" line (matched
# literally, not as a regex, since section titles contain regex-special
# characters like backticks/parens) and the next "^## " heading.
extract_doc_section() {
  local heading="$1"
  awk -v heading="$heading" '
    index($0, heading) == 1 { found=1; next }
    found && /^## / { exit }
    found { print }
  ' "$CONTRACT"
}

# check_schema_version <label> <header_file> <constant> <doc_heading>
# Compares every quoted "schema_version": "X" occurrence within the doc
# section named <doc_heading> against the real #define in <header_file>.
check_schema_version() {
  local label="$1" header_file="$2" const_name="$3" doc_heading="$4"
  CHECKS=$((CHECKS + 1))

  local real_value
  real_value=$(extract_define "$header_file" "$const_name")
  if [ -z "$real_value" ]; then
    fail "$label: could not find #define $const_name in $header_file"
    return
  fi

  local doc_values
  doc_values=$(extract_doc_section "$doc_heading" \
    | grep -oE '"schema_version"[[:space:]]*:[[:space:]]*"[0-9]+\.[0-9]+(\.[0-9]+)?"' \
    | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?')
  if [ -z "$doc_values" ]; then
    echo "WARN: $label: $CONTRACT's \"$doc_heading\" section doesn't quote a schema_version example -- nothing to check"
    return
  fi

  local mismatch=0 count=0
  while IFS= read -r v; do
    count=$((count + 1))
    [ "$v" = "$real_value" ] || mismatch=1
  done <<< "$doc_values"

  if [ "$mismatch" -eq 1 ]; then
    fail "$label: $CONTRACT's \"$doc_heading\" section quotes a schema_version that doesn't match $const_name ($real_value in $header_file) -- found: $(echo "$doc_values" | tr '\n' ' ')"
  else
    echo "  $label: OK ($real_value, $count occurrence(s) checked)"
  fi
}

echo "=== Schema-version drift (doc/ARTIFACT_CONTRACT.md vs. header constants) ==="
check_schema_version "manifest schema version" manifest.h MANIFEST_SCHEMA_VERSION \
  '## Manifest (`--manifest <file>`)'
check_schema_version "run-index schema version" run_index.h RUN_INDEX_SCHEMA_VERSION \
  '## Run index (`--run-index <file>`)'
check_schema_version "proctree JSON schema version" proctree.c PROCTREE_JSON_SCHEMA_VERSION \
  '## Tree file (`--tree <file>`)'

# "Versioning contract"'s own prose summary ("Current versions as of this
# writing: manifest `X`, run index `Y`") is a distinct pattern from the
# JSON examples above -- caught a real, separately-stale copy of the same
# version numbers when this check was first written (the JSON examples and
# this prose sentence had each drifted independently), so it gets its own
# check rather than being assumed covered by the two above.
CHECKS=$((CHECKS + 1))
prose=$(grep -oE 'manifest `[0-9.]+`, run index `[0-9.]+`' "$CONTRACT" | head -1)
if [ -z "$prose" ]; then
  fail "could not find the \"Current versions as of this writing\" prose summary in $CONTRACT"
else
  prose_manifest=$(echo "$prose" | grep -oE 'manifest `[0-9.]+`' | grep -oE '[0-9.]+')
  prose_run_index=$(echo "$prose" | grep -oE 'run index `[0-9.]+`' | grep -oE '[0-9.]+')
  real_manifest=$(extract_define manifest.h MANIFEST_SCHEMA_VERSION)
  real_run_index=$(extract_define run_index.h RUN_INDEX_SCHEMA_VERSION)
  if [ "$prose_manifest" != "$real_manifest" ] || [ "$prose_run_index" != "$real_run_index" ]; then
    fail "\"Current versions as of this writing\" prose says manifest $prose_manifest/run index $prose_run_index, but the real values are $real_manifest/$real_run_index"
  else
    echo "  \"Current versions as of this writing\" prose: OK ($prose_manifest/$prose_run_index)"
  fi
fi

# Known-not-yet-version-quoted constants: named in the doc, but no example
# value to compare against today. Listed explicitly (rather than silently
# skipped) so a future reader knows this script is aware of them, not that
# it forgot them.
for pair in "STORE_SCHEMA_VERSION:store.c" "TOPDOWN_FORMULA_VERSION:wspy.h" \
            "CURATION_SCHEMA_VERSION:web/server.py" "COMPARE_SCHEMA_VERSION:web/server.py"; do
  name="${pair%%:*}"
  file="${pair##*:}"
  if ! grep -qE "\"?$name\"?" "$CONTRACT"; then
    echo "WARN: $name ($file) isn't mentioned in $CONTRACT at all"
  fi
done

echo ""
echo "=== Tool-list drift (Makefile binaries vs. README.md sections) ==="
makefile_bins=$(grep -m1 '^all:' Makefile | sed 's/^all:[[:space:]]*//' | tr -d '\t')
gpu_bins=$(grep -m1 '^GPU_BINS' Makefile | sed -E 's/^GPU_BINS[[:space:]]*[:+]?=[[:space:]]*//')
all_bins=$(echo "$makefile_bins $gpu_bins" | tr ' ' '\n' | sed -E 's/\$\(GPU_BINS\)//' | grep -v '^$')

for bin in $all_bins; do
  CHECKS=$((CHECKS + 1))
  if grep -q -- "$bin" README.md; then
    echo "  $bin: OK (mentioned in README.md)"
  else
    fail "$bin is built by 'make all' but not mentioned anywhere in README.md"
  fi
done

# Reverse direction: a README "## <tool>" heading naming a wspy-prefixed
# tool that isn't a real binary anymore (excludes prose-only headings like
# "Building"/"Usage"/"Other contents").
while IFS= read -r heading; do
  tool=$(echo "$heading" | grep -oE 'wspy-[a-z-]+' | head -1)
  [ -n "$tool" ] || continue
  CHECKS=$((CHECKS + 1))
  if echo "$all_bins" | grep -qx "$tool" || [ -x "./$tool" ]; then
    : # real Makefile-built binary, or a real standalone script/binary in the repo root
  else
    fail "README.md has a section for '$tool' but it's not a Makefile binary or an executable in the repo root"
  fi
done < <(grep -E '^## .*wspy-' README.md)

echo ""
echo "=== $CHECKS checks run, $FAILURES failed ==="
if [ "$FAILURES" -gt 0 ]; then
  exit 1
fi
exit 0
