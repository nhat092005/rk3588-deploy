"""Paired bootstrap over pictures of the mAP change against an input.

Reads the shards of ``run_map_board.py`` or ``run_map_crop.py``. The
pictures are resampled with replacement B times, the same pictures for
every (model, input), and mAP@0.5:0.95 is computed again on each
sample. For each input it prints the change against the reference
input, per model and as the mean of the models, with the 95 % interval
(2.5 and 97.5 percentiles) over the samples, and writes every sample to
``--out`` as JSON.

Needs pycocotools. Each worker holds the ground truth (about 1 GB).

Usage: boot_map.py RESULTS ANNOTATIONS --out FILE [-B N] [--jobs N]
       [--inputs ref,R2,...]
Exit code 0 on success, 2 when the shards or the annotations are
missing.
"""

import argparse
import contextlib
import io
import json
import sys
from multiprocessing import Pool
from pathlib import Path

import eval_map as em
import numpy as np
from pycocotools.cocoeval import COCOeval

MODELS = ("yolov8n", "yolov5s", "yolov5s_relu", "yolox_s")
DEFAULT_INPUTS = "ref,R2,R30,R30c4"
DEFAULT_SAMPLES = 200
LOW, HIGH = 2.5, 97.5
EXIT_MISSING = 2
_settings: dict = {}


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", type=Path, help="folder of shards")
    parser.add_argument("annotations", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("-B", type=int, default=DEFAULT_SAMPLES)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--inputs", default=DEFAULT_INPUTS)
    return parser.parse_args(argv)


def init(annotations: str, samples: int) -> None:
    """Load the ground truth in a worker."""
    em.init_worker(annotations)
    _settings["samples"] = samples


def task(job: tuple[tuple[str, str], bytes, list[int]]) -> tuple:
    """Return (key, mAP of each resample) for one (model, input)."""
    key, raw, ids = job
    rows = np.frombuffer(raw, dtype=np.float32).reshape(-1, em.ROW)
    gt = em._ground_truth
    out = []
    with contextlib.redirect_stdout(io.StringIO()):
        engine = COCOeval(gt, gt.loadRes(rows), "bbox")
        engine.params.imgIds = ids
        engine.evaluate()
        full = list(engine.evalImgs)
        cats = len(engine.params.catIds)
        areas = len(engine.params.areaRng)
        count = len(ids)
        rng = np.random.default_rng(0)  # the same pictures for every input
        for _ in range(_settings["samples"]):
            pick = rng.integers(0, count, count)
            engine.evalImgs = [
                full[(k * areas + a) * count + i]
                for k in range(cats)
                for a in range(areas)
                for i in pick
            ]
            engine.accumulate()
            engine.summarize()
            out.append(float(engine.stats[0]))
    return key, out


def main(argv: list[str]) -> int:
    """Run the bootstrap, return the exit code."""
    args = parse_args(argv)
    if not args.annotations.is_file() or not list(
        args.results.glob("shard*.jsonl.gz")
    ):
        print("the shards or the annotations are missing")
        return EXIT_MISSING
    inputs = args.inputs.split(",")
    found = em.load_detections(args.results)
    ids = sorted(found.frames)
    jobs = [
        ((m, i), found.rows[(m, i)].tobytes(), ids)
        for m in MODELS
        for i in inputs
    ]
    with Pool(
        args.jobs, initializer=init, initargs=(str(args.annotations), args.B)
    ) as pool:
        samples = dict(pool.map(task, jobs, chunksize=1))
    args.out.write_text(
        json.dumps({f"{m}|{i}": v for (m, i), v in samples.items()}) + "\n"
    )
    scale = em.PERCENT
    print(f"{len(ids)} pictures, {args.B} samples")
    for label in inputs[1:]:
        deltas = {
            m: (
                np.array(samples[(m, label)])
                - np.array(samples[(m, inputs[0])])
            )
            * scale
            for m in MODELS
        }
        deltas["mean"] = np.mean([deltas[m] for m in MODELS], axis=0)
        for name, delta in deltas.items():
            low, high = np.percentile(delta, [LOW, HIGH])
            print(
                f"{label:6} {name:13} {delta.mean():+.2f}"
                f"  [{low:+.2f}, {high:+.2f}]"
            )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
