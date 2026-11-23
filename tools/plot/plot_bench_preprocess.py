"""Draw the figures and tables of bench_preprocess from its JSON files.

Each input is optional: a figure is drawn when its JSON is given.

* ``--source`` (``--sweep source``): latency p50 and CPU ms per frame
  against the source size (figures 1 and 2).
* ``--output`` (``--sweep output``): the same against the output size
  (figures 6 and 7).
* ``--fps-npu`` (``--sweep fps --npu MODEL``): system CPU and latency
  p95 against the FPS asked for, with the NPU running (figures 3, 4).
* ``--fps-rest`` (``--sweep fps``, no NPU): with ``--fps-npu``, the
  table of what the NPU background costs each method.
* ``--diff``: any number of runs, one table row each, for the pixel
  difference per test pattern.

Figure 5 (change of mAP per method) is drawn by ``plot_map.py``.

The figures follow the look of a paper: one column wide (3.5 in), serif
text, a full frame with inward ticks, the legend inside the axes, no
title (the caption goes under the figure, see ``captions.md``). Each is
written as a PNG (300 dpi). ``tables.md`` holds the numbers of
every figure, so no value is read off the colour alone.

Usage: plot_bench_preprocess.py --out DIR [--source F] [--output F]
[--fps-npu F] [--fps-rest F] [--diff F ...]
"""

import argparse
import json
import sys
from dataclasses import dataclass
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.axes import Axes
from matplotlib.lines import Line2D
from matplotlib.ticker import FuncFormatter, NullFormatter

METHODS = ("cv1", "cv8", "R2", "R30", "R31", "Ra")
SCHEMAS = (1, 2)
FOUR_K = (3840, 2160)
LOG_SPAN = 10
FPS_MAX_LABEL = "max"
EXIT_USAGE = 2
FIGURE_SIZE = (3.5, 2.6)
PAIR_SIZE = (7.0, 2.9)
QUAD_SIZE = (7.0, 5.7)

BLACK = "#000000"
GRID = "#d0d0d0"

# Colour, marker and line of each method. Colour alone never tells them
# apart: marker and line differ too, so a grey print still reads. R30,
# R31 and Ra often lie on one another (the same hardware path), so they
# differ in size and fill: a small solid marker on a larger hollow one.
STYLES = {
    "cv1": {"color": "#1f3fd1", "marker": "o", "ls": "-", "ms": 4.5},
    "cv8": {"color": "#e86a00", "marker": "s", "ls": "--", "ms": 4.5},
    "R2": {"color": "#1a9a3c", "marker": "^", "ls": "-", "ms": 5},
    "R30": {"color": "#d91f26", "marker": "v", "ls": "-.", "ms": 4},
    "R31": {
        "color": "#7e3fa6",
        "marker": "D",
        "ls": ":",
        "ms": 6,
        "hollow": 1,
    },
    "Ra": {
        "color": "#222222",
        "marker": "o",
        "ls": "-",
        "ms": 8,
        "hollow": 1,
    },
}
ZORDER = {"cv1": 3, "cv8": 3, "R2": 4, "R30": 7, "R31": 6, "Ra": 5}
LEGEND_NOTE = (
    "Every method turns one NV12 frame (BT.601 full range) into the square"
    " model input: NV12 to RGB, scale to fit keeping the aspect ratio,"
    " rotate by 180 degrees, place in a square padded with the value 114."
    " cv1 and cv8 do it on the CPU with OpenCV (a one-pass colour"
    " conversion, bilinear cv::resize, cv::rotate) on 1 and 8 threads."
    " R2, R30, R31 and Ra use the RGA (two calls: colour conversion, then"
    " scale and rotate): R2 is RGA2, R30 and R31 are RGA3 core 0 and core"
    " 1, Ra leaves the choice of core to the kernel."
)
MARK_NOTE = (
    "A cross at the bottom marks a size the RGA driver rejected, a plus a"
    " setup that failed; no line is drawn there."
)


@dataclass(frozen=True)
class Cell:
    """One method at one point: a value, or why there is none."""

    value: float | None
    status: str
    message: str = ""


@dataclass(frozen=True)
class Axis:
    """The x axis of a figure: ticks, full point names, a title."""

    labels: list[str]
    full: list[str]
    name: str


@dataclass(frozen=True)
class Spec:
    """What a figure shows on the y axis."""

    key: str
    ylabel: str
    log: bool = False


def fmt(value: float) -> str:
    """Return a value with enough digits to tell it from the next."""
    if value >= 10:
        return f"{value:.1f}"
    if value >= 1:
        return f"{value:.2f}"
    return f"{value:.3f}"


def load(path: Path) -> dict:
    """Return a bench_preprocess JSON, checking what it is."""
    with path.open(encoding="utf-8") as file:
        doc = json.load(file)
    if doc.get("stage") != "preprocess" or doc.get("schema") not in SCHEMAS:
        message = f"{path}: not a bench_preprocess JSON of schema 1 or 2"
        raise ValueError(message)
    return doc


def cells(doc: dict, label: str, key: str) -> list[Cell]:
    """Return the cell of one method at every point, in point order."""
    out = []
    for point in doc["points"]:
        method = next(m for m in point["methods"] if m["label"] == label)
        status = method["status"]
        value = method.get(key) if status == "ok" else None
        out.append(Cell(value, status, method.get("message", "")))
    return out


def source_axis(doc: dict) -> Axis:
    """Return the x axis of the source sweep (16:9 frames, by width)."""
    points = doc["points"]
    return Axis(
        [str(p["source_width"]) for p in points],
        [f"{p['source_width']}x{p['source_height']}" for p in points],
        "Source width (px, 16:9)",
    )


def output_axis(doc: dict) -> Axis:
    """Return the x axis of the output sweep."""
    labels = [str(p["target"]) for p in doc["points"]]
    return Axis(labels, labels, "Output size (px, square)")


def fps_axis(doc: dict) -> Axis:
    """Return the x axis of the FPS sweep."""
    labels = [
        FPS_MAX_LABEL if p["fps"] == 0 else str(p["fps"])
        for p in doc["points"]
    ]
    return Axis(labels, labels, "Frame rate asked for (FPS)")


def runs(cell_list: list[Cell]) -> list[list[int]]:
    """Return the runs of consecutive indices that have a value."""
    out: list[list[int]] = []
    for i, cell in enumerate(cell_list):
        if cell.value is None:
            continue
        if out and out[-1][-1] == i - 1:
            out[-1].append(i)
        else:
            out.append([i])
    return out


def paper_style() -> None:
    """Set the look of a paper figure: serif, framed, ticks inward."""
    mpl.rcParams.update(
        {
            "font.family": "serif",
            "font.serif": ["STIXGeneral"],
            "mathtext.fontset": "stix",
            "font.size": 9,
            "axes.linewidth": 0.8,
            "axes.labelsize": 9,
            "xtick.direction": "in",
            "ytick.direction": "in",
            "xtick.top": True,
            "ytick.right": True,
            "xtick.labelsize": 8,
            "ytick.labelsize": 8,
            "legend.fontsize": 8,
        },
    )


def line_kwargs(label: str) -> dict:
    """Return the matplotlib arguments of the line of a method."""
    style = STYLES[label]
    colour = style["color"]
    return {
        "color": colour,
        "linestyle": style["ls"],
        "linewidth": 1.3,
        "marker": style["marker"],
        "markersize": style["ms"],
        "markerfacecolor": "none" if style.get("hollow") else colour,
        "markeredgecolor": colour,
        "markeredgewidth": 1.0,
        "zorder": ZORDER[label],
    }


def plot_on(
    ax: Axes,
    axis: Axis,
    spec: Spec,
    data: dict[str, list[Cell]],
    *,
    legend: bool,
) -> None:
    """Plot a line per method on ax, a cross where none ran."""
    values = [c.value for s in data.values() for c in s if c.value]
    low, high = min(values), max(values)
    # A log axis only when the values span more than a decade.
    log = spec.log and high / low > LOG_SPAN
    if log:
        ax.set_yscale("log")
        ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f"{v:g}"))
        ax.yaxis.set_minor_formatter(NullFormatter())
        floor = low / 2.2
        # A third of the height stays free for the legend.
        ax.set_ylim(floor, high * (high / floor) ** 0.28)
    else:
        floor = 0.0
        ax.set_ylim(0, high * 1.32)
    for label in METHODS:
        series = data[label]
        for run in runs(series):
            ax.plot(run, [series[i].value for i in run], **line_kwargs(label))
        shift = (METHODS.index(label) - (len(METHODS) - 1) / 2) * 0.09
        for i, cell in enumerate(series):
            if cell.value is None:
                ax.plot(
                    i + shift,
                    floor * 1.3 if log else high * 0.05,
                    marker="x" if cell.status == "rejected" else "+",
                    color=STYLES[label]["color"],
                    markersize=5,
                    markeredgewidth=1.2,
                    linestyle="",
                    zorder=8,
                )
    if legend:
        handles = [
            Line2D([0], [0], label=label, **line_kwargs(label))
            for label in METHODS
        ]
        ax.legend(
            handles=handles,
            loc="upper left",
            ncol=3,
            frameon=False,
            handlelength=2.4,
            columnspacing=1.0,
            labelspacing=0.3,
            borderaxespad=0.3,
        )
    ax.set_xticks(range(len(axis.labels)), axis.labels)
    ax.set_xlim(-0.35, len(axis.labels) - 0.65)
    ax.set_xlabel(axis.name)
    ax.set_ylabel(spec.ylabel + (", log scale" if log else ""))
    ax.grid(color=GRID, linewidth=0.5, linestyle="--", which="major")
    ax.set_axisbelow(True)


def draw(
    path: Path, axis: Axis, spec: Spec, data: dict[str, list[Cell]]
) -> None:
    """Draw one figure."""
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    plot_on(ax, axis, spec, data, legend=True)
    fig.tight_layout(pad=0.4)
    fig.savefig(path.with_suffix(".png"), dpi=300)
    plt.close(fig)


@dataclass(frozen=True)
class Panel:
    """One plot of a multi-panel image: axis, y spec, data, caption."""

    axis: Axis
    spec: Spec
    data: dict[str, list[Cell]]
    text: str


def draw_panels(
    path: Path,
    shape: tuple[int, int],
    size: tuple[float, float],
    panels: list[Panel],
) -> None:
    """Draw panels in a grid: legend in (a) only, labels (a), (b)..."""
    fig, axes = plt.subplots(*shape, figsize=size, squeeze=False)
    for i, (ax, panel) in enumerate(zip(axes.flat, panels, strict=True)):
        plot_on(ax, panel.axis, panel.spec, panel.data, legend=i == 0)
        # Just under the x label, so the caption belongs to its panel;
        # h_pad keeps the rows apart.
        ax.annotate(
            f"({'abcdefgh'[i]}) {panel.text}",
            xy=(0.5, 0.0),
            xycoords="axes fraction",
            xytext=(0, -34),
            textcoords="offset points",
            ha="center",
            va="top",
        )
    fig.tight_layout(pad=0.4, w_pad=1.2, h_pad=2.4)
    fig.savefig(path.with_suffix(".png"), dpi=300)
    plt.close(fig)


def md_table(header: list[str], rows: list[list[str]]) -> str:
    """Return a Markdown table."""
    lines = [
        "| " + " | ".join(header) + " |",
        "|" + "---|" * len(header),
    ]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(lines) + "\n"


def number(cell: Cell) -> str:
    """Return a value for a table, or what happened instead."""
    if cell.value is not None:
        return fmt(cell.value)
    return cell.status


def values_table(
    axis: Axis,
    data: dict[str, list[Cell]],
) -> str:
    """Return the numbers of a figure, a row per x."""
    rows = [
        [label, *(number(data[m][i]) for m in METHODS)]
        for i, label in enumerate(axis.full)
    ]
    return md_table([axis.name, *METHODS], rows)


def refusal_table(docs: list[tuple[str, dict]]) -> str:
    """Return the calls that did not run, with their message."""
    rows = []
    for name, doc in docs:
        axis = (
            source_axis(doc) if doc["sweep"] == "source" else output_axis(doc)
        )
        for label in METHODS:
            for i, cell in enumerate(cells(doc, label, "p50_ms")):
                if cell.status != "ok":
                    rows.append(
                        [
                            name,
                            axis.full[i],
                            label,
                            cell.status,
                            cell.message,
                        ],
                    )
    if not rows:
        return "No call was rejected or failed.\n"
    return md_table(["sweep", "point", "method", "status", "message"], rows)


def npu_table(rest: dict, npu: dict) -> str:
    """Return the change of p50 with the NPU running, in percent."""
    axis = fps_axis(rest)
    rows = []
    for i, label in enumerate(axis.full):
        row = [label]
        for method in METHODS:
            base = cells(rest, method, "p50_ms")[i].value
            busy = cells(npu, method, "p50_ms")[i].value
            row.append(
                "-"
                if base is None or busy is None
                else f"{(busy / base - 1) * 100:+.1f}",
            )
        rows.append(row)
    return md_table(["FPS asked for", *METHODS], rows)


def diff_table(docs: list[Path]) -> str:
    """Return the pixel difference to cv1 per pattern, at 4K."""
    rows = []
    for path in docs:
        doc = load(path)
        point = next(
            (
                p
                for p in doc["points"]
                if (p["source_width"], p["source_height"]) == FOUR_K
                and p["target"] == 640
            ),
            None,
        )
        if point is None:
            continue
        row = [doc["pattern"]]
        for method in METHODS:
            found = next(m for m in point["methods"] if m["label"] == method)
            if found["status"] != "ok" or "diff_mean" not in found:
                row.append("reference" if found["status"] == "ok" else "-")
                continue
            row.append(
                f"{found['diff_mean']:.2f} / {found['diff_max']:.0f}"
                f" / {found['diff_over_16_percent']:.2f}%",
            )
        rows.append(row)
    header = ["pattern", *METHODS]
    note = (
        "Pixel difference to cv1 at 3840x2160 into 640x640: mean / largest"
        " / share of values that differ by more than 16.\n\n"
    )
    return note + md_table(header, rows)


@dataclass(frozen=True)
class Figure:
    """A figure to draw: file stem, y axis, table title, caption."""

    stem: str
    spec: Spec
    title: str
    caption: str
    # Short name under the panel when two figures share one image.
    panel: str


def draw_figures(
    out: Path,
    doc: dict,
    axis: Axis,
    figures: tuple[Figure, ...],
    report: tuple[list[str], list[str]],
) -> list[Panel]:
    """Draw the figures of one JSON; return their panels."""
    sections, captions = report
    panels = []
    statuses: set[str] = set()
    for figure in figures:
        data = {m: cells(doc, m, figure.spec.key) for m in METHODS}
        panels.append(Panel(axis, figure.spec, data, figure.panel))
        draw(out / figure.stem, axis, figure.spec, data)
        sections.append(f"## {figure.title}\n\n{values_table(axis, data)}")
        statuses |= {c.status for s in data.values() for c in s}
        captions.append(
            f"**{figure.stem}**: {figure.caption} {note(statuses)}\n"
        )
    return panels


def statuses_of(panels: list[Panel]) -> set[str]:
    """Return every status that appears in the data of the panels."""
    return {c.status for p in panels for s in p.data.values() for c in s}


def note(statuses: set[str]) -> str:
    """Return the caption note on the methods and any crosses."""
    return LEGEND_NOTE + (f" {MARK_NOTE}" if statuses - {"ok"} else "")


def sweep_figures(
    doc: dict,
    names: tuple[str, str],
    stems: tuple[str, str],
    where: str,
    short: str,
) -> tuple[Figure, ...]:
    """Return the latency and the CPU figure of one sweep."""
    background = f"{doc['runs']} runs per point, NPU idle, log scale"
    return (
        Figure(
            stems[0],
            Spec("p50_ms", "Latency p50 (ms)", log=True),
            names[0],
            f"Latency p50 of the preprocess stage by {where} ({background}).",
            f"Latency p50 by {short}",
        ),
        Figure(
            stems[1],
            Spec("cpu_ms_per_frame", "CPU time per frame (ms)", log=True),
            names[1],
            f"CPU time per frame of the preprocess stage by {where}"
            f" ({background}).",
            f"CPU time per frame by {short}",
        ),
    )


def fps_figures(doc: dict) -> tuple[Figure, ...]:
    """Return the p95 latency and system CPU figure of the FPS sweep."""
    npu = doc["background"] == "npu"
    state = "the NPU model running meanwhile" if npu else "NPU idle"
    where = (
        f"frame rate asked for (3840x2160 into 640x640, {doc['seconds']} s"
        f" per method, {state})"
    )
    return (
        Figure(
            "fig4_p95_fps",
            Spec("p95_ms", "Latency p95 (ms)"),
            f"Latency p95 by frame rate ({state})",
            f"Latency p95 of the preprocess stage by {where}.",
            "Latency p95",
        ),
        Figure(
            "fig3_system_cpu_fps",
            Spec("system_cpu_percent", "System CPU, 8 cores (%)"),
            f"System CPU by frame rate ({state})",
            f"Share of all 8 cores that was busy, by {where}.",
            "System CPU",
        ),
    )


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--fps-npu", type=Path)
    parser.add_argument("--fps-rest", type=Path)
    parser.add_argument("--diff", type=Path, nargs="*", default=[])
    return parser.parse_args()


def main() -> int:
    """Draw what the files allow, write tables.md and captions.md."""
    args = parse_args()
    given = [args.source, args.output, args.fps_npu, args.diff]
    if not any(given):
        print("nothing to draw: give at least one JSON", file=sys.stderr)
        return EXIT_USAGE
    args.out.mkdir(parents=True, exist_ok=True)
    paper_style()
    sections: list[str] = []
    captions: list[str] = []
    report = (sections, captions)
    sweeps: list[tuple[str, dict]] = []
    by_sweep: dict[str, list[Panel]] = {}
    if args.source:
        doc = load(args.source)
        sweeps.append(("source", doc))
        figures = sweep_figures(
            doc,
            (
                "Latency p50 by source size",
                "CPU time per frame by source size",
            ),
            ("fig1_latency_source", "fig2_cpu_source"),
            "source frame size (16:9, output 640x640)",
            "source size",
        )
        by_sweep["source"] = draw_figures(
            args.out, doc, source_axis(doc), figures, report
        )
        runs_per_point = doc["runs"]
    if args.output:
        doc = load(args.output)
        sweeps.append(("output", doc))
        figures = sweep_figures(
            doc,
            (
                "Latency p50 by output size",
                "CPU time per frame by output size",
            ),
            ("fig6_latency_output", "fig7_cpu_output"),
            "output size (source 3840x2160)",
            "output size",
        )
        by_sweep["output"] = draw_figures(
            args.out, doc, output_axis(doc), figures, report
        )
        runs_per_point = doc["runs"]
    if len(by_sweep) == 2:
        panels = by_sweep["source"] + by_sweep["output"]
        draw_panels(args.out / "quad_size", (2, 2), QUAD_SIZE, panels)
        captions.append(
            "**quad_size**: The preprocess stage by source frame size"
            " (a, b; 16:9, output 640x640) and by output size (c, d; source"
            f" 3840x2160). Left: latency p50; right: CPU time per frame;"
            f" {runs_per_point} runs per point, NPU idle, log scale where"
            f" the axis says so. {note(statuses_of(panels))}\n",
        )
    else:
        for stem, panels in zip(
            ("pair_a_source", "pair_b_output"), by_sweep.values(), strict=False
        ):
            draw_panels(args.out / stem, (1, 2), PAIR_SIZE, panels)
    if args.fps_npu:
        npu = load(args.fps_npu)
        panels = draw_figures(
            args.out, npu, fps_axis(npu), fps_figures(npu), report
        )
        draw_panels(args.out / "pair_c_fps", (1, 2), PAIR_SIZE, panels)
        captions.append(
            "**pair_c_fps**: (a) latency p95 and (b) share of all 8 cores"
            f" that was busy, by frame rate asked for (3840x2160 into"
            f" 640x640, {npu['seconds']} s per method, the NPU model running"
            f" meanwhile). {note(statuses_of(panels))}\n",
        )
        if args.fps_rest:
            rest = load(args.fps_rest)
            sections.append(
                "## Latency p50 with the NPU running, against rest (%)\n\n"
                + npu_table(rest, npu),
            )
        alone = npu.get("npu_alone_p50_ms")
        if alone is not None:
            sections.append(
                f"The NPU model alone: p50 {alone:.1f} ms over"
                f" {npu['npu_alone_runs']} runs.\n",
            )
    if sweeps:
        sections.append(f"## Calls not run\n\n{refusal_table(sweeps)}")
    if args.diff:
        sections.append(f"## Output difference\n\n{diff_table(args.diff)}")
    (args.out / "tables.md").write_text("\n".join(sections), encoding="utf-8")
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
