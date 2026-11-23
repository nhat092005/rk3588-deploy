"""Run the FP32 ONNX detectors on the reference input (PC).

The floating-point counterpart of ``run_map_board.py``: the same frames,
the same reference input (``model_io.preprocess`` on the decoded NV12
frame) and the same post-processing, on ``onnxruntime`` instead of the
NPU. It shows how much mAP the INT8 conversion costs and anchors the
numbers (YOLOv8n of Ultralytics is 37.3 mAP@0.5:0.95, the zoo model is
an optimised variant, so a gap is expected).

Writes ``shard_fp32.jsonl.gz`` and ``shard_fp32.done`` to ``OUT`` in the
format of ``run_map_board.py``, as the models ``<name>_fp32``, input
``ref``. Put the folder next to the board results for ``eval_map.py``.

Usage: run_map_onnx.py FRAMES ONNX_DIR OUT [--limit N]
Exit code 0 when every frame was done, 2 on a bad command line.
"""

import argparse
import json
import sys
import time
from pathlib import Path

import numpy as np
import onnxruntime
from detect_postprocess import detect, to_coco
from run_map_board import MODELS, append_line, reference_input

NAME = "shard_fp32"
SUFFIX = "_fp32"
PIXEL_MAX = 255.0
PROGRESS_EVERY = 100


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("frames", type=Path, help="NV12 frames and sizes")
    parser.add_argument("onnx", type=Path, help="folder of the .onnx")
    parser.add_argument("out", type=Path, help="folder of the results")
    parser.add_argument("--limit", type=int, default=0, help="first N ids")
    args = parser.parse_args(argv)
    for name in MODELS:
        if not (args.onnx / f"{name}.onnx").is_file():
            parser.error(f"{args.onnx / (name + '.onnx')}: missing")
    return args


def main(argv: list[str]) -> int:
    """Run the frames, return the exit code."""
    args = parse_args(argv)
    sizes = json.loads((args.frames / "sizes.json").read_text())
    ids = sorted(sizes, key=int)
    if args.limit:
        ids = ids[: args.limit]
    args.out.mkdir(parents=True, exist_ok=True)
    done_path = args.out / f"{NAME}.done"
    done = set(done_path.read_text().split()) if done_path.exists() else set()
    todo = [image_id for image_id in ids if image_id not in done]
    sessions = {
        name: onnxruntime.InferenceSession(
            str(args.onnx / f"{name}.onnx"),
            providers=["CPUExecutionProvider"],
        )
        for name in MODELS
    }
    started = time.monotonic()
    for count, image_id in enumerate(todo, start=1):
        width, height, wide, tall = sizes[image_id]
        image, meta = reference_input(
            args.frames / f"{image_id}.nv12", (wide, tall)
        )
        results = {}
        for name, family in MODELS.items():
            scale = 1.0 if family == "yolox" else PIXEL_MAX
            data = image.transpose(2, 0, 1)[None].astype(np.float32) / scale
            session = sessions[name]
            outputs = session.run(None, {session.get_inputs()[0].name: data})
            boxes, classes, scores = detect(family, outputs)
            results[name + SUFFIX] = {
                "ref": to_coco(boxes, classes, scores, meta, (width, height))
            }
        append_line(
            args.out,
            NAME,
            {"image_id": int(image_id), "missing": [], "results": results},
        )
        if count % PROGRESS_EVERY == 0 or count == len(todo):
            elapsed = time.monotonic() - started
            print(f"{count}/{len(todo)} frames, {elapsed:.0f} s", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
