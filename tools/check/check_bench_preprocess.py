"""Check the dump of bench_preprocess against rk3588_common.model_io.

``bench_preprocess --dump DIR`` writes ``frame.nv12`` (the input) and
one ``<label>.rgb`` (640x640x3, raw) per method. The reference is the
NV12 frame in limited range (what the RGA decodes), to BGR, rotated 180
degrees, then ``preprocess`` (Ultralytics val LetterBox).

Per method it checks the layout and prints the difference:

* layout: every padding pixel must equal the reference padding exactly.
  The padding region and value are read from the reference: the region
  is where the output of ``preprocess`` depends on the input, the value
  is what ``preprocess`` writes elsewhere.
* difference: mean, max and the share of values that differ by more than
  BIG_DIFF, over the image region and over all values.

Usage: check_bench_preprocess.py DIR width height
Exit code 0 when every layout is exact and the channel order is RGB, 1
otherwise, 2 when a file is missing.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
from check_letterbox import BIG_DIFF, nv12_to_bgr

from rk3588_common.model_io import IMGSZ, preprocess

ROTATION = cv2.ROTATE_180  # camera.rotation of app_config.json
FRAME = "frame.nv12"
EXIT_MISSING = 2


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="folder of --dump")
    parser.add_argument("width", type=int, help="frame width")
    parser.add_argument("height", type=int, help="frame height")
    return parser.parse_args()


def reference_output(dump: Path, width: int, height: int) -> np.ndarray:
    """Return the expected (640, 640, 3) RGB image of the frame."""
    raw = np.fromfile(dump / FRAME, dtype=np.uint8)
    rows = height * 3 // 2
    yuv = raw[: rows * width].reshape(rows, width)
    bgr = cv2.rotate(nv12_to_bgr(yuv, full_range=False), ROTATION)
    return preprocess(bgr)[0][0]


def padding_of(shape: tuple[int, int]) -> tuple[np.ndarray, np.ndarray]:
    """Return the padding mask and value of preprocess for a size.

    The input is made of two constant images. Where the two outputs
    differ the output depends on the input (the image region), elsewhere
    it is padding, and its value is the padding value.
    """
    height, width = shape
    dark = preprocess(np.zeros((height, width, 3), np.uint8))[0][0]
    light = preprocess(np.full((height, width, 3), 255, np.uint8))[0][0]
    padding = (dark == light).all(axis=2)
    values = np.unique(dark[padding].reshape(-1, 3), axis=0)
    if len(values) != 1:
        raise SystemExit(f"padding has {len(values)} values, expected 1")
    return padding, values[0]


def stats(expected: np.ndarray, actual: np.ndarray) -> str:
    """Return mean, max and the big-difference share of the values."""
    diff = np.abs(expected.astype(int) - actual.astype(int))
    big = float((diff > BIG_DIFF).mean() * 100)
    return (
        f"mean {diff.mean():6.3f} max {int(diff.max()):3d}"
        f" >{BIG_DIFF} {big:6.3f}%"
    )


def check(
    label: str,
    expected: np.ndarray,
    actual: np.ndarray,
    padding: np.ndarray,
    pad_value: np.ndarray,
) -> bool:
    """Print the result of one method, return whether it passes."""
    wrong = int((actual[padding] != pad_value).any(axis=1).sum())
    image = ~padding
    straight = float(np.abs(expected.astype(int) - actual).mean())
    swapped = float(np.abs(expected.astype(int) - actual[..., ::-1]).mean())
    order_ok = straight <= swapped
    print(
        f"{label:4s} padding wrong {wrong} of {int(padding.sum())}"
        f" | image {stats(expected[image], actual[image])}"
        f" | all {stats(expected, actual)}"
        f" | {'RGB' if order_ok else 'BGR (swap channels)'}"
    )
    return wrong == 0 and order_ok


def main() -> int:
    """Check every method of the dump, return the exit code."""
    args = parse_args()
    files = sorted(args.dump.glob("*.rgb"))
    if not (args.dump / FRAME).is_file() or not files:
        print(f"{args.dump}: needs {FRAME} and at least one .rgb file")
        return EXIT_MISSING
    expected = reference_output(args.dump, args.width, args.height)
    padding, pad_value = padding_of((args.height, args.width))
    print(
        f"reference: padding value {pad_value.tolist()},"
        f" image region {int((~padding).sum())} pixels,"
        f" padding {int(padding.sum())} pixels"
    )
    passed = True
    for path in files:
        actual = np.fromfile(path, dtype=np.uint8).reshape(IMGSZ, IMGSZ, 3)
        passed &= check(path.stem, expected, actual, padding, pad_value)
    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
