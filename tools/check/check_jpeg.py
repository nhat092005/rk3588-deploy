"""Compare the JPEG of probe_jpeg with the NV12 copy it came from.

The NV12 file is converted to BGR (BT.601, full or limited range), the
JPEG is decoded, and the script prints the mean absolute difference and
the PSNR per channel. A colour range or channel order mistake shows as
a large mean difference. It also writes a small preview PNG of the JPEG.
Usage: check_jpeg.py <nv12> <jpg> <width> <height> <full|limited> [png]
where width and height are the size of the NV12 copy and the range is
what the camera driver reports (full for this camera).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

PREVIEW_WIDTH = 960
NV12_ROWS_FACTOR = 3 / 2
MAX_MEAN_DIFF = 3.0  # 8-bit levels; JPEG at high quality stays near 1


def parse_args() -> argparse.Namespace:
    """Read the command line."""
    parser = argparse.ArgumentParser()
    parser.add_argument("nv12", type=Path)
    parser.add_argument("jpg", type=Path)
    parser.add_argument("width", type=int)
    parser.add_argument("height", type=int)
    parser.add_argument("range", choices=("full", "limited"))
    parser.add_argument("preview", type=Path, nargs="?")
    return parser.parse_args()


def load_nv12(
    path: Path, width: int, height: int, *, full_range: bool
) -> np.ndarray:
    """Return the NV12 file as a BGR image."""
    raw = np.fromfile(path, dtype=np.uint8)
    rows = int(height * NV12_ROWS_FACTOR)
    if raw.size != width * rows:
        sys.exit(f"{path}: {raw.size} bytes, expected {width * rows}")
    yuv = raw.reshape(rows, width)
    if not full_range:
        return cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)
    luma = yuv[:height].astype(np.float32)
    chroma = yuv[height:].reshape(height // 2, -1, 2).astype(np.float32)
    blue_diff = cv2.resize(chroma[..., 0], (width, height)) - 128.0
    red_diff = cv2.resize(chroma[..., 1], (width, height)) - 128.0
    red = luma + 1.402 * red_diff
    green = luma - 0.344136 * blue_diff - 0.714136 * red_diff
    blue = luma + 1.772 * blue_diff
    bgr = np.stack([blue, green, red], axis=2)
    return np.clip(np.rint(bgr), 0, 255).astype(np.uint8)


def main() -> int:
    """Print the differences, return 0 when the colours agree."""
    args = parse_args()
    reference = load_nv12(
        args.nv12, args.width, args.height, full_range=args.range == "full"
    )
    decoded = cv2.imread(str(args.jpg), cv2.IMREAD_COLOR)
    if decoded is None:
        sys.exit(f"cannot decode {args.jpg}")
    if decoded.shape != reference.shape:
        sys.exit(f"size {decoded.shape} differs from {reference.shape}")
    diff = np.abs(decoded.astype(np.int16) - reference.astype(np.int16))
    mean = diff.mean(axis=(0, 1))
    mse = ((decoded.astype(np.float64) - reference) ** 2).mean(axis=(0, 1))
    psnr = 10 * np.log10(255.0**2 / np.maximum(mse, 1e-12))
    print(f"jpeg {decoded.shape[1]}x{decoded.shape[0]}")
    print(f"mean abs diff BGR {np.round(mean, 2).tolist()}")
    print(f"PSNR BGR {np.round(psnr, 1).tolist()} dB")
    levels = [
        np.round(image.mean(axis=(0, 1)), 1).tolist()
        for image in (decoded, reference)
    ]
    print(f"mean level BGR jpeg {levels[0]}, nv12 {levels[1]}")
    if args.preview is not None:
        scale = PREVIEW_WIDTH / decoded.shape[1]
        small = cv2.resize(decoded, None, fx=scale, fy=scale)
        cv2.imwrite(str(args.preview), small)
    return 0 if float(mean.max()) <= MAX_MEAN_DIFF else 1


if __name__ == "__main__":
    sys.exit(main())
