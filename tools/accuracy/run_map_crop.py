"""mAP of the RGA letterbox with and without a cropped source rectangle.

Like ``run_map_board.py --canvas`` for the 4K case, but for the job the
app runs by default: the NV12 frame in limited range goes to the RGA
letterbox directly (no full range conversion), with R2, R30 and R30
with the source rectangle (4, 4, 3836, 2156). The three inputs and the
reference (``model_io.preprocess`` on the frame decoded by OpenCV,
rotated back) are run on every model of ``run_map_board.MODELS``.

The pictures are taken in batches of ``--batch``: each is enlarged into
a 3840x2160 canvas (``place_on_canvas``) and written as an NV12 frame,
``probe_rga_shift OUTDIR FRAMEDIR`` runs the RGA jobs for the whole
batch in one process, then the detectors run on the outputs. The
output is in the format of ``run_map_board.py`` (``shard0.jsonl.gz``,
``shard0.done``), with the inputs named ``ref``, ``R2``, ``R30`` and
``R30c4``, so ``eval_map.py`` reads it.

Needs the board venv (numpy, OpenCV, rknnlite) with ``rk3588_common``.

Usage: run_map_crop.py PICTURES MODELS OUT --probe PATH --ids FILE
       [--batch N] [--limit N] [--work DIR]
Exit code 0 when every frame was done, 1 on a failed frame, 2 on a bad
command line.
"""

import argparse
import hashlib
import subprocess
import sys
import time
from pathlib import Path

import cv2
import numpy as np
from detect_postprocess import IMGSZ, compose_meta, detect, to_coco
from run_map_board import (
    CORES,
    MODELS,
    REFERENCE,
    append_line,
    load_models,
)

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "generate"))
from make_coco_nv12 import place_on_canvas
from make_shift_frames import HEIGHT, WIDTH, nv12_limited

from rk3588_common.model_io import preprocess

CONFIGS = ("R2", "R30", "R30c4")
EXIT_USAGE = 2
DEFAULT_BATCH = 100


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("pictures", type=Path, help="folder of pictures")
    parser.add_argument("models", type=Path, help="folder of the .rknn")
    parser.add_argument("out", type=Path, help="folder of the results")
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--ids", type=Path, required=True)
    parser.add_argument("--batch", type=int, default=DEFAULT_BATCH)
    parser.add_argument("--limit", type=int, default=0, help="first N ids")
    parser.add_argument("--core", choices=CORES, default="auto")
    parser.add_argument(
        "--work", type=Path, default=Path("/tmp/map_crop"), help="scratch"
    )
    args = parser.parse_args(argv)
    if args.batch < 1:
        parser.error("--batch must be positive")
    for name in MODELS:
        if not (args.models / f"{name}.rknn").is_file():
            parser.error(f"{args.models / (name + '.rknn')}: missing")
    return args


def write_frames(
    ids: list[str], args: argparse.Namespace, frames: Path
) -> dict[str, tuple[tuple[int, int], tuple[float, int, int]]]:
    """Write the NV12 frames of a batch; return {id: (size, place)}."""
    placed = {}
    for image_id in ids:
        path = args.pictures / f"{int(image_id):012d}.jpg"
        picture = cv2.imread(str(path), cv2.IMREAD_COLOR)
        if picture is None:
            raise OSError(f"{path}: cannot read the picture")
        frame, placement = place_on_canvas(picture, (WIDTH, HEIGHT))
        nv12_limited(cv2.rotate(frame, cv2.ROTATE_180)).tofile(
            frames / f"{image_id}.nv12"
        )
        placed[image_id] = ((picture.shape[1], picture.shape[0]), placement)
    return placed


def reference_input(
    frame: Path,
) -> tuple[np.ndarray, tuple[float, int, int]]:
    """Return the reference input (640x640x3) and its meta."""
    raw = np.fromfile(frame, dtype=np.uint8).reshape(HEIGHT * 3 // 2, WIDTH)
    bgr = cv2.rotate(cv2.cvtColor(raw, cv2.COLOR_YUV2BGR_NV12), cv2.ROTATE_180)
    image, (gain, left, top, _, _) = preprocess(bgr)
    return image[0], (gain, left, top)


def process_frame(
    image_id: str,
    placed: tuple[tuple[int, int], tuple[float, int, int]],
    frames: Path,
    outputs: Path,
    models: dict,
) -> dict:
    """Return the line of one frame."""
    size, placement = placed
    reference, meta = reference_input(frames / f"{image_id}.nv12")
    meta = compose_meta(meta, placement)
    inputs = {REFERENCE: reference}
    missing = []
    for label in CONFIGS:
        path = outputs / f"{image_id}_{label}.rgb"
        if not path.is_file():
            missing.append(label)
            continue
        inputs[label] = np.fromfile(path, dtype=np.uint8).reshape(
            IMGSZ, IMGSZ, 3
        )
    seen: dict[str, str] = {}
    results: dict[str, dict[str, list]] = {name: {} for name in models}
    for label, image in inputs.items():
        digest = hashlib.sha1(image.tobytes()).hexdigest()
        if digest in seen:
            for name in models:
                results[name][label] = results[name][seen[digest]]
            continue
        seen[digest] = label
        for name, runtime in models.items():
            raw = runtime.inference(inputs=[image[None]])
            boxes, classes, scores = detect(MODELS[name], raw)
            results[name][label] = to_coco(boxes, classes, scores, meta, size)
    return {
        "image_id": int(image_id),
        "missing": missing,
        "results": results,
    }


def main(argv: list[str]) -> int:
    """Run the batches, return the exit code."""
    args = parse_args(argv)
    ids = sorted({str(int(n)) for n in args.ids.read_text().split()}, key=int)
    if args.limit:
        ids = ids[: args.limit]
    args.out.mkdir(parents=True, exist_ok=True)
    done_path = args.out / "shard0.done"
    done = set(done_path.read_text().split()) if done_path.exists() else set()
    todo = [image_id for image_id in ids if image_id not in done]
    print(f"{len(todo)} frames to do, {len(done)} done", flush=True)
    models = load_models(args.models, args.core)
    started = time.monotonic()
    failed = finished = 0
    for first in range(0, len(todo), args.batch):
        batch = todo[first : first + args.batch]
        frames = args.work / "frames"
        outputs = args.work / "outputs"
        for folder in (frames, outputs):
            folder.mkdir(parents=True, exist_ok=True)
            for stale in folder.iterdir():
                stale.unlink()
        placed = write_frames(batch, args, frames)
        code = subprocess.run(
            [str(args.probe), str(outputs), str(frames)],
            capture_output=True,
            check=False,
        ).returncode
        if code != 0:
            print(f"the probe failed ({code}) on the batch of {batch[0]}")
            return 1
        for image_id in batch:
            try:
                line = process_frame(
                    image_id, placed[image_id], frames, outputs, models
                )
            except (OSError, ValueError, RuntimeError) as error:
                print(f"{image_id}: {error}")
                failed += 1
                continue
            append_line(args.out, "shard0", line)
            finished += 1
        elapsed = time.monotonic() - started
        left = elapsed / finished * (len(todo) - finished) if finished else 0
        print(
            f"{finished}/{len(todo)} frames, {elapsed:.0f} s,"
            f" about {left / 60:.0f} min left",
            flush=True,
        )
    for runtime in models.values():
        runtime.release()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
