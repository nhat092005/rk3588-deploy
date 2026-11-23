"""Run the detectors on the outputs of the preprocess methods (board).

For each NV12 frame of ``make_coco_nv12.py`` it runs ``bench_preprocess
--input FRAME --dump DIR`` (the six methods), builds the reference
input (``rk3588_common.model_io.preprocess`` on the decoded frame,
rotated back), runs every model on the seven inputs on the NPU and
writes the detections as COCO boxes of the original picture. An input
that is byte-identical to an earlier one of the same frame is not run
again (the detectors are deterministic).

Output, in ``--out``: ``shard<K>.jsonl.gz`` with one line per frame,
``{"image_id": 139, "missing": [...], "results": {model: {input:
[[x, y, w, h, score, category], ...]}}}``, and ``shard<K>.done`` with
the finished ids, so a stopped run goes on where it was. ``missing``
lists the methods that the RGA refused for that frame (no output).

Needs the board venv (numpy, OpenCV, rknnlite) with ``rk3588_common``
and ``tools/check`` reachable (this file adds ``../check`` itself).

Usage: run_map_board.py FRAMES MODELS OUT --bench PATH [--limit N]
       [--shard K/N] [--core auto|0|1|2] [--work DIR]
Exit code 0 when every frame was done, 1 on a failed frame, 2 on a bad
command line.
"""

import argparse
import gzip
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from detect_postprocess import IMGSZ, detect, to_coco

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "check"))
from check_bench_preprocess import nv12_full_to_bgr

from rk3588_common.model_io import preprocess

MODELS = {
    "yolov8n": "yolov8",
    "yolov5s": "yolov5",
    "yolov5s_relu": "yolov5",
    "yolox_s": "yolox",
}
LABELS = ("cv1", "cv8", "R2", "R30", "R31", "Ra")
REFERENCE = "ref"
CORES = {"auto": "NPU_CORE_AUTO", "0": "NPU_CORE_0", "1": "NPU_CORE_1"}
CORES["2"] = "NPU_CORE_2"
PROGRESS_EVERY = 25
EXIT_USAGE = 2


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("frames", type=Path, help="NV12 frames and sizes")
    parser.add_argument("models", type=Path, help="folder of the .rknn")
    parser.add_argument("out", type=Path, help="folder of the results")
    parser.add_argument("--bench", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0, help="first N ids")
    parser.add_argument("--shard", default="0/1", help="K/N of the ids")
    parser.add_argument("--core", choices=CORES, default="auto")
    parser.add_argument(
        "--work", type=Path, default=Path("/tmp/map_work"), help="scratch"
    )
    args = parser.parse_args(argv)
    try:
        args.index, args.count = (int(x) for x in args.shard.split("/"))
    except ValueError:
        parser.error("--shard is K/N")
    if not 0 <= args.index < args.count:
        parser.error("--shard K/N needs 0 <= K < N")
    for name in MODELS:
        if not (args.models / f"{name}.rknn").is_file():
            parser.error(f"{args.models / (name + '.rknn')}: missing")
    return args


def load_models(folder: Path, core: str) -> dict:
    """Load every model on the NPU, return {name: RKNNLite}."""
    from rknnlite.api import RKNNLite

    models = {}
    for name in MODELS:
        runtime = RKNNLite()
        if runtime.load_rknn(str(folder / f"{name}.rknn")) != 0:
            raise SystemExit(f"{name}: load_rknn failed")
        mask = getattr(RKNNLite, CORES[core])
        if runtime.init_runtime(core_mask=mask) != 0:
            raise SystemExit(f"{name}: init_runtime failed")
        models[name] = runtime
    return models


def reference_input(
    frame: Path, size: tuple[int, int]
) -> tuple[np.ndarray, tuple[float, int, int]]:
    """Return the reference input (640, 640, 3) and its letterbox meta.

    The NV12 frame, decoded as the CPU method does, rotated back, then
    ``model_io.preprocess`` (border 114).
    """
    width, height = size
    raw = np.fromfile(frame, dtype=np.uint8)
    rows = height * 3 // 2
    bgr = cv2.rotate(
        nv12_full_to_bgr(raw.reshape(rows, width)), cv2.ROTATE_180
    )
    image, (gain, left, top, _, _) = preprocess(bgr)
    return image[0], (gain, left, top)


def run_bench(
    bench: Path, frame: Path, size: tuple[int, int], dump: Path
) -> list[str]:
    """Run the six methods, return the labels that have an output."""
    command = [
        str(bench),
        "--input",
        str(frame),
        "--size",
        f"{size[0]}x{size[1]}",
        "--runs",
        "1",
        "--warmup",
        "0",
        "--dump",
        str(dump),
    ]
    for stale in dump.glob("*.rgb"):
        stale.unlink()
    subprocess.run(command, capture_output=True, check=False)
    return [label for label in LABELS if (dump / f"{label}.rgb").is_file()]


def process_frame(
    image_id: str, sizes: dict, args: argparse.Namespace, models: dict
) -> dict:
    """Return the line of one frame."""
    width, height, wide, tall = sizes[image_id]
    dump = args.work / f"shard{args.index}"
    dump.mkdir(parents=True, exist_ok=True)
    frame = args.frames / f"{image_id}.nv12"
    present = run_bench(args.bench, frame, (wide, tall), dump)
    reference, meta = reference_input(frame, (wide, tall))
    inputs = {REFERENCE: reference}
    for label in present:
        inputs[label] = np.fromfile(dump / f"{label}.rgb", dtype=np.uint8)
        inputs[label] = inputs[label].reshape(IMGSZ, IMGSZ, 3)
    seen: dict[str, str] = {}  # digest of an input -> the input that ran
    results: dict[str, dict[str, list]] = {name: {} for name in models}
    for label, image in inputs.items():
        digest = hashlib.sha1(image.tobytes()).hexdigest()
        if digest in seen:
            for name in models:
                results[name][label] = results[name][seen[digest]]
            continue
        seen[digest] = label
        for name, runtime in models.items():
            outputs = runtime.inference(inputs=[image[None]])
            boxes, classes, scores = detect(MODELS[name], outputs)
            results[name][label] = to_coco(
                boxes, classes, scores, meta, (width, height)
            )
    missing = [label for label in LABELS if label not in present]
    return {
        "image_id": int(image_id),
        "missing": missing,
        "results": results,
    }


def append_line(folder: Path, name: str, line: dict) -> None:
    """Add a frame to ``NAME.jsonl.gz`` and its id to ``NAME.done``."""
    with gzip.open(folder / f"{name}.jsonl.gz", "at") as out:
        out.write(json.dumps(line, separators=(",", ":")) + "\n")
    with (folder / f"{name}.done").open("a") as marker:
        marker.write(f"{line['image_id']}\n")


def main(argv: list[str]) -> int:
    """Run the frames of the shard, return the exit code."""
    args = parse_args(argv)
    sizes = json.loads((args.frames / "sizes.json").read_text())
    ids = sorted(sizes, key=int)
    if args.limit:
        ids = ids[: args.limit]
    ids = ids[args.index :: args.count]
    args.out.mkdir(parents=True, exist_ok=True)
    done_path = args.out / f"shard{args.index}.done"
    done = set(done_path.read_text().split()) if done_path.exists() else set()
    todo = [image_id for image_id in ids if image_id not in done]
    print(f"shard {args.shard}: {len(todo)} frames to do, {len(done)} done")
    models = load_models(args.models, args.core)
    started = time.monotonic()
    failed = 0
    for count, image_id in enumerate(todo, start=1):
        try:
            line = process_frame(image_id, sizes, args, models)
        except (OSError, ValueError, RuntimeError) as error:
            print(f"{image_id}: {error}")
            failed += 1
            continue
        append_line(args.out, f"shard{args.index}", line)
        if count % PROGRESS_EVERY == 0 or count == len(todo):
            elapsed = time.monotonic() - started
            left = elapsed / count * (len(todo) - count)
            print(
                f"{count}/{len(todo)} frames, {elapsed:.0f} s,"
                f" about {left / 60:.0f} min left",
                flush=True,
            )
    for runtime in models.values():
        runtime.release()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
