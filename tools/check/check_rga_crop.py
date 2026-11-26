"""Compare the RGA letterbox with and without a cropped source rect.

``probe_rga_shift OUTDIR FRAMEDIR`` runs each ``FRAMEDIR/*.nv12`` (from
make_shift_frames.py) with R2, R30 and R30 with the source rectangle
(4, 4, 3836, 2156), and writes ``<id>_<config>.rgb``. The reference is
the frame decoded by OpenCV (limited range), rotated back, then
``rk3588_common.model_io.preprocess``. Over the picture rows (the
padding rows are not written by the probe) it prints, per config, the
mean absolute difference to the reference, the share of values that
differ by more than 16, and the same against R2.

Usage: check_rga_crop.py FRAMEDIR OUTDIR
Exit code 0 on success, 2 when a file is missing.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np

from rk3588_common.model_io import preprocess

WIDTH, HEIGHT = 3840, 2160
SIDE = 640
PAD_TOP = 140
ROWS = 360
BIG_DIFF = 16
CONFIGS = ("R2", "R30", "R30c4")
EXIT_MISSING = 2


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frames", type=Path, help="folder of the NV12 frames")
    parser.add_argument("out", type=Path, help="folder written by the probe")
    return parser.parse_args()


def difference(first: np.ndarray, second: np.ndarray) -> tuple[float, float]:
    """Return (mean, percent over BIG_DIFF) of the picture rows."""
    rows = slice(PAD_TOP, PAD_TOP + ROWS)
    diff = np.abs(first[rows] - second[rows])
    return float(diff.mean()), float((diff > BIG_DIFF).mean() * 100)


def main() -> int:
    """Print the table, return the exit code."""
    args = parse_args()
    names = sorted(path.stem for path in args.frames.glob("*.nv12"))
    if not names:
        print(f"{args.frames}: no frame")
        return EXIT_MISSING
    ref_diff = {c: [] for c in CONFIGS}
    r2_diff = {c: [] for c in CONFIGS}
    for name in names:
        raw = np.fromfile(args.frames / f"{name}.nv12", dtype=np.uint8)
        bgr = cv2.cvtColor(
            raw.reshape(HEIGHT * 3 // 2, WIDTH), cv2.COLOR_YUV2BGR_NV12
        )
        reference = preprocess(cv2.rotate(bgr, cv2.ROTATE_180))[0][0]
        outputs = {}
        for config in CONFIGS:
            path = args.out / f"{name}_{config}.rgb"
            if not path.is_file():
                print(f"{path}: missing")
                return EXIT_MISSING
            outputs[config] = (
                np.fromfile(path, dtype=np.uint8)
                .reshape(SIDE, SIDE, 3)
                .astype(np.int16)
            )
        for config in CONFIGS:
            ref_diff[config].append(
                difference(outputs[config], reference.astype(np.int16))
            )
            r2_diff[config].append(difference(outputs[config], outputs["R2"]))
    print(f"{len(names)} frames")
    print("config  vs reference: mean  share>16 | vs R2: mean  share>16")
    for config in CONFIGS:
        a = np.array(ref_diff[config])
        b = np.array(r2_diff[config])
        print(
            f"{config:6}  {a[:, 0].mean():12.3f} {a[:, 1].mean():8.2f} | "
            f"{b[:, 0].mean():10.3f} {b[:, 1].mean():8.2f}"
        )
    r30 = np.array(ref_diff["R30"])[:, 0]
    crop = np.array(ref_diff["R30c4"])[:, 0]
    print(
        f"R30c4 closer to the reference than R30 in "
        f"{(crop < r30).sum()} of {len(names)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
