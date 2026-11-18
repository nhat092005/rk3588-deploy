#!/usr/bin/env bash
#
# Start the PC <-> board link needed by tier 1 (eval_perf, eval_memory) and
# NPU accuracy from the PC.
#
# Needs root, and must be re-run after every reboot. Run on the board:
#   sudo bash deploy/sh/env/start_rknn_server.sh

# No -e: a failing pkill (nothing to kill) must not stop the script.
set -uo pipefail

readonly ADB_PORT=5555
readonly ADB_LOG=/tmp/adbd.log
readonly RKNN_LOG=/tmp/rknn_server.log

#######################################
# Wait until no process with this exact name is left (max 5 s).
# Arguments:
#   Process name.
#######################################
wait_gone() {
  local _
  for _ in $(seq 50); do
    if ! pgrep -x "$1" > /dev/null; then
      return 0
    fi
    sleep 0.1
  done
  echo "WARNING: $1 still running after 5 s"
}

#######################################
# Print whether adbd listens on ADB_PORT, with its log when it does not.
#######################################
report_adbd() {
  local _
  for _ in $(seq 100); do
    if ss -ltn | grep -q ":${ADB_PORT} "; then
      break
    fi
    sleep 0.1
  done
  if ss -ltn | grep -q ":${ADB_PORT} "; then
    echo "adbd listening on ${ADB_PORT}"
  else
    echo "adbd NOT listening on ${ADB_PORT}"
    echo "adbd process: $(pgrep -a adbd || echo none)"
    echo "--- ${ADB_LOG}"
    tail -20 "${ADB_LOG}"
  fi
}

main() {
  # adbd must be fully gone before the new one opens /dev/usb-ffs/adb/ep0.
  pkill -x adbd
  wait_gone adbd
  ADB_TCP_PORT="${ADB_PORT}" setsid /usr/bin/adbd \
    > "${ADB_LOG}" 2>&1 < /dev/null &

  pkill -x rknn_server
  wait_gone rknn_server
  setsid /usr/bin/rknn_server > "${RKNN_LOG}" 2>&1 < /dev/null &

  report_adbd
  pgrep -a rknn_server || {
    echo "rknn_server NOT running"
    tail -20 "${RKNN_LOG}"
  }
}

main "$@"
