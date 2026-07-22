#!/usr/bin/env bash
set -euo pipefail

# scripts/release_prep.sh - release-prep checklist/script.
#
# INVESTIGATION.md's 4.2 "Release-prep checklist/script" item: captures the
# v4.0/v4.1/v4.1.1/v4.2 release process (version bump, stale-version-reference
# check, full test matrix, merged-PR label audit, release-notes draft, tag/
# publish) as a repeatable script instead of redoing it by hand every phase.
#
# Deliberately never runs the hard-to-reverse, publicly-visible steps
# itself -- git tag, git push of the tag, gh release create are always
# print-only, for the user to run by hand once ready, matching how every
# push/PR gets handled in this project's own workflow (asked first, never
# auto-executed). The one GitHub-side mutation this script *can* perform
# (applying a missing release label to an already-merged PR) is low-risk
# and trivially reversible, unlike those three -- and -y still gates it.
#
# Needs `gh` (authenticated) for the PR/label audit and release-notes
# draft; degrades to a skip-with-warning, not a hard failure, if `gh`
# isn't available or authenticated -- same "measured vs unavailable" idiom
# used throughout this codebase (see scripts/setup_perf.sh for the same
# check-then-prompt/-y convention this script also follows).
#
# Label application uses `gh api repos/{owner}/{repo}/issues/<n>/labels`
# (the REST endpoint), not `gh pr edit --add-label` -- confirmed live that
# `gh pr edit` fails outright on this repo with a "Projects (classic) is
# being deprecated" GraphQL error (its mutation response tries to also
# fetch project-card data, which errors out and aborts the whole request,
# so the label silently never gets applied even though `gh pr edit` exits
# nonzero, which the script's own `&&` already catches). `{owner}`/`{repo}`
# are `gh`'s own template placeholders, resolved from the repo's git
# remote, so this isn't hardcoded to a specific fork.
#
# Usage: ./scripts/release_prep.sh --version X.Y[.Z] [options]

SCRIPT_NAME=$(basename "$0")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "$REPO_ROOT"

VERSION=""
SINCE_TAG=""
AUTO_APPLY=0
SKIP_TESTS=0

usage() {
  cat <<EOF
Usage: $SCRIPT_NAME --version X.Y[.Z] [options]

Runs the release-prep checklist: pre-flight checks, a merged-PR/label
audit, a version bump, a stale-version-reference grep, the full test
matrix, a release-notes draft, doc bookkeeping reminders, and finally the
exact tag/push/publish commands to run by hand -- this script never runs
those three itself.

Options:
  --version X.Y[.Z]   new wspy version (required for the version-bump and
                      stale-reference-grep phases; the audit/test-matrix
                      phases still run without it). X.Y is shorthand for
                      X.Y.0 -- wspy.h always carries a MAJOR/MINOR/PATCH
                      triple, but every X.Y.0 release so far has tagged as
                      plain "vX.Y" (v4.0, v4.1, v4.2), not "vX.Y.0" -- only a
                      real patch release tags as "vX.Y.Z" (v4.1.1). This
                      script now derives the tag/title from the resulting
                      patch number rather than echoing back whatever was
                      typed, so either form produces the right tag.
  --since <tag>       compare against this tag instead of the most recent
                      one (default: \`git describe --tags --abbrev=0\`)
  -y                  apply the missing-release-label fix without
                      prompting per PR (still never touches tag/push/release)
  --skip-tests        skip phase 5 (./run_tests.sh) -- for iterating on this
                      script itself; a real release should not use this
  -h, --help          show this help

Phases that are always print-only, never executed by this script: git tag,
git push of the tag, gh release create. Phases needing real editorial
judgment (release-notes prose/grouping, INVESTIGATION.md's "Shipped since"
fold-in and Status line, whether a flagged stale-reference hit is a real
bug) are printed as reminders, not auto-applied.
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="$2"; shift 2 ;;
    --since) SINCE_TAG="$2"; shift 2 ;;
    -y) AUTO_APPLY=1; shift ;;
    --skip-tests) SKIP_TESTS=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "$SCRIPT_NAME: unknown option '$1'" >&2; usage >&2; exit 2 ;;
  esac
done

if [ -n "$VERSION" ] && ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
  echo "$SCRIPT_NAME: --version must be X.Y or X.Y.Z (got '$VERSION')" >&2
  exit 2
fi
# X.Y is shorthand for X.Y.0 -- normalize once here so every phase below
# can keep assuming a full MAJOR.MINOR.PATCH triple (wspy.h always has all
# three #defines). DISPLAY_VERSION is the separate, tag-facing form: drop
# the patch back off when it's 0, since that's the convention every X.Y.0
# release has actually tagged/titled with (v4.0, v4.1, v4.2 -- not
# v4.0.0/v4.1.0/v4.2.0; only a real patch release like v4.1.1 keeps the
# third component).
DISPLAY_VERSION=""
if [ -n "$VERSION" ]; then
  if [[ "$VERSION" =~ ^[0-9]+\.[0-9]+$ ]]; then
    VERSION="${VERSION}.0"
  fi
  IFS=. read -r _V_MAJOR _V_MINOR _V_PATCH <<< "$VERSION"
  if [ "$_V_PATCH" = "0" ]; then
    DISPLAY_VERSION="${_V_MAJOR}.${_V_MINOR}"
  else
    DISPLAY_VERSION="$VERSION"
  fi
fi

HAVE_GH=1
if ! command -v gh >/dev/null 2>&1 || ! gh auth status >/dev/null 2>&1; then
  HAVE_GH=0
fi

echo "=== Phase 1: pre-flight ==="
branch=$(git rev-parse --abbrev-ref HEAD)
if [ "$branch" != "master" ]; then
  echo "WARN: on branch '$branch', not master -- release prep is normally done from master"
fi
if [ -n "$(git status --porcelain)" ]; then
  echo "WARN: working tree is not clean -- commit or stash before proceeding"
fi
git fetch origin master --quiet 2>/dev/null || true
local_head=$(git rev-parse HEAD 2>/dev/null || echo "")
remote_head=$(git rev-parse origin/master 2>/dev/null || echo "")
if [ -n "$remote_head" ] && [ "$local_head" != "$remote_head" ] && [ "$branch" = "master" ]; then
  echo "WARN: local master ($local_head) differs from origin/master ($remote_head) -- pull first"
fi
if [ "$HAVE_GH" -eq 1 ]; then
  open_count=$(gh pr list --state open --base master --json number --jq 'length' 2>/dev/null || echo "?")
  echo "  $open_count open PR(s) against master -- review whether any should be included before tagging"
else
  echo "  (skipped: gh not available/authenticated)"
fi
echo ""

if [ -z "$SINCE_TAG" ]; then
  SINCE_TAG=$(git describe --tags --abbrev=0 2>/dev/null || echo "")
fi
echo "=== Phase 2: merged-PR / release-label audit (since ${SINCE_TAG:-<no prior tag>}) ==="
matched_prs_json="[]"
if [ "$HAVE_GH" -eq 0 ]; then
  echo "  (skipped: gh not available/authenticated)"
elif [ -z "$SINCE_TAG" ]; then
  echo "  (skipped: no prior tag found to compare against)"
else
  # Exact git ancestry (git log <tag>..HEAD --merges), not a gh --search
  # date filter -- date-based search is imprecise at same-day tag/PR
  # boundaries (confirmed live: v4.1.1's own tagged commit's author-date
  # collided with its own PR's merge time, so a ">=" date search
  # double-counted it as "since v4.1.1" when it's actually v4.1.1's own
  # content). PR numbers straight from the standard GitHub merge-commit
  # message format are unambiguous.
  pr_numbers=$(git log "${SINCE_TAG}..HEAD" --merges --format='%s' | grep -oE '#[0-9]+' | grep -oE '[0-9]+' || true)
  target_label=""
  if [ -n "$VERSION" ]; then
    target_label="v$(echo "$VERSION" | cut -d. -f1-2)"
  fi
  if [ -z "$pr_numbers" ]; then
    echo "  no merge commits found since $SINCE_TAG"
  else
    all_prs_json=$(gh pr list --state merged --base master --limit 300 \
      --json number,title,labels 2>/dev/null || echo "[]")
    missing_count=0
    matched_prs_json=$(python3 -c "
import json,sys
wanted = set(int(x) for x in sys.argv[1].split())
prs = json.load(sys.stdin)
matched = [p for p in prs if p['number'] in wanted]
matched.sort(key=lambda p: p['number'], reverse=True)
print(json.dumps(matched))
" "$pr_numbers" <<< "$all_prs_json")
    while IFS=$'\t' read -r num title has_label; do
      [ -z "$num" ] && continue
      if [ -z "$target_label" ]; then
        echo "  #$num $title"
      elif [ "$has_label" != "1" ]; then
        missing_count=$((missing_count + 1))
        echo "  MISSING $target_label: #$num $title"
        if [ "$AUTO_APPLY" -eq 1 ]; then
          gh api "repos/{owner}/{repo}/issues/$num/labels" -f "labels[]=$target_label" >/dev/null && echo "    -> labeled"
        # Explicitly from /dev/tty, not this loop's own stdin (redirected
        # from the process substitution below) -- reading from fd 0 here
        # would silently consume the next line of PR data instead of
        # prompting, a well-known `while read < <(...)` + nested `read -p`
        # gotcha. The `read` itself is the `if` condition (not a separate
        # `[ -r /dev/tty ]` pre-check, which passes on permission bits alone
        # even when the actual open() fails with ENXIO for "no controlling
        # terminal" -- confirmed live in a headless/non-interactive shell)
        # so its failure is caught by `else`, not `set -e`, since a
        # command's own exit status inside an `if` condition is exempt from
        # errexit by bash's own rules.
        elif { read -r -p "    Apply label '$target_label' to #$num now? [y/N] " ans < /dev/tty; } 2>/dev/null; then
          if [ "$ans" = "y" ] || [ "$ans" = "Y" ]; then
            gh api "repos/{owner}/{repo}/issues/$num/labels" -f "labels[]=$target_label" >/dev/null && echo "    -> labeled"
          fi
        else
          echo "    (no interactive terminal available -- not applying; re-run with -y to apply automatically)"
        fi
      else
        echo "  OK #$num $title"
      fi
    done < <(echo "$matched_prs_json" | python3 -c "
import json,sys
target = '$target_label'
for pr in json.load(sys.stdin):
    labels = [l['name'] for l in pr['labels']]
    has = '1' if (not target or target in labels) else '0'
    print(f\"{pr['number']}\t{pr['title']}\t{has}\")
")
    if [ -n "$target_label" ] && [ "$missing_count" -eq 0 ]; then
      echo "  all merged PRs already carry the $target_label label"
    elif [ -z "$target_label" ]; then
      echo "  (pass --version to check against a specific release label)"
    fi
    pr_count=$(echo "$pr_numbers" | wc -l)
    matched_count=$(echo "$matched_prs_json" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
    if [ "$pr_count" != "$matched_count" ]; then
      echo "  WARN: $pr_count merge commit(s) found via git log, but only $matched_count matched via" \
           "gh pr list -- increase --limit above or check for a PR outside the fetched window"
    fi
  fi
fi
echo ""

OLD_MAJOR=$(grep -m1 -oE '^#define WSPY_VERSION_MAJOR [0-9]+' wspy.h | grep -oE '[0-9]+$')
OLD_MINOR=$(grep -m1 -oE '^#define WSPY_VERSION_MINOR [0-9]+' wspy.h | grep -oE '[0-9]+$')
OLD_PATCH=$(grep -m1 -oE '^#define WSPY_VERSION_PATCH [0-9]+' wspy.h | grep -oE '[0-9]+$')
OLD_VERSION_SHORT="${OLD_MAJOR}.${OLD_MINOR}"

echo "=== Phase 3: version bump ==="
if [ -z "$VERSION" ]; then
  echo "  (skipped: no --version given; current version is ${OLD_MAJOR}.${OLD_MINOR}.${OLD_PATCH})"
else
  NEW_MAJOR=$(echo "$VERSION" | cut -d. -f1)
  NEW_MINOR=$(echo "$VERSION" | cut -d. -f2)
  NEW_PATCH=$(echo "$VERSION" | cut -d. -f3)
  sed -i.bak -E \
    -e "s/^#define WSPY_VERSION_MAJOR [0-9]+/#define WSPY_VERSION_MAJOR ${NEW_MAJOR}/" \
    -e "s/^#define WSPY_VERSION_MINOR [0-9]+/#define WSPY_VERSION_MINOR ${NEW_MINOR}/" \
    -e "s/^#define WSPY_VERSION_PATCH [0-9]+/#define WSPY_VERSION_PATCH ${NEW_PATCH}/" \
    wspy.h
  rm -f wspy.h.bak
  echo "  wspy.h: ${OLD_MAJOR}.${OLD_MINOR}.${OLD_PATCH} -> ${VERSION}"
fi
echo ""

echo "=== Phase 4: stale-version-reference grep (old: ${OLD_VERSION_SHORT}) ==="
if [ -z "$VERSION" ]; then
  echo "  (skipped: no --version given, nothing to compare against)"
else
  # Deliberately narrow: the exact "wspy_version": "X.Y" JSON-field pattern
  # doc/ARTIFACT_CONTRACT.md's manifest/run-index examples use (what broke
  # in both the real v4.0 and v4.1 releases), not a bare search for the old
  # version number as a word -- that would flood on INVESTIGATION.md's own
  # legitimate historical narrative ("4.2 shipped X", etc.), which must not
  # be touched.
  hits=$(grep -rn -E "\"wspy_version\"[[:space:]]*:[[:space:]]*\"${OLD_VERSION_SHORT}\"" \
    --include='*.md' --include='*.c' . 2>/dev/null || true)
  if [ -z "$hits" ]; then
    echo "  no \"wspy_version\": \"${OLD_VERSION_SHORT}\" examples found"
  else
    echo "  found stale wspy_version examples (update to \"$(echo "$VERSION" | cut -d. -f1-2)\"):"
    echo "$hits" | sed 's/^/    /'
  fi
  echo "  Also worth a manual look: any hardcoded test assertion or doc prose"
  echo "  quoting \"${OLD_VERSION_SHORT}\" as *this* release's version specifically"
  echo "  (not a historical reference to when a feature shipped)."
fi
echo ""

echo "=== Phase 5: full test matrix (./run_tests.sh) ==="
if [ "$SKIP_TESTS" -eq 1 ]; then
  echo "  (skipped: --skip-tests -- do not use this for an actual release)"
elif ! ./run_tests.sh; then
  echo "FAIL: run_tests.sh failed -- fix before continuing release prep"
  exit 1
fi
echo ""

echo "=== Phase 6: release-notes draft ==="
if [ "$HAVE_GH" -eq 0 ] || [ -z "$SINCE_TAG" ]; then
  echo "  (skipped: needs gh + a prior tag)"
else
  notes_file=$(mktemp /tmp/wspy-release-notes.XXXXXX.md)
  {
    echo "# wspy ${DISPLAY_VERSION:-<version>}"
    echo ""
    echo "<!-- DRAFT: group these thematically with prose framing before publishing, -->"
    echo "<!-- mirroring past release bodies (see \`gh release view v4.2\`) -- this is -->"
    echo "<!-- a flat chronological skeleton, not a finished body. -->"
    echo ""
    echo "$matched_prs_json" | python3 -c "
import json,sys
for pr in json.load(sys.stdin):
    print(f\"- #{pr['number']} {pr['title']}\")
"
  } > "$notes_file"
  echo "  draft written to $notes_file ($(wc -l < "$notes_file") lines) -- edit before use"
fi
echo ""

echo "=== Phase 7: doc bookkeeping reminders (manual -- real editorial judgment, not automated) ==="
echo "  - Fold the rolling \"Shipped since 4.2\"-style section in INVESTIGATION.md into a"
echo "    proper \"What shipped in ${DISPLAY_VERSION:-<X.Y>}\" pointer-list section (its own stated"
echo "    intent once this phase's backlog is empty -- keep it to names/pointers, not a"
echo "    feature log; CLAUDE.md documents mechanism, this doc shouldn't restate it)."
echo "  - Prune: move any multi-paragraph design write-up or validation narrative still"
echo "    sitting inline in INVESTIGATION.md (rather than already in doc/INVESTIGATION_ARCHIVE.md)"
echo "    for a now-fully-shipped item -- the open backlog should only ever contain open work."
echo "  - Update INVESTIGATION.md's top \"Status (<date>): ...\" line."
echo "  - Check README.md for currency beyond the tool-list check"
echo "    tests/doc_version_check.sh already automates -- usage examples, version-specific"
echo "    prose, newly-relevant flags/workflows worth a mention."
echo "  - Retrospective: did this release's checklist miss anything real (like PR #124's"
echo "    missing label did, the first time this script ran)? If so, update"
echo "    scripts/release_prep.sh (and its CLAUDE.md entry) now, while it's fresh --"
echo "    this tool is meant to get better release over release, not stay static."
echo ""

echo "=== Phase 8: tag/publish -- run these yourself when ready, this script never does ==="
tag="v${DISPLAY_VERSION:-X.Y[.Z]}"
echo "  git tag -a $tag -m \"wspy ${DISPLAY_VERSION:-X.Y[.Z]}"
echo ""
echo "  <one-paragraph summary of what shipped>\""
echo ""
echo "  git push origin $tag"
echo ""
echo "  gh release create $tag --title \"wspy ${DISPLAY_VERSION:-X.Y[.Z]}\" --notes-file <edited-release-notes.md>"
