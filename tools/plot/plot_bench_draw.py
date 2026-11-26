"""Draw the figures and tables of bench_draw from its JSON files.

Each input is optional: figures are drawn for the JSON that is given.

* ``--boxes`` (``--sweep boxes``): latency p50 (figure 1) and CPU ms per
  frame (figure 2) against the number of boxes, on one frame size.
* ``--source`` (``--sweep source``): the same against the frame size
  (figures 3 and 4), with the same number of boxes.

The figures follow the look of the preprocess ones
(``plot_bench_preprocess.py``): one column wide (3.5 in), serif text, a
full frame with inward ticks, the legend inside the axes, no title (the
caption goes under the figure, see ``captions.md``). Each is written as
a PNG (300 dpi). ``tables.md`` holds the numbers of every figure.

Usage: plot_bench_draw.py --out DIR [--boxes F] [--source F]
"""

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter, NullFormatter

METHODS = ("R2", "cv1", "cv8")
SCHEMA = 1
EXIT_USAGE = 2
FIGURE_SIZE = (3.5, 2.6)
BLACK = "#000000"
GRID = "#d0d0d0"
STYLES = {
    "cv1": {"color": "#1f3fd1", "marker": "o", "ls": "-", "ms": 4.5},
    "cv8": {"color": "#e86a00", "marker": "s", "ls": "--", "ms": 4.5},
    "R2": {"color": "#1a9a3c", "marker": "^", "ls": "-", "ms": 5},
}
NOTE = (
    "Every method draws the same box outlines (4 pixels wide, inside the"
    " box, one colour per class) on one NV12 frame in place. R2 is the"
    " RGA2 as the application calls it (12 boxes per job). cv1 and cv8"
    " fill the Y plane and the Cb Cr plane through OpenCV on 1 and 8"
    " threads (cv8: one band of rows per task). The three frames are equal"
    " byte for byte. A fresh frame is put back by an RGA copy before every"
    " run, as a new camera frame arrives (the copy is not timed)."
)


@dataclass(frozen=True)
class Spec:
    """What a figure shows on the y axis."""

    key: str
    ylabel: str


LATENCY = Spec("p50_ms", "Latency p50 (ms)")
CPU = Spec("cpu_ms_per_frame", "CPU time per frame (ms)")


def fmt(value: float) -> str:
    """Return a value with enough digits to tell it from the next."""
    if value >= 10:
        return f"{value:.1f}"
    if value >= 1:
        return f"{value:.2f}"
    return f"{value:.3f}"


def load(path: Path) -> dict:
    """Return a bench_draw JSON, checking what it is."""
    with path.open(encoding="utf-8") as file:
        doc = json.load(file)
    if doc.get("stage") != "draw" or doc.get("schema") != SCHEMA:
        message = f"{path}: not a bench_draw JSON of schema {SCHEMA}"
        raise ValueError(message)
    return doc


def values(doc: dict, label: str, key: str) -> list[float | None]:
    """Return a method value at every point, None when it has none."""
    out: list[float | None] = []
    for point in doc["points"]:
        method = next(m for m in point["methods"] if m["label"] == label)
        out.append(method.get(key) if method["status"] == "ok" else None)
    return out


def style_axes(ax: plt.Axes) -> None:
    """Give an axes the paper look."""
    for side in ax.spines.values():
        side.set_color(BLACK)
        side.set_linewidth(0.8)
    ax.tick_params(direction="in", top=True, right=True, labelsize=8)
    ax.grid(color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)


def draw(
    xs: list[float],
    ticks: dict[str, list],
    doc: dict,
    spec: Spec,
    path: Path,
) -> None:
    """Draw one figure: a line per method against xs."""
    plt.rcParams["font.family"] = "serif"
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for label in METHODS:
        ys = values(doc, label, spec.key)
        pairs = [(x, y) for x, y in zip(xs, ys, strict=True) if y is not None]
        if not pairs:
            continue
        style = STYLES[label]
        ax.plot(
            [p[0] for p in pairs],
            [p[1] for p in pairs],
            color=style["color"],
            marker=style["marker"],
            linestyle=style["ls"],
            markersize=style["ms"],
            linewidth=1.2,
            label=label,
        )
    if ticks["log"]:
        ax.set_xscale("log", base=2)
        ax.xaxis.set_minor_formatter(NullFormatter())
        ax.xaxis.set_major_formatter(
            FuncFormatter(lambda v, _: f"{v:g}"),
        )
    ax.set_xticks(ticks["at"])
    ax.set_xticklabels(ticks["names"], rotation=ticks["rotation"])
    ax.set_xlabel(ticks["xlabel"], fontsize=9)
    ax.set_ylabel(spec.ylabel, fontsize=9)
    ax.set_ylim(bottom=0)
    ax.legend(loc="upper left", fontsize=8, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def box_axis(doc: dict) -> tuple[list[float], dict]:
    """Return x values and ticks of the box-count sweep."""
    xs = [float(p["boxes"]) for p in doc["points"]]
    ticks = {
        "log": True,
        "at": xs,
        "names": [f"{x:g}" for x in xs],
        "rotation": 0,
        "xlabel": "Boxes per frame",
    }
    return xs, ticks


def source_axis(doc: dict) -> tuple[list[float], dict]:
    """Return x values and ticks of the frame-size sweep."""
    points = doc["points"]
    xs = [float(i) for i in range(len(points))]
    names = [f"{p['width']}x{p['height']}" for p in points]
    ticks = {
        "log": False,
        "at": xs,
        "names": names,
        "rotation": 30,
        "xlabel": "Frame size (pixels)",
    }
    return xs, ticks


def table(doc: dict, title: str, names: list[str]) -> list[str]:
    """Return markdown lines of latency and CPU per method and point."""
    lines = [f"### {title}", ""]
    lines.append(
        "| Point | "
        + " | ".join(f"{m} p50 ms | {m} CPU ms" for m in METHODS)
        + " |"
    )
    lines.append("|" + "---|" * (1 + 2 * len(METHODS)))
    for index, name in enumerate(names):
        cells = []
        for label in METHODS:
            for key in (LATENCY.key, CPU.key):
                value = values(doc, label, key)[index]
                cells.append("-" if value is None else fmt(value))
        lines.append(f"| {name} | " + " | ".join(cells) + " |")
    lines.append("")
    return lines


def state(doc: dict) -> str:
    """Return the clock state of a run as a sentence."""
    locked = (
        doc["frequency_before"]["locked"] and doc["frequency_after"]["locked"]
    )
    return (
        f"{doc['runs']} runs after {doc['warmup']} warmup,"
        f" heap {doc['heap']}, {doc.get('frame', 'reused')} frame,"
        " frequencies "
        + ("locked" if locked else "NOT locked")
        + f", OpenCV {doc['opencv']}."
    )


def main() -> int:
    """Draw the figures that the given JSON files allow."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--boxes", type=Path)
    parser.add_argument("--source", type=Path)
    args = parser.parse_args()
    if args.boxes is None and args.source is None:
        sys.stderr.write("give --boxes or --source\n")
        return EXIT_USAGE
    args.out.mkdir(parents=True, exist_ok=True)
    tables: list[str] = ["# Tables of bench_draw", ""]
    captions: list[str] = ["# Captions of bench_draw", ""]
    if args.boxes is not None:
        doc = load(args.boxes)
        xs, ticks = box_axis(doc)
        draw(xs, ticks, doc, LATENCY, args.out / "fig1_draw_latency_boxes.png")
        draw(xs, ticks, doc, CPU, args.out / "fig2_draw_cpu_boxes.png")
        tables += table(doc, "Number of boxes", ticks["names"])
        tables += [state(doc), ""]
        captions += [
            f"**Figure 1.** Latency p50 of drawing the boxes against their"
            f" number on a frame of {doc['points'][0]['width']}x"
            f"{doc['points'][0]['height']}. {NOTE} {state(doc)}",
            "",
            f"**Figure 2.** CPU time of the process per frame, for the same"
            f" runs (all threads, waiting not counted). {NOTE}",
            "",
        ]
    if args.source is not None:
        doc = load(args.source)
        xs, ticks = source_axis(doc)
        draw(xs, ticks, doc, LATENCY, args.out / "fig3_draw_latency_size.png")
        draw(xs, ticks, doc, CPU, args.out / "fig4_draw_cpu_size.png")
        tables += table(doc, "Frame size", ticks["names"])
        tables += [state(doc), ""]
        captions += [
            f"**Figure 3.** Latency p50 of drawing {doc['points'][0]['boxes']}"
            f" boxes against the frame size. {NOTE} {state(doc)}",
            "",
            f"**Figure 4.** CPU time of the process per frame, for the same"
            f" runs. {NOTE}",
            "",
        ]
    (args.out / "tables.md").write_text("\n".join(tables), encoding="utf-8")
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
