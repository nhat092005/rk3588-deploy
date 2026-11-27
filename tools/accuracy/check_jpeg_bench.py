"""Check the JPEG files that ``bench_jpeg`` wrote (stage 4).

Runs on the board (or anywhere with ``ffmpeg`` and numpy). ``bench_jpeg
--out-dir DIR`` writes ``<method>_q<Q>_f<I>.jpg`` for each image of its
cycle; image I is frame I x STEP of the NV12 clip. This script decodes
every file with FFmpeg (one decoder for both methods), compares it with
that frame, and reads the markers of the file. Per method and quality
it records:

* the PSNR of the luma plane and of the two chroma planes together (dB,
  the mean over the images, 255 as the peak),
* the mean size in bytes,
* the sampling factors of the three components (from the SOF marker),
* the quantization tables (from the DQT markers), and for each quality
  whether the tables of the methods are equal.

Usage: check_jpeg_bench.py CLIP.nv12 DIR OUT.json [--size WxH]
[--frames N] [--step N]
Exit code 0 on success, 1 when a file does not decode to the frame size
or is missing, 2 on a bad command line.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

NAME = re.compile(r"(?P<method>[a-z]+)_q(?P<q>\d+)_f(?P<i>\d+)\.jpg")
SOF_MARKERS = {0xC0, 0xC1, 0xC2}
DQT = 0xDB
SOS = 0xDA
PEAK = 255.0
FFMPEG = ("ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error")


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("clip", type=Path, help="NV12 clip")
    parser.add_argument("directory", type=Path, help="DIR of bench_jpeg")
    parser.add_argument("out", type=Path, help="JSON file to write")
    parser.add_argument("--size", default="3840x2160")
    parser.add_argument("--frames", type=int, default=8)
    parser.add_argument("--step", type=int, default=11)
    return parser.parse_args(argv)


def markers(data: bytes) -> dict:
    """Return the sampling factors and quantization tables of a JPEG."""
    found = {"sampling": [], "tables": {}}
    pos = 2
    while pos + 4 <= len(data) and data[pos] == 0xFF:
        marker = data[pos + 1]
        length = int.from_bytes(data[pos + 2 : pos + 4], "big")
        body = data[pos + 4 : pos + 2 + length]
        if marker in SOF_MARKERS:
            count = body[5]
            found["sampling"] = [
                f"{body[7 + 3 * k] >> 4}x{body[7 + 3 * k] & 15}"
                for k in range(count)
            ]
        elif marker == DQT:
            at = 0
            while at < len(body):
                precision, table = body[at] >> 4, body[at] & 15
                size = 64 * (precision + 1)
                found["tables"][table] = list(body[at + 1 : at + 1 + size])
                at += 1 + size
        elif marker == SOS:
            break
        pos += 2 + length
    return found


def decode(path: Path, width: int, height: int) -> np.ndarray:
    """Decode a JPEG to a flat I420 frame (full range) with FFmpeg."""
    result = subprocess.run(
        [
            *FFMPEG,
            "-i",
            str(path),
            "-f",
            "rawvideo",
            "-pix_fmt",
            "yuvj420p",
            "-",
        ],
        capture_output=True,
        check=True,
    )
    frame = np.frombuffer(result.stdout, np.uint8)
    if frame.size != width * height * 3 // 2:
        message = (
            f"{path}: decoded {frame.size} bytes, not a {width}x{height} frame"
        )
        raise ValueError(message)
    return frame


def psnr(a: np.ndarray, b: np.ndarray) -> float:
    """Return the PSNR in dB between two byte arrays."""
    diff = a.astype(np.int32) - b.astype(np.int32)
    mse = float(np.mean(diff * diff))
    return float("inf") if mse == 0 else 10 * np.log10(PEAK * PEAK / mse)


def source_i420(clip: np.ndarray, width: int, height: int) -> np.ndarray:
    """Return an NV12 frame as I420: Y, then Cb, then Cr."""
    luma = clip[: width * height]
    chroma = clip[width * height :].reshape(-1, 2)
    return np.concatenate([luma, chroma[:, 0], chroma[:, 1]])


def main() -> int:
    """Check every file of the directory and write the JSON."""
    args = parse_args(sys.argv[1:])
    width, height = (int(v) for v in args.size.split("x"))
    frame_bytes = width * height * 3 // 2
    clip = np.memmap(args.clip, np.uint8, "r").reshape(-1, frame_bytes)
    luma_bytes = width * height
    groups: dict[tuple[str, int], list[tuple[int, Path]]] = {}
    for path in sorted(args.directory.glob("*.jpg")):
        found = NAME.fullmatch(path.name)
        if found:
            key = (found["method"], int(found["q"]))
            groups.setdefault(key, []).append((int(found["i"]), path))
    if not groups:
        sys.stderr.write(f"no bench_jpeg files in {args.directory}\n")
        return 1
    cases = []
    for (method, quality), files in sorted(
        groups.items(), key=lambda g: (g[0][1], g[0][0])
    ):
        luma, chroma, sizes = [], [], []
        info = {}
        for index, path in sorted(files):
            raw = path.read_bytes()
            sizes.append(len(raw))
            info = markers(raw)
            source = source_i420(
                np.asarray(clip[index * args.step]), width, height
            )
            decoded = decode(path, width, height)
            luma.append(psnr(decoded[:luma_bytes], source[:luma_bytes]))
            chroma.append(psnr(decoded[luma_bytes:], source[luma_bytes:]))
        cases.append(
            {
                "method": method,
                "quality": quality,
                "images": len(files),
                "psnr_y": float(np.mean(luma)),
                "psnr_c": float(np.mean(chroma)),
                "mean_bytes": float(np.mean(sizes)),
                "sampling": info["sampling"],
                "tables": {str(k): v for k, v in info["tables"].items()},
            }
        )
    by_quality: dict[int, dict[str, dict]] = {}
    for case in cases:
        by_quality.setdefault(case["quality"], {})[case["method"]] = case
    equal = {}
    for quality, pair in sorted(by_quality.items()):
        if len(pair) == 2:
            first, second = pair.values()
            equal[str(quality)] = first["tables"] == second["tables"]
    args.out.write_text(
        json.dumps(
            {
                "schema": 1,
                "stage": "jpeg",
                "size": args.size,
                "step": args.step,
                "tables_equal": equal,
                "cases": cases,
            },
            indent=2,
        )
        + "\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
