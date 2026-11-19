#!/usr/bin/env bash
#
# Install the MediaMTX server that the live stream of rk3588_deploy is published to.
# Needs internet on the board, no root. The release is pinned and its SHA-256 is checked.
# Remove it by deleting the folder.
#
# Run on the board:
#   bash deploy/sh/env/install_mediamtx.sh
#
# Environment: MEDIAMTX_DIR, the install folder (default ~/rk3588-board/mediamtx).
# Without internet on the board, download the archive on the PC and pass its path as the
# first argument; the checksum is still checked against the pinned value.

set -euo pipefail

readonly VERSION="v1.21.4"
readonly ARCHIVE="mediamtx_${VERSION}_linux_arm64.tar.gz"
readonly SHA256="47784081da54c0608ea4dc359f5cfb19880209434769c532b3ed3a67ea99c29d"
readonly URL="https://github.com/bluenviron/mediamtx/releases/download/${VERSION}/${ARCHIVE}"
readonly MEDIAMTX_DIR="${MEDIAMTX_DIR:-${HOME}/rk3588-board/mediamtx}"

WORK_DIR=""

#######################################
# Remove the download folder, if one was made.
#######################################
cleanup() {
  if [[ -n "${WORK_DIR}" ]]; then
    rm -rf "${WORK_DIR}"
  fi
}

main() {
  local archive="${1:-}"
  trap cleanup EXIT

  if [[ -z "${archive}" ]]; then
    WORK_DIR="$(mktemp -d)"
    archive="${WORK_DIR}/${ARCHIVE}"
    curl -fsSL -o "${archive}" "${URL}"
  fi
  echo "${SHA256}  ${archive}" | sha256sum -c -

  mkdir -p "${MEDIAMTX_DIR}"
  tar -xzf "${archive}" -C "${MEDIAMTX_DIR}" mediamtx
  echo "installed ${VERSION} in ${MEDIAMTX_DIR}"
}

main "$@"
