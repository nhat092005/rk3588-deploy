#!/usr/bin/env bash
# Shared by rec_4k.sh, rec_640x360.sh and rec_both.sh: ISP crop, stream
# format and the summary of a recorded file. Source it, do not run it.
#
# The sensor frame is 4224x3136 (4:3). The ISP crops it to the 16:9 area
# of CROP_HEIGHT rows starting at row CROP_TOP, then scales it to the
# size of the stream.

readonly SENSOR_WIDTH=4224
readonly SENSOR_HEIGHT=3136
readonly CROP_HEIGHT=2376
readonly CROP_TOP=380

#######################################
# Crop a video node to the 16:9 area of the sensor frame.
# Arguments:
#   Video node, for example /dev/video11.
# Returns:
#   Non-zero when v4l2-ctl fails.
#######################################
crop_16_9() {
  local -r node="$1"
  local -r size="width=${SENSOR_WIDTH},height=${CROP_HEIGHT}"
  local -r area="top=${CROP_TOP},left=0,${size}"
  v4l2-ctl -d "${node}" --set-selection="target=crop,${area}" >/dev/null
}

#######################################
# Give every node back the full sensor frame. Best effort: a failure
# is ignored, so a cleanup after an error can go on.
# Arguments:
#   Video nodes.
#######################################
restore_full_frame() {
  local -r area="top=0,left=0,width=${SENSOR_WIDTH},height=${SENSOR_HEIGHT}"
  local node
  for node in "$@"; do
    v4l2-ctl -d "${node}" --set-selection="target=crop,${area}" \
      >/dev/null 2>&1
  done
  echo
  echo "Crop restored to the full frame."
}

#######################################
# Set the NV12 size a node outputs.
# Arguments:
#   Video node, width, height.
# Returns:
#   Non-zero when v4l2-ctl fails.
#######################################
set_nv12_size() {
  local -r node="$1" width="$2" height="$3"
  v4l2-ctl -d "${node}" \
    --set-fmt-video="width=${width},height=${height},pixelformat=NV12" \
    >/dev/null
}

#######################################
# Print the size, frame count and duration of a recorded file, and the
# scp command that copies it to a PC.
# Arguments:
#   File.
# Returns:
#   Non-zero when the file is missing or empty.
#######################################
report_file() {
  local -r file="$1"
  local address info size
  if [[ ! -s "${file}" ]]; then
    echo "${file}: not written" >&2
    return 1
  fi
  info="$(ffprobe -v error \
    -show_entries stream=width,height,nb_frames,duration \
    -of csv=p=0 "${file}" | tr '\n' ' ')"
  size="$(du -h "${file}" | cut -f1)"
  address="$(hostname -I | awk '{print $1}')"
  echo "${file}: ${info}${size}"
  echo "  scp ${USER}@${address}:${file} ."
}
