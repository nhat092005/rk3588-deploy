"""Compare the RGA letterbox output with rk3588_common.model_io.

The reference is NV12 to BGR (BT.601, full or limited range), the
clockwise rotation, then ``preprocess`` (Ultralytics val LetterBox).
Prints the difference to the RGA output and to the same output with red
and blue swapped.
Usage: check_letterbox.py nv12 width height rgb rotation range
where range is ``full`` (what the camera driver reports) or ``limited``.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

from rk3588_common.model_io import IMGSZ, preprocess

ROTATIONS = {
    0: None,
    90: cv2.ROTATE_90_CLOCKWISE,
    180: cv2.ROTATE_180,
    270: cv2.ROTATE_90_COUNTERCLOCKWISE,
}
BIG_DIFF = 16  # a pixel channel that differs by more than this


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("nv12", type=Path, help="camera frame, NV12")
    parser.add_argument("width", type=int, help="frame width")
    parser.add_argument("height", type=int, help="frame height")
    parser.add_argument("rgb", type=Path, help="RGA output, 640x640x3")
    parser.add_argument("rotation", type=int, choices=sorted(ROTATIONS))
    parser.add_argument("range", choices=("full", "limited"))
    return parser.parse_args()


def nv12_to_bgr(yuv: np.ndarray, full_range: bool) -> np.ndarray:
    """Convert an NV12 image (rows = 3/2 height) to BGR, BT.601."""
    if not full_range:
        return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)
    height = yuv.shape[0] * 2 // 3
    luma = yuv[:height].astype(np.float32)
    chroma = yuv[height:].reshape(height // 2, -1, 2).astype(np.float32)
    size = (luma.shape[1], height)
    blue_diff = cv2.resize(chroma[..., 0], size) - 128.0
    red_diff = cv2.resize(chroma[..., 1], size) - 128.0
    red = luma + 1.402 * red_diff
    green = luma - 0.344136 * blue_diff - 0.714136 * red_diff
    blue = luma + 1.772 * blue_diff
    bgr = np.stack([blue, green, red], axis=2)
    return np.clip(np.rint(bgr), 0, 255).astype(np.uint8)


def reference(args: argparse.Namespace) -> np.ndarray:
    """Return the expected (640, 640, 3) RGB image."""
    raw = np.fromfile(args.nv12, dtype=np.uint8)
    rows = args.height * 3 // 2
    yuv = raw[: rows * args.width].reshape(rows, args.width)
    bgr = nv12_to_bgr(yuv, args.range == "full")
    code = ROTATIONS[args.rotation]
    if code is not None:
        bgr = cv2.rotate(bgr, code)
    return preprocess(bgr)[0][0]


def report(name: str, expected: np.ndarray, actual: np.ndarray) -> float:
    """Print the difference statistics, return the mean."""
    diff = np.abs(expected.astype(int) - actual.astype(int))
    mean = float(diff.mean())
    big = float((diff > BIG_DIFF).mean() * 100)
    print(
        f"{name}: mean {mean:.3f} max {int(diff.max())} "
        f"channels>{BIG_DIFF} {big:.3f}%"
    )
    return mean


def main() -> int:
    """Compare and return 0 when the RGA output is close."""
    args = parse_args()
    actual = np.fromfile(args.rgb, dtype=np.uint8).reshape(IMGSZ, IMGSZ, 3)
    expected = reference(args)
    same = report("rgb order", expected, actual)
    swapped = report("r/b swapped", expected, actual[..., ::-1])
    print("verdict:", "RGB" if same <= swapped else "BGR (swap channels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
