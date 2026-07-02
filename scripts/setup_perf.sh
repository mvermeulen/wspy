#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME=$(basename "$0")
REQ_NMI=0
REQ_PERF=1     # documented minimum for wspy to work unprivileged (see README/CLAUDE.md)
REQ_PERF_PERMISSIVE=-1

usage(){
  cat <<EOF
Usage: $SCRIPT_NAME [-y] [-p|--permissive]

Checks current kernel settings for nmi_watchdog and perf_event_paranoid and
asks to update them if necessary.

Options:
  -y               Apply changes without prompting.
  -p, --permissive Set perf_event_paranoid to $REQ_PERF_PERMISSIVE (no restrictions) instead of
                    the default $REQ_PERF. This is more than wspy needs and lets any
                    unprivileged user on the system monitor other processes'
                    perf events, so only use it if you understand that tradeoff.
  -h, --help       Show this help.

Notes:
- Changes via sysctl/sysfs are immediate but not persistent across reboots.
  To persist, add settings to /etc/sysctl.conf or a file under /etc/sysctl.d/.
- This script will use sudo if not run as root.
EOF
}

read_trim(){
  tr -d ' \t\n\r' <"$1" 2>/dev/null || true
}

set_value(){
  local key="$1" val="$2" path="$3"
  if command -v sysctl >/dev/null 2>&1; then
    if [ "$(id -u)" -eq 0 ]; then
      sysctl -w "$key=$val" >/dev/null
    elif command -v sudo >/dev/null 2>&1; then
      echo "Requesting sudo to set $key=$val"
      sudo sysctl -w "$key=$val" >/dev/null
    else
      echo "Cannot set $key, run as root or install sudo" >&2
      return 1
    fi
  else
    # sysctl binary not available; fall back to writing the /proc/sys file directly
    if [ "$(id -u)" -eq 0 ]; then
      echo "$val" > "$path"
    elif command -v sudo >/dev/null 2>&1; then
      echo "Requesting sudo to set $path=$val"
      echo "$val" | sudo tee "$path" >/dev/null
    else
      echo "Cannot set $path, run as root or install sudo" >&2
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
  local cur
  cur=$(read_trim "$path" || echo "")
  if [ -z "$cur" ]; then
    echo "$name: unable to read current value from $path, skipping"
    return 1
  fi
  echo "$name: current=$cur desired=$desired"
  if [ "$name" = "perf_event_paranoid" ]; then
    if ! [[ "$cur" =~ ^-?[0-9]+$ ]]; then
      echo "$name: current value '$cur' is not numeric, skipping" >&2
      return 1
    fi
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

  if set_value "$key" "$desired" "$path"; then
    echo "$name: updated to $desired"
  else
    echo "$name: failed to update $desired" >&2
    return 1
  fi
}

main(){
  local status=0

  apply_if_needed "nmi_watchdog" "/proc/sys/kernel/nmi_watchdog" "kernel.nmi_watchdog" "$REQ_NMI" || status=1

  apply_if_needed "perf_event_paranoid" "/proc/sys/kernel/perf_event_paranoid" "kernel.perf_event_paranoid" "$REQ_PERF" || status=1

  echo "Done. To make changes persistent across reboots, add the following to /etc/sysctl.d/99-performance.conf or /etc/sysctl.conf:"
  echo "kernel.nmi_watchdog=$REQ_NMI"
  echo "kernel.perf_event_paranoid=$REQ_PERF"

  return "$status"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  AUTO_APPLY=0
  for arg in "$@"; do
    case "$arg" in
      -y) AUTO_APPLY=1 ;;
      -p|--permissive) REQ_PERF="$REQ_PERF_PERMISSIVE" ;;
      -h|--help) usage; exit 0 ;;
      *) echo "$SCRIPT_NAME: unknown option '$arg'" >&2; usage >&2; exit 2 ;;
    esac
  done
  main
fi
