#!/usr/bin/env bash
#
# Run the MediaMTX server that the live stream of rk3588_deploy is published to.
# Start it before `rk3588_deploy --stream true`, and again after every reboot.
# Stop it with Ctrl-C. Install it first: deploy/sh/env/install_mediamtx.sh.
#
# Run on the board:
#   bash deploy/sh/stream/run_mediamtx.sh
# Watch from the PC:
#   ffplay -fflags nobuffer -flags low_delay -rtsp_transport tcp rtsp://<board>:8554/live
#
# Environment: MEDIAMTX_DIR, the install folder (default ~/rk3588-board/mediamtx).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly SCRIPT_DIR
readonly MEDIAMTX_DIR="${MEDIAMTX_DIR:-${HOME}/rk3588-board/mediamtx}"

main() {
  if [[ ! -x "${MEDIAMTX_DIR}/mediamtx" ]]; then
    echo "mediamtx is not installed in ${MEDIAMTX_DIR}: run deploy/sh/env/install_mediamtx.sh" >&2
    exit 1
  fi
  exec "${MEDIAMTX_DIR}/mediamtx" "${SCRIPT_DIR}/mediamtx.yml"
}

main "$@"
