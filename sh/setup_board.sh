#!/usr/bin/env bash
# Create the board venv (no sudo) in the board root, the directory the meta-repo make sync-board fills
# (common/python, deploy/, 3rdparty/ wheel). Run on the board: bash <board root>/deploy/sh/setup_board.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 -m venv .venv
.venv/bin/pip install --upgrade pip
.venv/bin/pip install -r deploy/requirements-board.txt
# rk3588_common is imported from common/python, as the editable install does on the PC
site=$(.venv/bin/python -c "import sysconfig; print(sysconfig.get_paths()['purelib'])")
echo "$PWD/common/python" > "$site/rk3588_common.pth"
.venv/bin/python -c "import numpy, cv2, onnxruntime, psutil, yaml, rk3588_common.model_io; from rknnlite.api import RKNNLite; print('numpy', numpy.__version__, 'cv2', cv2.__version__, 'onnxruntime', onnxruntime.__version__, 'rk3588_common and RKNNLite OK')"
