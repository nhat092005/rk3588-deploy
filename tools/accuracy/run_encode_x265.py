"""Run the CPU side of the encode bench (x265) and check stream quality.

Runs on the board, next to ``bench_encode`` (the MPP side), on the same
NV12 clip. x265 is the H.265 encoder of FFmpeg (libx265); it takes
planar frames, so the clip is first rewritten as I420 (the same bytes,
Cb and Cr in two planes), which costs nothing in the timed runs.

For every case ``PRESET:THREADS`` (for example ``ultrafast:8``) the
script runs ``ffmpeg -benchmark`` on the clip, a constant bitrate of
``--kbps`` (VBV equal to the rate), a key frame every ``--gop`` frames,
x265 thread pool of THREADS threads, and records the frames per
second (frames over the real time of the whole ffmpeg run), the CPU time
per frame (user plus system, all threads), the bytes of the stream and
its bitrate. A warmup run comes first and is dropped. ``--stream`` names
extra streams (for example the ``--out`` file of ``bench_encode``): each
one, and the stream of the last run of each case, is decoded and
compared with the clip, PSNR of the three planes (all frames), by
FFmpeg.

Usage: run_encode_x265.py CLIP.nv12 OUT.json [--size WxH] [--kbps N]
[--fps N] [--gop N] [--runs N] [--cases P:T,...]
[--stream LABEL=FILE ...]
Exit code 0 on success, 1 when ffmpeg fails, 2 on a bad command line.
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

import numpy as np

DEFAULT_CASES = "ultrafast:8,ultrafast:1,veryfast:8"
BENCH_LINE = re.compile(
    r"bench: utime=(?P<u>[\d.]+)s stime=(?P<s>[\d.]+)s rtime=(?P<r>[\d.]+)s"
)
PSNR_LINE = re.compile(
    r"PSNR y:(?P<y>[\d.inf]+) u:(?P<u>[\d.inf]+) v:(?P<v>[\d.inf]+)"
    r" average:(?P<avg>[\d.inf]+)"
)
FFMPEG = ("ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "info")


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("clip", type=Path, help="NV12 clip")
    parser.add_argument("out", type=Path, help="JSON file to write")
    parser.add_argument("--size", default="3840x2160")
    parser.add_argument("--kbps", type=int, default=20000)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--gop", type=int, default=30)
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--cases", default=DEFAULT_CASES)
    parser.add_argument(
        "--stream", action="append", default=[], metavar="LABEL=FILE"
    )
    return parser.parse_args(argv)


def write_i420(clip: Path, i420: Path, width: int, height: int) -> int:
    """Write the I420 copy of an NV12 clip, return its frame count."""
    frame = width * height * 3 // 2
    frames = clip.stat().st_size // frame
    data = np.memmap(clip, np.uint8, "r", shape=(frames, frame))
    with i420.open("wb") as out:
        for index in range(frames):
            planes = np.asarray(data[index])
            luma = planes[: width * height]
            chroma = planes[width * height :].reshape(-1, 2)
            out.write(luma.tobytes())
            out.write(chroma[:, 0].tobytes())
            out.write(chroma[:, 1].tobytes())
    return frames


def encode(
    i420: Path, case: str, args: argparse.Namespace, stream: Path
) -> tuple[float, float, float] | None:
    """Run one x265 encode: (utime, stime, rtime), None on failure."""
    preset, threads = case.split(":")
    rate = f"{args.kbps}k"
    params = (
        f"keyint={args.gop}:min-keyint={args.gop}:scenecut=0:"
        f"pools={threads}:log-level=error"
    )
    command = [
        *FFMPEG,
        "-benchmark",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "yuv420p",
        "-s",
        args.size,
        "-r",
        str(args.fps),
        "-i",
        str(i420),
        "-c:v",
        "libx265",
        "-preset",
        preset,
        "-b:v",
        rate,
        "-maxrate",
        rate,
        "-bufsize",
        rate,
        "-x265-params",
        params,
        "-f",
        "hevc",
        "-y",
        str(stream),
    ]
    done = subprocess.run(command, capture_output=True, text=True, check=False)
    match = BENCH_LINE.search(done.stderr)
    if done.returncode != 0 or match is None:
        sys.stderr.write(done.stderr[-2000:])
        return None
    return (float(match["u"]), float(match["s"]), float(match["r"]))


def psnr(stream: Path, i420: Path, size: str) -> dict | None:
    """Return the PSNR of a stream against the clip, all frames."""
    raw = ("-f", "rawvideo", "-pix_fmt", "yuv420p", "-s", size)
    decode = subprocess.Popen(
        [
            *FFMPEG,
            "-loglevel",
            "error",
            "-i",
            str(stream),
            "-f",
            "rawvideo",
            "-",
        ],
        stdout=subprocess.PIPE,
    )
    compare = subprocess.run(
        [
            *FFMPEG,
            *raw,
            "-i",
            "-",
            *raw,
            "-i",
            str(i420),
            "-lavfi",
            "[0:v][1:v]psnr",
            "-f",
            "null",
            "-",
        ],
        stdin=decode.stdout,
        capture_output=True,
        text=True,
        check=False,
    )
    decode.stdout.close()
    decode.wait()
    match = PSNR_LINE.search(compare.stderr)
    if match is None:
        return None
    return {k: float(match[k]) for k in ("y", "u", "v", "avg")}


def main(argv: list[str]) -> int:
    """Run the cases, write the JSON, return the exit code."""
    args = parse_args(argv)
    width, height = (int(v) for v in args.size.split("x"))
    i420 = args.clip.with_suffix(".i420")
    frames = (
        write_i420(args.clip, i420, width, height)
        if not i420.exists()
        else i420.stat().st_size // (width * height * 3 // 2)
    )
    result: dict = {
        "schema": 1,
        "stage": "encode",
        "method": "x265",
        "width": width,
        "height": height,
        "frames": frames,
        "target_kbps": args.kbps,
        "fps": args.fps,
        "gop": args.gop,
        "runs": args.runs,
        "cases": [],
        "quality": [],
    }
    streams = {}
    for case in args.cases.split(","):
        stream = args.clip.with_name(f"x265_{case.replace(':', '_')}.h265")
        timings = []
        for run in range(args.runs + 1):  # the first run is the warmup
            times = encode(i420, case, args, stream)
            if times is None:
                return 1
            if run > 0:
                timings.append(times)
        size = stream.stat().st_size
        entry = {
            "case": case,
            "bytes": size,
            "kbps": size * 8 / (frames / args.fps) / 1000,
            "runs": [
                {
                    "encode_fps": frames / real,
                    "cpu_ms_per_frame": (user + system) * 1000 / frames,
                    "real_s": real,
                }
                for user, system, real in timings
            ],
        }
        result["cases"].append(entry)
        streams[f"x265 {case}"] = stream
        sys.stdout.write(
            f"{case}: "
            + ", ".join(
                f"{r['encode_fps']:.2f} fps {r['cpu_ms_per_frame']:.1f} ms"
                for r in entry["runs"]
            )
            + f", {entry['kbps']:.0f} kbps\n"
        )
    for item in args.stream:
        label, _, path = item.partition("=")
        streams[label] = Path(path)
    for label, stream in streams.items():
        value = psnr(stream, i420, args.size)
        result["quality"].append({"label": label, "psnr": value})
        sys.stdout.write(f"{label}: PSNR {value}\n")
    args.out.write_text(json.dumps(result, indent=1) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
