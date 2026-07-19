#!/usr/bin/env bash
set -euo pipefail

# One-time registration helper for scripts/pts_hooks/{pre,post}_test_run.sh
# against Phoronix Test Suite's built-in result_notifier module -- see
# doc/phoronix_hook_investigation.md for the full mechanism and
# INVESTIGATION.md's 4.3 Tier 6 item 20. Mirrors setup_perf.sh's own
# check-then-prompt shape (report current vs. desired, apply only on
# confirmation or -y), not a silent auto-fixer -- this touches real
# per-user Phoronix Test Suite state (~/.phoronix-test-suite), not just a
# kernel sysctl, so the default stays report-only.
#
# Registration has two independent pieces PTS itself keeps in two
# different files:
#   1. modules-data/result_notifier/module-settings.ini -- which
#      executable runs for each result_notifier hook point (a plain
#      key=value ini file PTS's own pts_module::module_config_save()
#      already merges non-destructively; this is the same file
#      `phoronix-test-suite module-setup result_notifier` would write).
#      The directory MUST be the underscored "result_notifier", matching
#      the module's literal PHP class name (pts_module::read_option()
#      resolves the directory from pts_module_manager's current-module
#      string, which is exactly that class name) -- not a hyphenated
#      "result-notifier". A hyphenated directory is silently never read,
#      so registration appears to succeed but the hooks never actually
#      fire (verified against the installed pts-core/objects/client/
#      pts_module.php).
#   2. user-config.xml's <AutoLoadModules> list -- whether result_notifier
#      loads automatically on every `phoronix-test-suite` invocation at
#      all (without this, the hooks above are configured but never fire).
#
# Note separately: even with this fixed, phoronix-test-suite's own bundled
# result_notifier.php (pts-core/modules/) has had a fatal-error bug of its
# own whenever a real hook script is configured, reported/fixed upstream at
# https://github.com/phoronix-test-suite/phoronix-test-suite/issues/925 and
# https://github.com/phoronix-test-suite/phoronix-test-suite/pull/924 -- a
# host without that upstream fix (or an equivalent local patch to
# /usr/share/phoronix-test-suite/pts-core/modules/result_notifier.php) will
# see phoronix-test-suite itself crash as soon as the hooks below are
# actually registered and fire, not just silently fail to run them.

SCRIPT_NAME=$(basename "$0")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PRE_HOOK="${SCRIPT_DIR}/pts_hooks/pre_test_run.sh"
POST_HOOK="${SCRIPT_DIR}/pts_hooks/post_test_run.sh"

PTS_USER_PATH="${PTS_USER_PATH:-$HOME/.phoronix-test-suite}"
USER_CONFIG="${PTS_USER_PATH}/user-config.xml"
MODULE_SETTINGS_DIR="${PTS_USER_PATH}/modules-data/result_notifier"
MODULE_SETTINGS="${MODULE_SETTINGS_DIR}/module-settings.ini"

usage(){
  cat <<EOF
Usage: $SCRIPT_NAME [-y]

Checks whether scripts/pts_hooks/{pre,post}_test_run.sh are registered with
Phoronix Test Suite's result_notifier module, and offers to register them
if not.

Options:
  -y          Apply changes without prompting.
  -h, --help  Show this help.

Notes:
- This edits files under \$PTS_USER_PATH (default ~/.phoronix-test-suite),
  not this repository. Safe to re-run; each check is independent and
  idempotent.
- After running, a fresh phoronix-test-suite invocation should print
  "The result_notifier module for providing external hooks has been
  loaded." -- confirms the AutoLoadModules registration took effect.
EOF
}

read_ini_value(){
  # Plain PHP parse_ini_file()-shaped file: "key=value" lines, no quoting
  # in play here since our own values are always absolute paths.
  local key="$1"
  [ -f "$MODULE_SETTINGS" ] || return 0
  awk -F= -v k="$key" '$1==k{print substr($0, index($0,"=")+1); exit}' "$MODULE_SETTINGS"
}

check_module_settings(){
  local cur_pre cur_post
  cur_pre=$(read_ini_value pre_test_run_process || true)
  cur_post=$(read_ini_value post_test_run_process || true)

  echo "result_notifier module-settings.ini: $MODULE_SETTINGS"
  echo "  pre_test_run_process:  current='${cur_pre:-<unset>}' desired='$PRE_HOOK'"
  echo "  post_test_run_process: current='${cur_post:-<unset>}' desired='$POST_HOOK'"

  if [ "$cur_pre" = "$PRE_HOOK" ] && [ "$cur_post" = "$POST_HOOK" ]; then
    echo "  OK"
    return 0
  fi

  if [ "$AUTO_APPLY" -eq 0 ]; then
    read -r -p "Write $MODULE_SETTINGS now? [y/N] " ans || ans="n"
    case "$ans" in
      [Yy]* ) ;;
      * ) echo "  Skipped"; return 0;;
    esac
  else
    echo "  Auto-applying"
  fi

  mkdir -p "$MODULE_SETTINGS_DIR"
  # Preserve every other existing key (pre_test_process/interim_test_run_process/
  # post_test_process), only overwrite the two this script owns -- same
  # merge-not-clobber intent as PTS's own module_config_save().
  {
    if [ -f "$MODULE_SETTINGS" ]; then
      grep -Ev '^(pre_test_run_process|post_test_run_process)=' "$MODULE_SETTINGS" || true
    fi
    echo "pre_test_run_process=$PRE_HOOK"
    echo "post_test_run_process=$POST_HOOK"
  } > "${MODULE_SETTINGS}.new"
  mv "${MODULE_SETTINGS}.new" "$MODULE_SETTINGS"
  echo "  Updated"
}

check_autoload(){
  echo "user-config.xml <AutoLoadModules>: $USER_CONFIG"
  if [ ! -f "$USER_CONFIG" ]; then
    echo "  $USER_CONFIG not found -- run phoronix-test-suite at least once first, skipping"
    return 0
  fi

  if grep -q '<AutoLoadModules>[^<]*\bresult_notifier\b' "$USER_CONFIG"; then
    echo "  OK (result_notifier already present)"
    return 0
  fi

  local current_line
  current_line=$(grep -o '<AutoLoadModules>[^<]*</AutoLoadModules>' "$USER_CONFIG" || true)
  if [ -z "$current_line" ]; then
    echo "  no <AutoLoadModules> element found -- edit $USER_CONFIG by hand:"
    echo "    add a <Modules><AutoLoadModules>result_notifier</AutoLoadModules></Modules> block"
    return 0
  fi
  echo "  current: $current_line"

  if [ "$AUTO_APPLY" -eq 0 ]; then
    read -r -p "Append result_notifier to <AutoLoadModules> in $USER_CONFIG now? [y/N] " ans || ans="n"
    case "$ans" in
      [Yy]* ) ;;
      * ) echo "  Skipped"; return 0;;
    esac
  else
    echo "  Auto-applying"
  fi

  local new_line="${current_line/<\/AutoLoadModules>/, result_notifier<\/AutoLoadModules>}"
  cp "$USER_CONFIG" "${USER_CONFIG}.bak-$(date +%s)"
  sed -i "s#${current_line}#${new_line}#" "$USER_CONFIG"
  echo "  Updated (backup written alongside user-config.xml)"
}

main(){
  if [ ! -x "$PRE_HOOK" ] || [ ! -x "$POST_HOOK" ]; then
    echo "$SCRIPT_NAME: expected hook scripts not found/executable under $SCRIPT_DIR/pts_hooks/" >&2
    exit 1
  fi
  check_module_settings
  echo
  check_autoload
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  AUTO_APPLY=0
  for arg in "$@"; do
    case "$arg" in
      -y) AUTO_APPLY=1 ;;
      -h|--help) usage; exit 0 ;;
      *) echo "$SCRIPT_NAME: unknown option '$arg'" >&2; usage >&2; exit 2 ;;
    esac
  done
  main
fi
