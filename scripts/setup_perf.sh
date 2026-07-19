#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME=$(basename "$0")
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REQ_NMI=0
REQ_PERF=1     # documented minimum for wspy to work unprivileged (see README/CLAUDE.md)
REQ_PERF_PERMISSIVE=-1
# --power's RAPL/energy-pkg access needs root or CAP_PERFMON specifically --
# stricter than perf_event_paranoid alone covers (confirmed live: --ibs-basic
# opens fine at the same paranoid level that denies --power). Repo-root
# wspy binary, relative to this script's own location so it doesn't matter
# what directory this is invoked from. Overridable via --wspy-binary for a
# build living elsewhere.
WSPY_BIN="$(cd "${SCRIPT_DIR}/.." && pwd)/wspy"
DESIRED_CAP="cap_perfmon+ep"
DO_SETCAP=1

usage(){
  cat <<EOF
Usage: $SCRIPT_NAME [-y] [-p|--permissive] [--wspy-binary <path>] [--no-setcap]

Checks current kernel settings for nmi_watchdog and perf_event_paranoid and
asks to update them if necessary, then checks/grants the wspy binary
CAP_PERFMON (needed for --power specifically -- see CLAUDE.md's power.c
entry) so it works without sudo.

Options:
  -y                  Apply changes without prompting.
  -p, --permissive    Set perf_event_paranoid to $REQ_PERF_PERMISSIVE (no restrictions) instead of
                      the default $REQ_PERF. This is more than wspy needs and lets any
                      unprivileged user on the system monitor other processes'
                      perf events, so only use it if you understand that tradeoff.
  --wspy-binary <path> Path to the wspy binary to grant CAP_PERFMON to
                      (default: $WSPY_BIN).
  --no-setcap         Skip the CAP_PERFMON check/grant entirely -- only touch
                      the sysctls, matching this script's pre-existing scope.
  -h, --help          Show this help.

Notes:
- Changes via sysctl/sysfs are immediate but not persistent across reboots.
  To persist, add settings to /etc/sysctl.conf or a file under /etc/sysctl.d/.
- CAP_PERFMON is a file capability attached to the wspy binary's own inode --
  rebuilding wspy (make) replaces that file and drops the grant. Re-run this
  script (or just \`sudo setcap $DESIRED_CAP <path to wspy>\`) after every
  rebuild if --power access without sudo matters to you.
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

check_setcap(){
  if [ "$DO_SETCAP" -eq 0 ]; then
    return 0
  fi
  if ! command -v getcap >/dev/null 2>&1 || ! command -v setcap >/dev/null 2>&1; then
    echo "CAP_PERFMON (--power): getcap/setcap not found (install libcap2-bin on Debian/Ubuntu, libcap on Fedora/RHEL) -- skipping"
    return 0
  fi
  if [ ! -e "$WSPY_BIN" ]; then
    echo "CAP_PERFMON (--power): $WSPY_BIN not found -- run 'make' first, then re-run this script if --power access without sudo matters to you"
    return 0
  fi

  # getcap's output format has changed across libcap versions ("path =
  # cap_perfmon+ep" vs. "path cap_perfmon=ep"), so match on the capability
  # name rather than depend on either exact separator/operator spelling.
  # getcap exits 0 and prints nothing for a file with no capabilities at
  # all (not an error), so "none" has to come from an empty-output check,
  # not a failed-command fallback.
  local cur_cap
  cur_cap=$(getcap "$WSPY_BIN" 2>/dev/null || true)
  if echo "$cur_cap" | grep -q 'cap_perfmon'; then
    echo "CAP_PERFMON (--power): $WSPY_BIN already has it -- OK"
    return 0
  fi
  echo "CAP_PERFMON (--power): $WSPY_BIN does not have it (current: ${cur_cap:-none})"

  if [ "$AUTO_APPLY" -eq 0 ]; then
    read -r -p "Grant $DESIRED_CAP to $WSPY_BIN now? [y/N] " ans || ans="n"
    case "$ans" in
      [Yy]* ) ;;
      * ) echo "CAP_PERFMON: Skipped"; return 0;;
    esac
  else
    echo "CAP_PERFMON: Auto-applying"
  fi

  local setcap_cmd=(setcap "$DESIRED_CAP" "$WSPY_BIN")
  if [ "$(id -u)" -eq 0 ]; then
    "${setcap_cmd[@]}"
  elif command -v sudo >/dev/null 2>&1; then
    echo "Requesting sudo to run: ${setcap_cmd[*]}"
    sudo "${setcap_cmd[@]}"
  else
    echo "CAP_PERFMON: cannot set, run as root or install sudo" >&2
    return 1
  fi

  if getcap "$WSPY_BIN" 2>/dev/null | grep -q 'cap_perfmon'; then
    echo "CAP_PERFMON: granted -- $WSPY_BIN can now use --power without sudo"
  else
    echo "CAP_PERFMON: setcap did not report an error but the capability isn't visible afterward -- verify manually with 'getcap $WSPY_BIN'" >&2
    return 1
  fi
}

main(){
  local status=0

  apply_if_needed "nmi_watchdog" "/proc/sys/kernel/nmi_watchdog" "kernel.nmi_watchdog" "$REQ_NMI" || status=1

  apply_if_needed "perf_event_paranoid" "/proc/sys/kernel/perf_event_paranoid" "kernel.perf_event_paranoid" "$REQ_PERF" || status=1

  check_setcap || status=1

  echo "Done. To make changes persistent across reboots, add the following to /etc/sysctl.d/99-performance.conf or /etc/sysctl.conf:"
  echo "kernel.nmi_watchdog=$REQ_NMI"
  echo "kernel.perf_event_paranoid=$REQ_PERF"
  if [ "$DO_SETCAP" -eq 1 ]; then
    echo "Remember: CAP_PERFMON on $WSPY_BIN must be re-granted after every 'make' rebuild."
  fi

  return "$status"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  AUTO_APPLY=0
  while [ $# -gt 0 ]; do
    case "$1" in
      -y) AUTO_APPLY=1; shift ;;
      -p|--permissive) REQ_PERF="$REQ_PERF_PERMISSIVE"; shift ;;
      --wspy-binary) WSPY_BIN="$2"; shift 2 ;;
      --no-setcap) DO_SETCAP=0; shift ;;
      -h|--help) usage; exit 0 ;;
      *) echo "$SCRIPT_NAME: unknown option '$1'" >&2; usage >&2; exit 2 ;;
    esac
  done
  main
fi
