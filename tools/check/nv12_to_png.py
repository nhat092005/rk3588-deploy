#!/usr/bin/env python3
"""Convert a raw NV12 frame (the out.raw of probe_camera) to a PNG.

The file has no header, so width and height must be the ones
probe_camera ran with. The default output is the input path with a .png
suffix. Needs numpy and OpenCV (the repo .venv has both).
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

# An NV12 frame has height * 3 / 2 rows of `width` bytes: the Y plane,
# then the interleaved UV plane.
CHROMA_ROWS_NUM = 3
CHROMA_ROWS_DEN = 2


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("file", type=Path, help="raw NV12 frame")
    parser.add_argument("width", type=int, help="frame width, even")
    parser.add_argument("height", type=int, help="frame height, even")
    parser.add_argument(
        "out", type=Path, nargs="?", help="output PNG (default: file.png)"
    )
    args = parser.parse_args(argv)
    if args.width <= 0 or args.height <= 0:
        parser.error("width and height must be positive")
    if args.width % 2 or args.height % 2:
        parser.error("width and height must be even")
    return args


def main(argv: list[str]) -> int:
    """Convert the frame and print its size and luma statistics."""
    args = parse_args(argv)
    width, height = args.width, args.height
    dst = args.out or args.file.with_suffix(".png")

    rows = height * CHROMA_ROWS_NUM // CHROMA_ROWS_DEN
    expected = width * rows
    try:
        raw = np.fromfile(args.file, dtype=np.uint8)
    except OSError as err:
        print(f"cannot read {args.file}: {err}", file=sys.stderr)
        return 1
    if raw.size != expected:
        print(
            f"size mismatch: {args.file} has {raw.size} bytes, "
            f"NV12 {width}x{height} needs {expected}",
            file=sys.stderr,
        )
        return 1

    bgr = cv2.cvtColor(raw.reshape(rows, width), cv2.COLOR_YUV2BGR_NV12)
    if not cv2.imwrite(str(dst), bgr):
        print(f"cannot write {dst}", file=sys.stderr)
        return 1
    luma = raw[: width * height]
    print(
        f"{dst}: {width}x{height}, luma min/mean/max "
        f"{luma.min()}/{luma.mean():.1f}/{luma.max()}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
