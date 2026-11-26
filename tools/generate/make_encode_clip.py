"""Make the 4K NV12 clip of the encode bench from COCO pictures.

Real pictures with a known, repeatable motion: the first pictures of
a folder (sorted by name) are cropped to 4:3, scaled to 640x480 and laid
side by side into one mosaic; the clip is a window of the mosaic that
moves right by ``--step`` pixels per frame, like a camera panning. Each
frame is converted to NV12, BT.601 full range (what the camera of the
board reports, ``make_coco_nv12.to_nv12``).

An encoder's time and bitrate depend on the picture and the motion,
so the clip is a stand-in, not a scene of the application. The camera
of the board gave no usable one (the scene was almost black).

Usage: make_encode_clip.py PICTURES OUT.nv12 [--frames N]
[--step PIXELS] [--size WxH]
Exit code 0 on success, 2 on a bad command line or too few pictures.
"""

import argparse
import math
import sys
from pathlib import Path

import cv2
import numpy as np
from make_coco_nv12 import EXTENSIONS, to_nv12

TILE_W, TILE_H = 640, 480
DEFAULT_FRAMES = 90
DEFAULT_STEP = 8
DEFAULT_SIZE = "3840x2160"


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pictures", type=Path, help="folder of pictures")
    parser.add_argument("out", type=Path, help="NV12 file to write")
    parser.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    parser.add_argument("--step", type=int, default=DEFAULT_STEP)
    parser.add_argument("--size", default=DEFAULT_SIZE)
    return parser.parse_args(argv)


def tile_of(path: Path) -> np.ndarray:
    """Return a picture cropped to 4:3 and scaled to one mosaic tile."""
    bgr = cv2.imread(str(path), cv2.IMREAD_COLOR)
    if bgr is None:
        message = f"{path}: cannot read the picture"
        raise ValueError(message)
    height, width = bgr.shape[:2]
    keep_w = min(width, height * TILE_W // TILE_H)
    keep_h = keep_w * TILE_H // TILE_W
    left, top = (width - keep_w) // 2, (height - keep_h) // 2
    crop = bgr[top : top + keep_h, left : left + keep_w]
    return cv2.resize(crop, (TILE_W, TILE_H), interpolation=cv2.INTER_AREA)


def mosaic(pictures: list[Path], width: int, height: int) -> np.ndarray:
    """Return a mosaic of at least width x height, filled row by row."""
    cols, rows = math.ceil(width / TILE_W), math.ceil(height / TILE_H)
    if len(pictures) < cols * rows:
        message = f"need {cols * rows} pictures, found {len(pictures)}"
        raise ValueError(message)
    tiles = [tile_of(p) for p in pictures[: cols * rows]]
    return np.vstack(
        [np.hstack(tiles[r * cols : (r + 1) * cols]) for r in range(rows)]
    )


def main(argv: list[str]) -> int:
    """Write the clip, return the exit code."""
    args = parse_args(argv)
    width, height = (int(v) for v in args.size.split("x"))
    pictures = sorted(
        p for p in args.pictures.iterdir() if p.suffix.lower() in EXTENSIONS
    )
    try:
        canvas = mosaic(
            pictures, width + args.step * (args.frames - 1), height
        )
    except ValueError as error:
        sys.stderr.write(f"{error}\n")
        return 2
    with args.out.open("wb") as out:
        for index in range(args.frames):
            left = args.step * index
            window = canvas[:height, left : left + width]
            out.write(to_nv12(np.ascontiguousarray(window)).tobytes())
    sys.stdout.write(
        f"{args.frames} frames of {width}x{height}, step {args.step}, "
        f"{len(pictures)} pictures available\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
