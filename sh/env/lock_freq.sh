#!/usr/bin/env bash
#
# Fix CPU / NPU / DDR frequency before board measurements, or restore it.
#
# Needs root. Run on the board:
#   sudo bash deploy/sh/env/lock_freq.sh lock | unlock | status
#
# Installed copy for passwordless sudo: /usr/local/sbin/lock_freq.sh.
# Its code (not comments) must equal this file; `make check-board` of the
# meta-repo compares them.
#
# RKNPU2 User Guide (p.72-73) uses "userspace"; this board's "performance"
# pins every clock at its max without the debugfs path, so that is used
# here. Original governors: CPU ondemand, NPU rknpu_ondemand, DDR
# dmc_ondemand.

set -euo pipefail

readonly CPU=/sys/devices/system/cpu/cpufreq
readonly NPU=/sys/class/devfreq/fdab0000.npu
readonly DDR=/sys/class/devfreq/dmc

#######################################
# Print governor and current frequency of every CPU policy, NPU and DDR.
#######################################
status() {
  local policy
  for policy in "${CPU}"/policy*; do
    echo "$(basename "${policy}"):" \
      "$(cat "${policy}/scaling_governor")" \
      "$(cat "${policy}/scaling_cur_freq") kHz"
  done
  echo "npu: $(cat "${NPU}/governor") $(cat "${NPU}/cur_freq") Hz"
  echo "ddr: $(cat "${DDR}/governor") $(cat "${DDR}/cur_freq") Hz"
}

#######################################
# Set the governor of every CPU policy, the NPU and the DDR, then print
# the status.
# Arguments:
#   Governor for the CPU policies.
#   Governor for the NPU.
#   Governor for the DDR.
#######################################
set_governors() {
  local policy
  for policy in "${CPU}"/policy*; do
    echo "$1" > "${policy}/scaling_governor"
  done
  echo "$2" > "${NPU}/governor"
  echo "$3" > "${DDR}/governor"
  status
}

main() {
  case "${1:-status}" in
    lock) set_governors performance performance performance ;;
    unlock) set_governors ondemand rknpu_ondemand dmc_ondemand ;;
    status) status ;;
    *)
      echo "usage: $0 lock|unlock|status"
      exit 1
      ;;
  esac
}

main "$@"
