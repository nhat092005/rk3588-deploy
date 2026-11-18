#!/usr/bin/env bash
#
# Create the board venv (no sudo) in the board root.
#
# The board root is the directory that `make sync-board` of the meta-repo
# fills (common/python, deploy/, the 3rdparty wheel).
# Run on the board: bash <board root>/deploy/sh/env/setup_board.sh

set -euo pipefail

main() {
  local site
  cd "$(dirname "$0")/../../.."
  python3 -m venv .venv
  .venv/bin/pip install --upgrade pip
  .venv/bin/pip install -r deploy/requirements-board.txt
  # rk3588_common is imported from common/python, as the editable install
  # does on the PC.
  site="$(.venv/bin/python -c \
    "import sysconfig; print(sysconfig.get_paths()['purelib'])")"
  echo "${PWD}/common/python" > "${site}/rk3588_common.pth"
  .venv/bin/python - <<'EOF'
import cv2
import numpy
import onnxruntime
import psutil
import yaml
from rknnlite.api import RKNNLite
import rk3588_common.model_io
print("numpy", numpy.__version__, "cv2", cv2.__version__,
      "onnxruntime", onnxruntime.__version__,
      "rk3588_common and RKNNLite OK")
EOF
}

main "$@"
