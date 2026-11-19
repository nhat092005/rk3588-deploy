"""Write the C++ test data of the tracker.

Runs the real Ultralytics ``BYTETracker`` (from the repo ``.venv``),
``lap.lapjv`` and ``KalmanFilterXYAH`` on seeded synthetic data and
prints ``test/vision/tracker/tracker_fixture.h`` to stdout. The C++ port
must reproduce every identifier and box. Needs the ``lap`` package,
which ``ai/pyproject.toml`` pins (``make sync``) and Ultralytics loads
for the tracker.
Usage: make_tracker_fixture.py > out.h
"""

import sys
from collections import Counter
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from typing import NamedTuple

import lap
import numpy as np
import yaml
from ultralytics.trackers import byte_tracker
from ultralytics.trackers.basetrack import TrackState
from ultralytics.trackers.utils import matching, stracks
from ultralytics.trackers.utils.kalman_filter import KalmanFilterXYAH
from ultralytics.utils import ops

WIDTH = 1920
HEIGHT = 1080
PERSON = 4
SEED = 20261115
# Each branch must run at least once, or the data proves nothing.
REQUIRED_EVENTS = (
    "second_match",
    "unconfirmed_match",
    "unconfirmed_removed",
    "removed_after_buffer",
    "refound",
    "revived_from_removed",
    "duplicate_dropped",
    "empty_frame",
    "low_score_box",
    "degenerate_box",
)
EVENTS: Counter[str] = Counter()


def number(value: float) -> str:
    """Return the shortest float32 literal that round-trips."""
    text = np.format_float_positional(np.float32(value), unique=True, trim="0")
    return f"{text}F"


def real(value: float) -> str:
    """Return a double literal that parses back to the same value."""
    return f"{float(value)!r}"


class Detections:
    """What ``BYTETracker.update`` reads from a Results object."""

    def __init__(
        self, xyxy: np.ndarray, conf: np.ndarray, cls: np.ndarray
    ) -> None:
        """Keep float32 arrays, derive ``xywh`` like ``Boxes.xywh``."""
        self.xyxy = np.asarray(xyxy, dtype=np.float32).reshape(-1, 4)
        self.conf = np.asarray(conf, dtype=np.float32).reshape(-1)
        self.cls = np.asarray(cls, dtype=np.float32).reshape(-1)
        self.xywh = ops.xyxy2xywh(self.xyxy)

    def __len__(self) -> int:
        """Return the number of boxes."""
        return len(self.conf)

    def __getitem__(self, mask: np.ndarray) -> "Detections":
        """Return the boxes selected by a boolean mask."""
        return Detections(self.xyxy[mask], self.conf[mask], self.cls[mask])


def instrument() -> None:
    """Count the branches of the tracker that the data must reach."""
    original_assignment = matching.linear_assignment

    def assignment(cost: np.ndarray, thresh: float, *args: object) -> tuple:
        result = original_assignment(cost, thresh, *args)
        if len(result[0]) and thresh == 0.5:
            EVENTS["second_match"] += 1
        if len(result[0]) and thresh == 0.7:
            EVENTS["unconfirmed_match"] += 1
        return result

    matching.linear_assignment = assignment

    original_reactivate = byte_tracker.STrack.re_activate

    def reactivate(self: object, *args: object, **kwargs: object) -> None:
        EVENTS["refound"] += 1
        if self.state == TrackState.Removed:
            EVENTS["revived_from_removed"] += 1
        original_reactivate(self, *args, **kwargs)

    byte_tracker.STrack.re_activate = reactivate

    original_duplicates = stracks.remove_duplicate_stracks

    def duplicates(
        first: list, second: list, *args: object
    ) -> tuple[list, list]:
        kept = original_duplicates(first, second, *args)
        if len(kept[0]) != len(first) or len(kept[1]) != len(second):
            EVENTS["duplicate_dropped"] += 1
        return kept

    stracks.remove_duplicate_stracks = duplicates


def new_tracker() -> byte_tracker.BYTETracker:
    """Return a BYTETracker with the bytetrack.yaml values."""
    path = Path(byte_tracker.__file__).parent.parent / "cfg/trackers"
    args = SimpleNamespace(
        **yaml.safe_load((path / "bytetrack.yaml").read_text())
    )
    return byte_tracker.BYTETracker(args)


def check_dtypes() -> None:
    """Pin the dtypes the cost matrices depend on."""
    sample = Detections(
        np.array([[0, 0, 10, 20]]), np.array([0.5]), np.array([0])
    )
    joined = np.concatenate(
        [sample.xywh, np.arange(1).reshape(-1, 1)], axis=-1
    )
    assert sample.xywh.dtype == np.float32
    assert sample.conf.dtype == np.float32
    assert joined.dtype == np.float64, joined.dtype  # parse_bboxes widens


def run_scenario(frames: list[Detections], name: str) -> dict:
    """Track a sequence and return inputs and outputs per frame."""
    tracker = new_tracker()
    rows = []
    for detections in frames:
        output = tracker.update(detections)
        if len(detections) == 0:
            EVENTS["empty_frame"] += 1
        for removed in tracker.removed_stracks_frame:
            if removed.is_activated:
                EVENTS["removed_after_buffer"] += 1
            else:
                EVENTS["unconfirmed_removed"] += 1
        rows.append((detections, np.asarray(output).reshape(-1, 8)))
    return {"name": name, "frames": rows}


def box(cx: float, cy: float, w: float, h: float) -> list[float]:
    """Return xyxy of a centre, width, height box."""
    return [cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2]


def make_frame(
    boxes: list[tuple[list[float], float, int]],
) -> Detections:
    """Build a frame, best score first like the post-processing."""
    ordered = sorted(boxes, key=lambda item: -np.float32(item[1]))
    if not ordered:
        return Detections(np.empty((0, 4)), np.empty(0), np.empty(0))
    return Detections(
        np.array([item[0] for item in ordered]),
        np.array([item[1] for item in ordered]),
        np.array([item[2] for item in ordered]),
    )


def new_person(rng: np.random.Generator, frames: int) -> dict:
    """Draw the motion, size and score of one person."""
    return {
        "birth": int(rng.integers(0, 40)),
        "death": int(rng.integers(60, frames + 20)),
        "x": rng.uniform(200, 1700),
        "y": rng.uniform(250, 850),
        "vx": rng.normal(0, 4),
        "vy": rng.normal(0, 2),
        "w": rng.uniform(60, 140),
        "h": rng.uniform(150, 320),
        "base": rng.uniform(0.45, 0.95),
        "cls": int(rng.choice([PERSON, PERSON, PERSON, 0, 3])),
        "gap": (0, 0),
    }


def crowd(rng: np.random.Generator) -> list[Detections]:
    """Many people: crossings, gaps, low scores, false boxes."""
    frames = 100
    people = [new_person(rng, frames) for _ in range(12)]
    # Two people that cross, one that disappears for 12 frames.
    people[0].update(x=300.0, y=500.0, vx=9.0, vy=0.0, birth=0, death=frames)
    people[1].update(x=1500.0, y=520.0, vx=-9.0, vy=0.0, birth=0, death=frames)
    people[2].update(gap=(50, 62), birth=0, death=frames)
    out = []
    for t in range(frames):
        boxes = []
        for p in people:
            if (
                not p["birth"] <= t < p["death"]
                or p["gap"][0] <= t < p["gap"][1]
            ):
                continue
            if rng.random() < 0.04:
                continue
            cx = p["x"] + p["vx"] * (t - p["birth"])
            cy = p["y"] + p["vy"] * (t - p["birth"])
            conf = float(np.clip(p["base"] + rng.normal(0, 0.08), 0.26, 0.99))
            if rng.random() < 0.08:
                conf = float(rng.uniform(0.12, 0.24))
            jitter = rng.normal(0, 1.5, 4)
            xyxy = np.array(box(cx, cy, p["w"], p["h"])) + jitter
            xyxy = np.clip(xyxy, 0, [WIDTH, HEIGHT, WIDTH, HEIGHT])
            boxes.append((xyxy.tolist(), conf, p["cls"]))
            if rng.random() < 0.03:
                boxes.append(((xyxy + 6).tolist(), conf * 0.9, p["cls"]))
        if rng.random() < 0.2:
            fx, fy = rng.uniform(100, 1800), rng.uniform(100, 950)
            size = rng.uniform(40, 120)
            boxes.append((box(fx, fy, size, size), rng.uniform(0.1, 0.4), 1))
        out.append(make_frame(boxes))
    return out


def empty_frames() -> list[Detections]:
    """Empty frame 1, a gap of six frames, then the same people."""
    out = []
    for t in range(60):
        if t == 0 or 20 <= t < 26:
            out.append(make_frame([]))
            continue
        out.append(
            make_frame(
                [
                    (box(400 + 3 * t, 500, 110, 260), 0.9, PERSON),
                    (box(1200 - 2 * t, 600, 90, 240), 0.7, PERSON),
                ]
            )
        )
    return out


def revival() -> list[Detections]:
    """Three still people that return after 30, 31 and 32 missed frames.

    Ultralytics drops a track from the lost list one frame after it
    marks it removed, so a person back after 31 missed frames still
    gets the old id.
    """
    returns = {400.0: 40, 960.0: 41, 1500.0: 42}
    out = []
    for t in range(70):
        boxes = []
        for x, back in returns.items():
            if t < 10 or back <= t < 56:
                boxes.append((box(x, 540, 100, 250), 0.9, PERSON))
        out.append(make_frame(boxes))
    return out


def degenerate() -> list[Detections]:
    """Scores on the thresholds and boxes without area."""
    low = float(np.float32(0.1))
    out = []
    for t in range(20):
        out.append(
            make_frame(
                [
                    (box(300 + t, 500, 100, 200), 0.25, PERSON),
                    (box(700 + t, 500, 100, 200), 0.2499, PERSON),
                    (box(1100 + t, 500, 100, 200), low, PERSON),
                    (box(1500 + t, 500, 100, 200), 0.6, PERSON),
                    ([900, 800, 900, 900], 0.9, PERSON),
                    ([900, 800, 950, 797], 0.9, PERSON),
                ]
            )
        )
        EVENTS["degenerate_box"] += 2
        EVENTS["low_score_box"] += 2
    return out


def ghost_overlap() -> list[Detections]:
    """Slide the predicted box of a lost person over a standing one."""
    out = []
    for t in range(45):
        boxes = [(box(560, 540, 100, 250), 0.9, PERSON)] if t >= 4 else []
        if t < 15:
            boxes.append((box(300 + 10 * t, 540, 100, 250), 0.9, PERSON))
        out.append(make_frame(boxes))
    return out


def shifted(width: float, overlap: float) -> float:
    """Return the shift that gives two equal boxes this IoU."""
    return width * (1 - overlap) / (1 + overlap)


def sweep(first: float, count: int) -> list[float]:
    """Return `count` overlaps from `first` in steps of 0.02."""
    return [round(first + 0.02 * k, 4) for k in range(count)]


class Sweep(NamedTuple):
    """Timeline of a sweep: when the people appear, jump and return."""

    birth: int
    jump: int
    back: int
    length: int
    scores: tuple[float, float]


def sweep_frames(overlaps: list[float], plan: Sweep) -> list[Detections]:
    """Move each person at `plan.jump` to an IoU `overlap` of before.

    One person per row, so the rows never touch. A person exists from
    `plan.birth`; from `plan.jump` the box is shifted sideways by the
    shift that gives its overlap with the box before, up to `plan.back`
    where it returns to its first place. `plan.scores` is the score
    before and at the jump.
    """
    out = []
    for t in range(plan.length):
        boxes = []
        for row, overlap in enumerate(overlaps):
            if t < plan.birth:
                continue
            centre_y = 30.0 + 50.0 * row
            moved = plan.jump <= t < plan.back
            centre_x = 400.0 + (shifted(100, overlap) if moved else 0.0)
            score = plan.scores[t == plan.jump]
            boxes.append((box(centre_x, centre_y, 100, 40), score, PERSON))
        out.append(make_frame(boxes))
    return out


def second_threshold() -> list[Detections]:
    """Track people whose low score box has IoU 0.40 to 0.60.

    The second association (cost 0.5) takes the pairs above IoU 0.5 and
    leaves the others lost, so a wrong threshold changes who is tracked.
    The person is back at the old place the next frame.
    """
    plan = Sweep(birth=0, jump=12, back=13, length=16, scores=(0.9, 0.18))
    return sweep_frames(sweep(0.40, 11), plan)


def first_threshold() -> list[Detections]:
    """Track people whose next box has IoU 0.12 to 0.32.

    With the score 0.9 fused in, the first association (cost 0.8) takes
    the pairs above IoU 0.222. The others get a new id.
    """
    plan = Sweep(birth=0, jump=8, back=14, length=14, scores=(0.9, 0.9))
    return sweep_frames(sweep(0.12, 11), plan)


def unconfirmed_threshold() -> list[Detections]:
    """Track new people whose next box has IoU 0.25 to 0.45.

    The unconfirmed association (cost 0.7) takes the pairs above IoU
    0.333 with the score 0.9 fused in. The others restart with a new id.
    """
    plan = Sweep(birth=3, jump=4, back=10, length=10, scores=(0.9, 0.9))
    return sweep_frames(sweep(0.25, 11), plan)


def solver_cases(rng: np.random.Generator) -> list[dict]:
    """Random cost matrices and the answer of lap.lapjv."""
    cases = []
    limits = [0.8, 0.5, 0.7]
    shapes = [(0, 3), (3, 0), (0, 0)]
    shapes += [
        (int(rng.integers(1, 9)), int(rng.integers(1, 9))) for _ in range(60)
    ]
    shapes += [(1, 1), (5, 5), (8, 2), (2, 8), (12, 12), (20, 7)]
    for index, (rows, columns) in enumerate(shapes):
        limit = limits[index % 3]
        kind = index % 4
        if kind == 0:
            cost = rng.uniform(0, 1.2, (rows, columns))
        elif kind == 1:  # many ties
            cost = rng.integers(0, 5, (rows, columns)) * 0.25
        elif kind == 2:  # all equal
            cost = np.full((rows, columns), 0.5)
        else:  # IoU like: mostly 1, a few good pairs
            cost = np.where(
                rng.random((rows, columns)) < 0.3,
                rng.uniform(0, 0.9, (rows, columns)),
                1.0,
            )
        cost = cost.astype(np.float32)
        if rows == 0 or columns == 0:
            x = np.full(rows, -1)
            y = np.full(columns, -1)
        else:
            _, x, y = lap.lapjv(cost, extend_cost=True, cost_limit=limit)
        cases.append(
            {"cost": cost, "limit": limit, "x": list(x), "y": list(y)}
        )
    return cases


def kalman_sequences(rng: np.random.Generator) -> list[list[dict]]:
    """Operations of KalmanFilterXYAH with the state after each one."""
    filter_ = KalmanFilterXYAH()
    sequences = []
    for start in (
        [100.0, 200.0, 0.4, 180.0],
        [800.0, 500.0, 1.2, 20.0],
        [5.0, 5.0, 0.3, 400.0],
    ):
        steps = []
        mean, cov = filter_.initiate(np.array(start))
        steps.append(("init", start, mean, cov))
        for i in range(12):
            if i % 3 == 2:
                measurement = np.array(mean[:4]) + rng.normal(0, 2, 4) * [
                    1,
                    1,
                    0.01,
                    1,
                ]
                mean, cov = filter_.update(mean, cov, measurement)
                steps.append(("update", measurement.tolist(), mean, cov))
            else:
                mean, cov = filter_.predict(mean, cov)
                steps.append(("predict", [0.0] * 4, mean, cov))
        sequences.append(steps)
    return sequences


OPERATIONS = {"init": 0, "predict": 1, "update": 2}


def declare(out: list[str], element: str, name: str, count: int) -> None:
    """Open a constexpr std::array of ``count`` elements."""
    out.append(f"inline constexpr std::array<{element}, {count}> {name}{{{{")


def scalars(out: list[str], name: str, values: list[int]) -> None:
    """Write a one line constexpr array of sizes."""
    items = ", ".join(str(v) for v in values)
    out.append(
        f"inline constexpr std::array<std::size_t, {len(values)}> "
        f"{name}{{{items}}};"
    )


def struct(out: list[str], name: str, fields: list[str]) -> None:
    """Write a struct with one field per line."""
    out.append(f"struct {name} {{")
    out.extend(f"    {field};" for field in fields)
    out.append("};")


def emit_solver(out: list[str], cases: list[dict]) -> None:
    """Append the solver cases."""
    costs = [number(v) for case in cases for v in case["cost"].ravel()]
    rows = [int(v) for case in cases for v in case["x"]]
    columns = [int(v) for case in cases for v in case["y"]]
    struct(
        out,
        "SolverCase",
        [
            "std::size_t rows",
            "std::size_t columns",
            "double limit",
            "std::size_t firstCost",
            "std::size_t firstRow",
            "std::size_t firstColumn",
        ],
    )
    out.append("")
    declare(out, "SolverCase", "solverCases", len(cases))
    cost_at = row_at = column_at = 0
    for case in cases:
        r, c = case["cost"].shape
        limit = case["limit"]
        out.append(
            f"    {{{r}, {c}, {limit!r}, {cost_at}, {row_at}, {column_at}}},"
        )
        cost_at += r * c
        row_at += r
        column_at += c
    out.append("}};")
    out.append("")
    declare(out, "float", "solverCosts", max(len(costs), 1))
    out.append("    " + ", ".join(costs))
    out.append("}};")
    declare(out, "int", "solverRows", max(len(rows), 1))
    out.append("    " + ", ".join(str(v) for v in rows))
    out.append("}};")
    declare(out, "int", "solverColumns", max(len(columns), 1))
    out.append("    " + ", ".join(str(v) for v in columns))
    out.append("}};")
    out.append("")


def emit_kalman(out: list[str], sequences: list[list[tuple]]) -> None:
    """Append the Kalman steps."""
    steps = [step for sequence in sequences for step in sequence]
    out.append("inline constexpr std::size_t stateCount{8};")
    out.append("inline constexpr std::size_t covarianceCount{64};")
    out.append("")
    struct(
        out,
        "KalmanStep",
        [
            "int operation /* 0 initiate, 1 predict, 2 update */",
            "std::array<double, 4> measurement",
            "std::array<double, stateCount> mean",
            "std::array<double, covarianceCount> covariance",
        ],
    )
    out.append("")
    scalars(out, "kalmanLengths", [len(s) for s in sequences])
    declare(out, "KalmanStep", "kalmanSteps", len(steps))
    for name, measurement, mean, cov in steps:
        values = ", ".join(real(v) for v in measurement)
        means = ", ".join(real(v) for v in mean)
        covs = ", ".join(real(v) for v in np.asarray(cov).ravel())
        out.append(
            f"    {{{OPERATIONS[name]}, {{{{{values}}}}}, "
            f"{{{{{means}}}}}, {{{{{covs}}}}}}},"
        )
    out.append("}};")
    out.append("")


def emit_tracker(out: list[str], scenarios: list[dict]) -> None:
    """Append the tracker scenarios."""
    inputs, outputs, frames = [], [], []
    for scenario in scenarios:
        for detections, tracks in scenario["frames"]:
            first_in, first_out = len(inputs), len(outputs)
            for xyxy, conf, cls in zip(
                detections.xyxy, detections.conf, detections.cls, strict=True
            ):
                values = ", ".join(number(v) for v in (*xyxy, conf))
                inputs.append(f"    {{{values}, {int(cls)}}},")
            for x1, y1, x2, y2, track, score, cls, idx in tracks:
                values = ", ".join(number(v) for v in (x1, y1, x2, y2, score))
                outputs.append(
                    f"    {{{int(track)}, {values}, {int(cls)}, {int(idx)}}},"
                )
            frames.append(
                f"    {{{first_in}, {len(inputs) - first_in}, {first_out}, "
                f"{len(outputs) - first_out}}},"
            )
    struct(
        out,
        "InputBox",
        [
            "float left",
            "float top",
            "float right",
            "float bottom",
            "float confidence",
            "int classId",
        ],
    )
    struct(
        out,
        "OutputBox",
        [
            "int id",
            "float left",
            "float top",
            "float right",
            "float bottom",
            "float score",
            "int classId",
            "int detectionIndex",
        ],
    )
    struct(
        out,
        "Frame",
        [
            "std::size_t firstInput",
            "std::size_t inputCount",
            "std::size_t firstOutput",
            "std::size_t outputCount",
        ],
    )
    out.append("")
    scalars(out, "scenarioFrames", [len(s["frames"]) for s in scenarios])
    declare(out, "InputBox", "inputs", len(inputs))
    out.extend(inputs)
    out.append("}};")
    declare(out, "OutputBox", "outputs", len(outputs))
    out.extend(outputs)
    out.append("}};")
    declare(out, "Frame", "frames", len(frames))
    out.extend(frames)
    out.append("}};")


def main() -> int:
    """Print the header."""
    rng = np.random.default_rng(SEED)
    check_dtypes()
    instrument()
    solver = solver_cases(rng)
    kalman = kalman_sequences(rng)
    builders: list[tuple[str, Callable[[], list[Detections]]]] = [
        ("crowd", lambda: crowd(rng)),
        ("empty_frames", empty_frames),
        ("revival", revival),
        ("degenerate", degenerate),
        ("ghost_overlap", ghost_overlap),
        ("second_threshold", second_threshold),
        ("first_threshold", first_threshold),
        ("unconfirmed_threshold", unconfirmed_threshold),
    ]
    scenarios = [run_scenario(build(), name) for name, build in builders]
    missing = [e for e in REQUIRED_EVENTS if EVENTS[e] == 0]
    print("events:", dict(EVENTS), file=sys.stderr)
    if missing:
        print(f"branches never reached: {missing}", file=sys.stderr)
        return 1
    out = [
        "// Generated by tools/generate/make_tracker_fixture.py with",
        "// Ultralytics BYTETracker, lap.lapjv and KalmanFilterXYAH.",
        "// Do not edit.",
        "#ifndef TRACKER_FIXTURE_H",
        "#define TRACKER_FIXTURE_H",
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "namespace trackerFixture {",
        "",
    ]
    emit_solver(out, solver)
    emit_kalman(out, kalman)
    emit_tracker(out, scenarios)
    out += [
        "",
        "} // namespace trackerFixture",
        "",
        "#endif // TRACKER_FIXTURE_H",
        "",
    ]
    sys.stdout.write("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
