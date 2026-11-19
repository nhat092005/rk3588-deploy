"""Check the segments that the continuous recording wrote.

Decodes every .h265 and .mp4 file under a recordings folder and prints its number
of frames and size. A segment passes when it decodes with the same size
every time and its length is within 20 percent of the expected number of
frames (30 frames per second times the segment length). The last segment
of a run may be shorter. Exit code 0 when every segment but the last
passes.
Usage: check_recordings.py <folder> --segment-seconds N [--fps 30]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import cv2

TOLERANCE = 0.2
SUFFIXES = ("*.h265", "*.mp4")


def count_frames(path: Path) -> tuple[int, tuple[int, int] | None]:
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
    """Check every segment and print one line each."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("folder", type=Path)
    parser.add_argument("--segment-seconds", type=float, required=True)
    parser.add_argument("--fps", type=float, default=30.0)
    args = parser.parse_args()
    expected = args.segment_seconds * args.fps
    files = sorted(
        path for suffix in SUFFIXES for path in args.folder.rglob(suffix)
    )
    if not files:
        print("FAIL no segment found")
        return 1
    failed = 0
    sizes = set()
    for index, path in enumerate(files):
        frames, size = count_frames(path)
        sizes.add(size)
        last = index == len(files) - 1
        within = abs(frames - expected) <= TOLERANCE * expected
        good = frames > 0 and (within or last)
        failed += 0 if good else 1
        kilobytes = path.stat().st_size // 1024
        mark = "ok  " if good else "FAIL"
        note = " (last, may be short)" if last and not within else ""
        print(f"{mark} {path.name}: {frames} frames, {kilobytes} KB{note}")
    if len(sizes) != 1:
        print(f"FAIL sizes differ: {sorted(map(str, sizes))}")
        return 1
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
