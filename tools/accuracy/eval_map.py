"""COCO mAP of every (model, preprocess method) from run_map_board.py.

Reads the ``shard*.jsonl.gz`` of the board run and the COCO annotations
(``instances_val2017.json``), evaluates each model on each input (the
reference and the six methods) over the frames that were run, and over
two groups of them: the pictures with the long side under 640 (the
preprocess scales them up) and the others (long side 640, no scaling),
as ``sizes.json`` of ``make_coco_nv12.py`` tells.

Only ``stats[0]`` (mAP@0.5:0.95) and ``stats[1]`` (mAP@0.5) of COCOeval
are read. Writes ``map.json`` and ``map.md`` (mAP and the difference to
the reference of the same model) to ``--out``.

Needs pycocotools.

Usage: eval_map.py RESULTS ANNOTATIONS FRAMES --out DIR [--jobs N]
Exit code 0 on success, 2 on a bad command line or missing data.
"""

import argparse
import contextlib
import gzip
import hashlib
import io
import json
import os
import sys
from array import array
from multiprocessing import Pool
from pathlib import Path

import numpy as np
from pycocotools.coco import COCO
from pycocotools.cocoeval import COCOeval

REFERENCE = "ref"
INPUTS = ("ref", "cv1", "cv8", "R2", "R30", "R31", "Ra")
GROUPS = ("all", "upscaled", "unscaled")
IMGSZ = 640
EXIT_MISSING = 2
ROW = 7  # image id, x, y, width, height, score, category
DEFAULT_JOBS = 3  # each worker holds the ground truth and one input
PERCENT = 100.0
MAP_5095 = 0
MAP_50 = 1

# Shared by the worker processes (set by init_worker).
_ground_truth: COCO | None = None


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse and check the command line. Exit code 2 on bad input."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("results", type=Path, help="folder of shard*.jsonl.gz")
    parser.add_argument(
        "annotations", type=Path, help="instances_val2017.json"
    )
    parser.add_argument("frames", type=Path, help="folder with sizes.json")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--jobs",
        type=int,
        default=min(DEFAULT_JOBS, os.cpu_count() or 1),
        help="worker processes, about 1 GB each",
    )
    args = parser.parse_args(argv)
    if not list(args.results.glob("shard*.jsonl.gz")):
        parser.exit(EXIT_MISSING, f"{args.results}: no shard*.jsonl.gz\n")
    return args


class Detections:
    """What the shards hold: the rows of each (model, input) as arrays.

    A row is [image id, x, y, width, height, score, category] in float32
    (the format ``COCO.loadRes`` takes), so 5000 frames of 28 inputs
    stay below half a gigabyte; the lines themselves are not kept.
    """

    def __init__(self) -> None:
        """Start with nothing."""
        self.rows: dict[tuple[str, str], array] = {}
        self.seen: dict[tuple[str, str], set[int]] = {}
        self.frames: set[int] = set()
        self.missing: dict[int, set[str]] = {}

    def add(self, line: dict) -> None:
        """Add a frame; a repeated frame of an input is skipped."""
        image_id = line["image_id"]
        self.frames.add(image_id)
        self.missing.setdefault(image_id, set()).update(line["missing"])
        for model, inputs in line["results"].items():
            for label, boxes in inputs.items():
                key = (model, label)
                done = self.seen.setdefault(key, set())
                if image_id in done:
                    continue
                done.add(image_id)
                rows = self.rows.setdefault(key, array("f"))
                for x, y, w, h, score, category in boxes:
                    rows.extend((image_id, x, y, w, h, score, category))


def load_detections(folder: Path) -> Detections:
    """Read the shards one line at a time."""
    found = Detections()
    for path in sorted(folder.glob("shard*.jsonl.gz")):
        with gzip.open(path, "rt") as source:
            for text in source:
                found.add(json.loads(text))
    return found


def init_worker(annotations: str) -> None:
    """Load the ground truth once per worker process."""
    global _ground_truth  # noqa: PLW0603
    with contextlib.redirect_stdout(io.StringIO()):
        _ground_truth = COCO(annotations)


def evaluate(job: tuple[bytes, dict[str, list[int]]]) -> dict[str, tuple]:
    """Return {group: (mAP@0.5:0.95, mAP@0.5)} of one set of rows."""
    raw, members = job
    assert _ground_truth is not None
    rows = np.frombuffer(raw, dtype=np.float32).reshape(-1, ROW)
    out = {}
    with contextlib.redirect_stdout(io.StringIO()):
        for group, image_ids in members.items():
            chosen = rows[np.isin(rows[:, 0], image_ids)]
            if not len(chosen):
                out[group] = (0.0, 0.0)
                continue
            found = _ground_truth.loadRes(chosen)
            engine = COCOeval(_ground_truth, found, "bbox")
            engine.params.imgIds = image_ids
            engine.evaluate()
            engine.accumulate()
            engine.summarize()
            out[group] = (
                float(engine.stats[MAP_5095]),
                float(engine.stats[MAP_50]),
            )
    return out


def group_of(size: list[int]) -> str:
    """Return "upscaled" if the picture is scaled up, else not."""
    return "upscaled" if max(size[0], size[1]) < IMGSZ else "unscaled"


def members_of(frames: set[int], sizes: dict) -> dict[str, list[int]]:
    """Return the image ids of each group."""
    members: dict[str, list[int]] = {name: [] for name in GROUPS}
    for image_id in sorted(frames):
        members["all"].append(image_id)
        members[group_of(sizes[str(image_id)])].append(image_id)
    return {name: ids for name, ids in members.items() if ids}


def tables(scores: dict, counts: dict, missing: dict) -> str:
    """Return the markdown tables: mAP, then the difference to ref."""
    out = []
    for group in GROUPS:
        if group not in counts:
            continue
        for metric, name in ((MAP_5095, "mAP@0.5:0.95"), (MAP_50, "mAP@0.5")):
            out.append(f"### {name}, {group} ({counts[group]} frames)\n")
            out.append("| model | " + " | ".join(INPUTS) + " |")
            out.append("|---|" + "---|" * len(INPUTS))
            for model in sorted(scores):
                row = scores[model]
                cells = []
                for label in INPUTS:
                    if label not in row:
                        cells.append("-")
                        continue
                    value = row[label][group][metric] * PERCENT
                    delta = value - row[REFERENCE][group][metric] * PERCENT
                    cells.append(
                        f"{value:.2f}"
                        if label == REFERENCE
                        else f"{value:.2f} ({delta:+.2f})"
                    )
                out.append(f"| {model} | " + " | ".join(cells) + " |")
            out.append("")
    if any(missing.values()):
        out.append(
            "Frames left out for every input because the RGA refused them:"
            f" {missing}\n"
        )
    return "\n".join(out)


def main(argv: list[str]) -> int:
    """Evaluate every model and input, return the exit code."""
    args = parse_args(argv)
    sizes_path = args.frames / "sizes.json"
    if not sizes_path.is_file() or not args.annotations.is_file():
        print("sizes.json or the annotations are missing")
        return EXIT_MISSING
    sizes = json.loads(sizes_path.read_text())
    found = load_detections(args.results)
    # A frame that the RGA refused is left out for every input, so the
    # inputs are compared on the same frames.
    refused = {i for i, labels in found.missing.items() if labels}
    members = members_of(found.frames - refused, sizes)
    counts = {name: len(ids) for name, ids in members.items()}
    # Inputs with the very same rows (cv1 and cv8, R30 R31 and Ra) are
    # evaluated once.
    unique: dict[str, tuple[str, str]] = {}
    twin: dict[tuple[str, str], str] = {}
    for key, rows in sorted(found.rows.items()):
        digest = hashlib.sha1(rows.tobytes()).hexdigest()
        unique.setdefault(digest, key)
        twin[key] = digest
    print(
        f"{len(found.frames)} frames, {len(refused)} left out (refused by"
        f" the RGA: {sorted(refused)}), {len(unique)} distinct evaluations"
    )
    jobs = [(found.rows[key].tobytes(), members) for key in unique.values()]
    with Pool(
        args.jobs, initializer=init_worker, initargs=(str(args.annotations),)
    ) as pool:
        results = pool.map(evaluate, jobs, chunksize=1)
    done = dict(zip(unique, results, strict=True))
    scores: dict[str, dict] = {}
    for (model, label), digest in twin.items():
        slot = scores.setdefault(model, {}).setdefault(label, {})
        for group, (map5095, map50) in done[digest].items():
            slot[group] = {MAP_5095: map5095, MAP_50: map50}
    missing = dict.fromkeys(INPUTS, 0)
    for labels in found.missing.values():
        for label in labels:
            missing[label] += 1
    args.out.mkdir(parents=True, exist_ok=True)
    named = {
        model: {
            label: {
                group: {"map5095": v[MAP_5095], "map50": v[MAP_50]}
                for group, v in groups.items()
            }
            for label, groups in row.items()
        }
        for model, row in scores.items()
    }
    (args.out / "map.json").write_text(
        json.dumps(
            {
                "frames": counts,
                "missing": missing,
                "left_out": sorted(refused),
                "scores": named,
            },
            indent=1,
        )
        + "\n"
    )
    (args.out / "map.md").write_text(tables(scores, counts, missing))
    print((args.out / "map.md").read_text())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
