"""Convert pictures (COCO val2017) to NV12 frames for bench_preprocess.

The accuracy flow feeds real pictures to the six preprocess methods:
``bench_preprocess --input FILE.nv12 --size WxH --dump DIR``. A frame of
the bench has both sides even and a width that is a multiple of 16
(the RGA stride rule), so each picture is padded at the right and at the
bottom by repeating its edge pixels, and rotated by 180 degrees (the
stage rotates it back, so the picture comes out upright with the padding
at the right and at the bottom, and the boxes of the original picture
stay valid). The conversion is BT.601 full range, what the camera of
the board reports, with the Cb Cr of each 2x2 block averaged.

Writes ``<image id>.nv12`` per picture and ``sizes.json`` that maps the
image id to ``[width, height, padded width, padded height]``. The image
id is the number in the file name (COCO: 000000000139.jpg is 139).

Usage: make_coco_nv12.py IMAGES_DIR OUT_DIR [--limit N]
Exit code 0 when every picture was converted, 1 when one could not be
read, 2 on a bad command line.
"""

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

STRIDE_ALIGNMENT = 16  # width multiple of the RGA RGB888 buffers
CANVAS_FILL = 114  # grey of the canvas around a picture
SIZES = "sizes.json"
EXTENSIONS = {".jpg", ".jpeg", ".png", ".bmp"}


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("images", type=Path, help="folder of pictures")
    parser.add_argument("out", type=Path, help="folder of the NV12 frames")
    parser.add_argument(
        "--limit", type=int, default=0, help="first N pictures by name"
    )
    args = parser.parse_args(argv)
    if not args.images.is_dir():
        parser.error(f"{args.images}: not a folder")
    if args.limit < 0:
        parser.error("--limit must not be negative")
    return args


def padded_size(width: int, height: int) -> tuple[int, int]:
    """Return the bench frame size: width of 16s, even height."""
    wide = -(-width // STRIDE_ALIGNMENT) * STRIDE_ALIGNMENT
    return wide, height + height % 2


def to_nv12(bgr: np.ndarray) -> np.ndarray:
    """Return the NV12 frame (rows = 3/2 height) of a BGR picture.

    cv2.COLOR_BGR2YCrCb is BT.601 full range (the I420 and NV12 codes of
    OpenCV are limited range). The Cb Cr of each 2x2 block are averaged.
    """
    height, width = bgr.shape[:2]
    luma, red, blue = cv2.split(cv2.cvtColor(bgr, cv2.COLOR_BGR2YCrCb))
    chroma = np.stack([blue, red], axis=2).astype(np.float32)
    chroma = chroma.reshape(height // 2, 2, width // 2, 2, 2).mean(axis=(1, 3))
    chroma = np.rint(chroma).astype(np.uint8).reshape(height // 2, width)
    return np.concatenate([luma, chroma])


def convert(picture: np.ndarray) -> tuple[np.ndarray, tuple[int, int]]:
    """Return the NV12 frame of a picture and its padded size."""
    height, width = picture.shape[:2]
    wide, tall = padded_size(width, height)
    padded = cv2.copyMakeBorder(
        picture, 0, tall - height, 0, wide - width, cv2.BORDER_REPLICATE
    )
    return to_nv12(cv2.rotate(padded, cv2.ROTATE_180)), (wide, tall)


def place_on_canvas(
    picture: np.ndarray, canvas: tuple[int, int]
) -> tuple[np.ndarray, tuple[float, int, int]]:
    """Enlarge a picture to fill a canvas and return (frame, placement).

    The picture keeps its aspect ratio, is enlarged as far as the canvas
    allows (cubic) and is centred, with even offsets, on grey. The
    placement is (scale, left, top): a canvas pixel (x, y) is the
    picture pixel ((x - left) / scale, (y - top) / scale). The frame is
    upright, rotated 180 degrees and sent through ``to_nv12`` by the
    caller.
    """
    wide, tall = canvas
    height, width = picture.shape[:2]
    scale = min(wide / width, tall / height)
    new_w = min(wide, round(width * scale))
    new_h = min(tall, round(height * scale))
    resized = cv2.resize(
        picture, (new_w, new_h), interpolation=cv2.INTER_CUBIC
    )
    left = (wide - new_w) // 2 // 2 * 2
    top = (tall - new_h) // 2 // 2 * 2
    frame = np.full((tall, wide, 3), CANVAS_FILL, dtype=np.uint8)
    frame[top : top + new_h, left : left + new_w] = resized
    return frame, (scale, left, top)


def main(argv: list[str]) -> int:
    """Convert the pictures, return the exit code."""
    args = parse_args(argv)
    names = sorted(
        path
        for path in args.images.iterdir()
        if path.suffix.lower() in EXTENSIONS
    )
    if args.limit:
        names = names[: args.limit]
    args.out.mkdir(parents=True, exist_ok=True)
    sizes: dict[str, list[int]] = {}
    failed = 0
    for path in names:
        picture = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if picture is None:
            print(f"{path}: cannot read the picture")
            failed += 1
            continue
        frame, (wide, tall) = convert(picture)
        frame.tofile(args.out / f"{int(path.stem)}.nv12")
        height, width = picture.shape[:2]
        sizes[str(int(path.stem))] = [width, height, wide, tall]
    (args.out / SIZES).write_text(json.dumps(sizes, indent=0) + "\n")
    print(f"{len(sizes)} frames in {args.out}, {failed} unreadable")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
