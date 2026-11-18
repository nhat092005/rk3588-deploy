#!/usr/bin/env bash
#
# Let the video group open /dev/dma_heap/reserved (the CMA heap below 4 GB).
#
# One-time board setup, needs root: the rule goes to /etc/udev/rules.d.
# Run on the board: sudo bash <board root>/deploy/sh/env/install_udev_rules.sh

set -euo pipefail

readonly RULE_NAME="99-rk3588-deploy.rules"
readonly RULE_DIR="/etc/udev/rules.d"

main() {
  local here
  here="$(cd "$(dirname "$0")" && pwd)"
  if [[ "${EUID}" -ne 0 ]]; then
    echo "error: run as root: sudo bash $0" >&2
    exit 1
  fi
  install -m 644 "${here}/${RULE_NAME}" "${RULE_DIR}/${RULE_NAME}"
  udevadm control --reload-rules
  udevadm trigger --subsystem-match=dma_heap
  udevadm settle
  ls -l /dev/dma_heap/reserved
}

main "$@"
