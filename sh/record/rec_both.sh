#!/usr/bin/env bash
# Record BOTH ISP streams at the same time, same scene, to compare them:
#   /dev/video11 mainpath: crop 16:9 -> 3840x2160 (4K UHD) -> OUT_4K
#   /dev/video12 selfpath: crop 16:9 -> 640x360 (AI input) -> OUT_360
# Usage: ./rec_both.sh    Stop with ONE Ctrl+C and wait for "Crop restored".
# Optional: BPS_4K=30000000 BPS_360=16000000 OUT_4K=... OUT_360=...
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=rec_common.sh
source "${SCRIPT_DIR}/rec_common.sh"

readonly MAIN_NODE=/dev/video11
readonly SELF_NODE=/dev/video12
readonly BPS_4K="${BPS_4K:-20000000}"
readonly BPS_360="${BPS_360:-8000000}"
readonly OUT_4K="${OUT_4K:-/tmp/cam_4k.mp4}"
readonly OUT_360="${OUT_360:-/tmp/cam_640x360.mp4}"

fifo=""
gst_main_pid=""
v4l2_self_pid=""
gst_self_pid=""

# Send gst ONE SIGINT, so it writes the MP4 index. A second SIGINT makes
# gst quit before that ("moov atom not found"). Both gst and v4l2-ctl run
# in their own session (setsid), so the terminal's Ctrl+C reaches only
# this script.
stop_recording() {
  kill -INT "${gst_main_pid}" 2>/dev/null
  kill "${v4l2_self_pid}" 2>/dev/null
}

cleanup() {
  restore_full_frame "${MAIN_NODE}" "${SELF_NODE}"
  if [[ -n "${fifo}" ]]; then
    rm -f "${fifo}"
    rmdir "$(dirname "${fifo}")"
  fi
}

# VIDIOC_S_FMT is refused (EBUSY) on one node while the other one streams,
# so both nodes are configured before either one starts.
configure_nodes() {
  local node
  for node in "${MAIN_NODE}" "${SELF_NODE}"; do
    crop_16_9 "${node}" || return 1
  done
  set_nv12_size "${MAIN_NODE}" 3840 2160 || return 1
  set_nv12_size "${SELF_NODE}" 640 360 || return 1
}

main() {
  trap stop_recording INT TERM
  if ! configure_nodes; then
    restore_full_frame "${MAIN_NODE}" "${SELF_NODE}"
    exit 1
  fi
  local fifo_dir
  fifo_dir="$(mktemp -d)"
  fifo="${fifo_dir}/video360.fifo"
  mkfifo "${fifo}"

  # 4K: gst reads the mainpath directly (a pipe at 373 MB/s dropped
  # frames). It starts first: its own S_FMT must come before the
  # selfpath streams.
  setsid gst-launch-1.0 -e v4l2src device="${MAIN_NODE}" \
    ! video/x-raw,format=NV12,width=3840,height=2160,framerate=30/1 \
    ! mpph264enc bps="${BPS_4K}" ! h264parse ! mp4mux \
    ! filesink location="${OUT_4K}" >/dev/null 2>&1 &
  gst_main_pid=$!
  sleep 2

  # 640x360: v4l2-ctl captures (gst's v4l2src would reset this node's
  # crop) and the VPU encodes through a FIFO.
  setsid gst-launch-1.0 -e filesrc location="${fifo}" \
    ! rawvideoparse format=nv12 width=640 height=360 framerate=30/1 \
    ! mpph264enc bps="${BPS_360}" ! h264parse ! mp4mux \
    ! filesink location="${OUT_360}" >/dev/null 2>&1 &
  gst_self_pid=$!
  echo "Recording 4K -> ${OUT_4K} and 640x360 -> ${OUT_360}" \
    "(one Ctrl+C to stop)"
  setsid stdbuf -o0 v4l2-ctl -d "${SELF_NODE}" --stream-mmap=4 \
    --stream-to="${fifo}" >/dev/null 2>&1 &
  v4l2_self_pid=$!

  wait "${gst_main_pid}"
  wait "${v4l2_self_pid}" "${gst_self_pid}"
  cleanup
  local status=0
  report_file "${OUT_4K}" || status=1
  report_file "${OUT_360}" || status=1
  return "${status}"
}

main "$@"
