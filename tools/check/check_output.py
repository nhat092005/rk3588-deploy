"""Check the files that probe_output or the application wrote.

Reads one violation directory (original.jpg, marked.jpg, event.json and
clip.h265). Both JPEGs have the size in event.json, the marked one
differs from the original, the clip decodes with the same size, and the
number of frames is printed. Exit code 0 when every check passes.
Usage: check_output.py <violation directory> [--min-frames N]
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import cv2
import numpy as np

DEFAULT_MIN_FRAMES = 100


def read_jpeg(path: Path) -> np.ndarray | None:
    """Return the decoded image or None."""
    return cv2.imread(str(path))


def count_clip(path: Path) -> tuple[int, tuple[int, int] | None]:
    """Return the number of decodable frames and the (width, height)."""
    capture = cv2.VideoCapture(str(path))
    count = 0
    size = None
    while True:
        ok, frame = capture.read()
        if not ok:
            break
        size = (frame.shape[1], frame.shape[0])
        count += 1
    return count, size


def main() -> int:
    """Run the checks and print one line each."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--min-frames", type=int, default=DEFAULT_MIN_FRAMES)
    args = parser.parse_args()
    folder: Path = args.directory
    info = json.loads((folder / "event.json").read_text())
    width, height = info["image"]["width"], info["image"]["height"]
    original = read_jpeg(folder / "original.jpg")
    marked = read_jpeg(folder / "marked.jpg")
    frames, clip_size = count_clip(folder / "clip.h265")
    same_size = (height, width)
    differs = False
    if original is not None and marked is not None:
        gap = np.abs(original.astype(int) - marked.astype(int))
        differs = float(gap.mean()) > 0.0
    enough = f"clip has at least {args.min_frames} frames"
    results = {
        "original.jpg has the event size": original is not None
        and original.shape[:2] == same_size,
        "marked.jpg has the event size": marked is not None
        and marked.shape[:2] == same_size,
        "marked differs from original": differs,
        "clip has the event size": clip_size == (width, height),
        enough: frames >= args.min_frames,
    }
    for name, passed in results.items():
        print(("ok   " if passed else "FAIL ") + name)
    print(f"clip frames: {frames}")
    return 0 if all(results.values()) else 1


if __name__ == "__main__":
    sys.exit(main())
