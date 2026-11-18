#!/usr/bin/env bash
# Record the AI input stream: ISP crop 4224x3136 -> 4224x2376 (16:9),
# scale -> 640x360 (the stream that feeds the AI model), H.264 by the VPU.
# Usage: ./rec_640x360.sh    Stop with Ctrl+C.
# Optional: BPS=16000000 OUT=file.mp4 ./rec_640x360.sh (default 8 Mbit/s)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=rec_common.sh
source "${SCRIPT_DIR}/rec_common.sh"

readonly NODE=/dev/video12
readonly BPS="${BPS:-8000000}"
readonly OUT="${OUT:-/tmp/cam_640x360.mp4}"

main() {
  # Ctrl+C must reach both ends of the pipe, so the file is finished (-e).
  # The script ignores it and goes on to the cleanup.
  trap ':' INT
  if ! crop_16_9 "${NODE}"; then
    exit 1
  fi
  if ! set_nv12_size "${NODE}" 640 360; then
    restore_full_frame "${NODE}"
    exit 1
  fi
  echo "Recording 640x360 @30 to ${OUT} (Ctrl+C to stop)"
  # v4l2-ctl captures and keeps the crop above. gst only encodes: the
  # format that gst's v4l2src sets makes the driver reset the crop to
  # 640x360 at (0,0).
  stdbuf -o0 v4l2-ctl -d "${NODE}" --stream-mmap=4 --stream-to=- 2>/dev/null \
    | gst-launch-1.0 -e fdsrc fd=0 \
      ! rawvideoparse format=nv12 width=640 height=360 framerate=30/1 \
      ! mpph264enc bps="${BPS}" ! h264parse ! mp4mux \
      ! filesink location="${OUT}" >/dev/null 2>&1
  restore_full_frame "${NODE}"
  report_file "${OUT}"
}

main "$@"
