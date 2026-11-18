#!/usr/bin/env bash
# Record the main video stream: ISP crop 4224x3136 -> 4224x2376 (16:9),
# scale -> 3840x2160 (4K UHD), H.264 by the VPU.
# Usage: ./rec_4k.sh    Stop with Ctrl+C.
# Optional: BPS=30000000 OUT=file.mp4 ./rec_4k.sh (default 20 Mbit/s)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=rec_common.sh
source "${SCRIPT_DIR}/rec_common.sh"

readonly NODE=/dev/video11
readonly BPS="${BPS:-20000000}"
readonly OUT="${OUT:-/tmp/cam_4k.mp4}"

main() {
  # Ctrl+C must reach gst-launch, which then finishes the file (-e).
  # The script ignores it and goes on to the cleanup.
  trap ':' INT
  if ! crop_16_9 "${NODE}"; then
    exit 1
  fi
  if ! set_nv12_size "${NODE}" 3840 2160; then
    restore_full_frame "${NODE}"
    exit 1
  fi
  echo "Recording 3840x2160 @30 to ${OUT} (Ctrl+C to stop)"
  gst-launch-1.0 -e v4l2src device="${NODE}" \
    ! video/x-raw,format=NV12,width=3840,height=2160,framerate=30/1 \
    ! mpph264enc bps="${BPS}" ! h264parse ! mp4mux \
    ! filesink location="${OUT}" >/dev/null 2>&1
  restore_full_frame "${NODE}"
  report_file "${OUT}"
}

main "$@"
