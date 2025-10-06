#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME=$(basename "$0")
REQ_PERF=-1    # recommended permissive value so unprivileged users can use perf
REQ_NMI=0

usage(){
  cat <<EOF
Usage: $SCRIPT_NAME [-y]

Checks current kernel settings for nmi_watchdog and perf_event_paranoid and
asks to update them if necessary. Use -y to apply changes without prompting.

Notes:
- Changes via sysctl/sysfs are immediate but not persistent across reboots.
  To persist, add settings to /etc/sysctl.conf or a file under /etc/sysctl.d/.
- This script will use sudo if not run as root.
EOF
}

AUTO_APPLY=0
if [ "${1-}" = "-y" ]; then
  AUTO_APPLY=1
fi

need_cmd(){
  command -v "$1" >/dev/null 2>&1 || { echo "$SCRIPT_NAME: required command '$1' not found" >&2; exit 2; }
}

need_cmd sysctl || true

read_trim(){
  tr -d ' \t\n\r' <"$1" 2>/dev/null || true
}

set_value(){
  local key="$1" val="$2"
  if [ "$(id -u)" -eq 0 ]; then
    sysctl -w "$key=$val" >/dev/null
  else
    if command -v sudo >/dev/null 2>&1; then
      echo "Requesting sudo to set $key=$val"
      sudo sysctl -w "$key=$val" >/dev/null
    else
      echo "Cannot set $key, run as root or install sudo" >&2
      return 1
    fi
  fi
}

apply_if_needed(){
  local name="$1" path="$2" key="$3" desired="$4"
  if [ ! -e "$path" ]; then
    echo "$name: $path not found, skipping"
    return 0
  fi
  cur=$(read_trim "$path" || echo "")
  if [ -z "$cur" ]; then
    echo "$name: unable to read current value from $path, skipping"
    return 1
  fi
  echo "$name: current=$cur desired=$desired"
  if [ "$name" = "perf_event_paranoid" ]; then
    # numeric comparison
    if (( cur > desired )); then
      echo "$name: needs update"
    else
      echo "$name: OK"
      return 0
    fi
  else
    if [ "$cur" != "$desired" ]; then
      echo "$name: needs update"
    else
      echo "$name: OK"
      return 0
    fi
  fi

  if [ "$AUTO_APPLY" -eq 0 ]; then
    read -r -p "Apply change to set $key=$desired now? [y/N] " ans || ans="n"
    case "$ans" in
      [Yy]* ) ;;
      * ) echo "Skipped updating $name"; return 0;;
    esac
  else
    echo "Auto-applying change: $key=$desired"
  fi

  if set_value "$key" "$desired"; then
    echo "$name: updated to $desired"
  else
    echo "$name: failed to update $desired" >&2
    return 1
  fi
}

main(){
  # nmi_watchdog
  apply_if_needed "nmi_watchdog" "/proc/sys/kernel/nmi_watchdog" "kernel.nmi_watchdog" "$REQ_NMI"

  # perf_event_paranoid
  apply_if_needed "perf_event_paranoid" "/proc/sys/kernel/perf_event_paranoid" "kernel.perf_event_paranoid" "$REQ_PERF"

  echo "Done. To make changes persistent across reboots, add the following to /etc/sysctl.d/99-performance.conf or /etc/sysctl.conf:"
  echo "kernel.nmi_watchdog=$REQ_NMI"
  echo "kernel.perf_event_paranoid=$REQ_PERF"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  if [ "${1-}" = "-h" ] || [ "${1-}" = "--help" ]; then
    usage
    exit 0
  fi
  main
fi
