"""Make the 4K NV12 frames of the RGA crop test (limited range).

For each picture of a folder, writes two 3840x2160 NV12 frames, rotated
180 degrees like the camera frame, BT.601 limited range (what the app
sends to the RGA by default):

* ``ENL/<id>.nv12``: the picture enlarged to fill the frame on grey
  (``place_on_canvas`` of make_coco_nv12.py);
* ``TILE/<id>.nv12``: the picture repeated over the frame at its own
  resolution, so the frame is sharp at one pixel.

Usage: make_shift_frames.py PICTURES ENL TILE
Exit code 0 on success, 2 on a bad command line.
"""

import argparse
import sys
from pathlib import Path

import cv2
import numpy as np
from make_coco_nv12 import EXTENSIONS, place_on_canvas

WIDTH, HEIGHT = 3840, 2160


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pictures", type=Path, help="folder of pictures")
    parser.add_argument("enl", type=Path, help="folder of the enlarged frames")
    parser.add_argument("tile", type=Path, help="folder of the tiled frames")
    return parser.parse_args()


def nv12_limited(bgr: np.ndarray) -> np.ndarray:
    """Return the NV12 frame (BT.601 limited range) of a BGR frame."""
    i420 = cv2.cvtColor(bgr, cv2.COLOR_BGR2YUV_I420)
    luma = i420[:HEIGHT]
    blue = i420[HEIGHT : HEIGHT + HEIGHT // 4].reshape(HEIGHT // 2, WIDTH // 2)
    red = i420[HEIGHT + HEIGHT // 4 :].reshape(HEIGHT // 2, WIDTH // 2)
    chroma = np.stack([blue, red], axis=2).reshape(HEIGHT // 2, WIDTH)
    return np.concatenate([luma, chroma], axis=0)


def main() -> int:
    """Write the frames, return the exit code."""
    args = parse_args()
    args.enl.mkdir(parents=True, exist_ok=True)
    args.tile.mkdir(parents=True, exist_ok=True)
    count = 0
    for path in sorted(args.pictures.iterdir()):
        if path.suffix.lower() not in EXTENSIONS:
            continue
        picture = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if picture is None:
            print(f"{path}: cannot read the picture")
            return 2
        height, width = picture.shape[:2]
        frame, _ = place_on_canvas(picture, (WIDTH, HEIGHT))
        nv12_limited(cv2.rotate(frame, cv2.ROTATE_180)).tofile(
            args.enl / f"{path.stem}.nv12"
        )
        repeats = (-(-HEIGHT // height), -(-WIDTH // width), 1)
        tiled = np.tile(picture, repeats)[:HEIGHT, :WIDTH]
        nv12_limited(cv2.rotate(tiled, cv2.ROTATE_180)).tofile(
            args.tile / f"{path.stem}.nv12"
        )
        count += 1
    print(f"{count} pictures, two frames each")
    return 0


if __name__ == "__main__":
    sys.exit(main())
