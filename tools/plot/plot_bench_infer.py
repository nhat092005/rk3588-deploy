"""Draw the figures and tables of the NPU inference bench (stage 5).

Inputs: the JSON of ``bench_infer`` (one model run back to back, one
file per placement of the calling thread) and the per-operator table of
the RKNN runtime (``perf_detail.txt``).

* Figure 1: latency of one inference (p50) by where the calling thread
  runs and how many NPU cores the model uses.
* Figure 2: CPU time of the process per inference, same cells.
* Figure 3: where the time of one inference goes, by operator type, from
  the per-operator table: NPU operators and operators that run on the
  CPU.

The figures follow the look of the other stages
(``plot_bench_jpeg.py``): one column wide (3.5 in), serif text, a full
frame with inward ticks, no title (the caption goes under the figure,
see ``captions.md``), PNG 300 dpi. ``tables.md`` holds the numbers.

Usage: plot_bench_infer.py --out DIR --core7 f.json --core0 f.json
--core1 f.json --free f1.json [f2.json ...] [--c012-core7 f.json]
[--c012-core0 f.json] --perf perf_detail.txt
"""

import argparse
import json
import re
import sys
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt

FIGURE_SIZE = (3.5, 2.6)
BLACK = "#000000"
GRID = "#d0d0d0"
NPU_COLOR = "#1f3fd1"
CPU_COLOR = "#d91f26"
STYLE = {
    "1": ("#1a9a3c", "^", "1 NPU core (auto)"),
    "3": ("#e86a00", "s", "3 NPU cores"),
}
NOTE = (
    "The model is the application default (YOLOv8s INT8, 6 classes,"
    " 960x960 input), run back to back on a constant input from a"
    " dma-buf, 300 timed inferences per point, frequencies locked."
)
RANK_ROW = re.compile(
    r"^(?P<op>\w+)\s+(?P<calls>\d+)\s+(?P<cpu>\d+)\s+(?P<gpu>\d+)\s+"
    r"(?P<npu>\d+)\s+(?P<total>\d+)\s+(?P<ratio>[\d.]+)%"
)
TOP_OPERATORS = 5


def load(path: Path) -> dict:
    """Return the JSON file at ``path``."""
    return json.loads(path.read_text(encoding="utf-8"))


def style_axes(ax: plt.Axes) -> None:
    """Give an axes the paper look."""
    for side in ax.spines.values():
        side.set_color(BLACK)
        side.set_linewidth(0.8)
    ax.tick_params(direction="in", top=True, right=True, labelsize=8)
    ax.grid(color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)


def operators(perf: Path) -> list[dict]:
    """Return the rows of the operator ranking table, in us."""
    rows = []
    for line in perf.read_text(encoding="utf-8").splitlines():
        found = RANK_ROW.match(line.strip())
        if found:
            rows.append(
                {
                    "op": found["op"],
                    "calls": int(found["calls"]),
                    "cpu": int(found["cpu"]),
                    "npu": int(found["npu"]),
                    "total": int(found["total"]),
                }
            )
    return rows


def cells(args: argparse.Namespace) -> list[tuple[str, dict[str, list[dict]]]]:
    """Return the placements: label and files per NPU core count."""
    out = [
        ("Core 7\n(big)", {"1": [load(args.core7)]}),
        ("Core 0\n(little)", {"1": [load(args.core0)]}),
        ("Core 1\n(little)", {"1": [load(args.core1)]}),
        ("Free", {"1": [load(p) for p in args.free]}),
    ]
    if args.c012_core7:
        out[0][1]["3"] = [load(args.c012_core7)]
    if args.c012_core0:
        out[1][1]["3"] = [load(args.c012_core0)]
    return out


def placement_figure(
    placed: list[tuple[str, dict[str, list[dict]]]],
    key: str,
    ylabel: str,
    path: Path,
) -> None:
    """Draw one value by placement, one point per run file."""
    plt.rcParams["font.family"] = "serif"
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for count, (color, marker, label) in STYLE.items():
        xs, ys = [], []
        for index, (_, files) in enumerate(placed):
            for sweep in files.get(count, []):
                xs.append(index)
                ys.append(sweep[key])
        if xs:
            ax.plot(
                xs,
                ys,
                linestyle="none",
                marker=marker,
                color=color,
                markersize=6,
                label=label,
            )
    ax.set_xticks(range(len(placed)))
    ax.set_xticklabels([p[0] for p in placed])
    ax.set_xlim(-0.5, len(placed) - 0.5)
    top = max(y for line in ax.lines for y in line.get_ydata())
    ax.set_ylim(0, top * 1.18)
    ax.set_xlabel("Where the calling thread runs", fontsize=9)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.legend(loc="lower right", fontsize=7, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def operator_figure(rows: list[dict], path: Path) -> None:
    """Draw the time of the main operator types, NPU and CPU."""
    plt.rcParams["font.family"] = "serif"
    ranked = sorted(rows, key=lambda r: r["total"], reverse=True)
    main = [r for r in ranked if r["total"] > 0][:TOP_OPERATORS]
    other = sum(r["total"] for r in ranked) - sum(r["total"] for r in main)
    labels = [r["op"] for r in main] + ["Other"]
    values = [r["total"] / 1000 for r in main] + [other / 1000]
    colors = [CPU_COLOR if r["cpu"] > r["npu"] else NPU_COLOR for r in main]
    colors.append("#808080")
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    positions = range(len(labels))
    ax.barh(positions, values, color=colors, edgecolor=BLACK, linewidth=0.6)
    ax.set_yticks(list(positions))
    ax.set_yticklabels(labels, fontsize=8)
    ax.invert_yaxis()
    for position, value in zip(positions, values, strict=True):
        ax.annotate(
            f"{value:.1f}",
            (value, position),
            textcoords="offset points",
            xytext=(3, 0),
            va="center",
            fontsize=8,
        )
    ax.set_xlim(0, max(values) * 1.15)
    ax.set_xlabel("Time per inference (ms)", fontsize=9)
    handles = [
        mpl.patches.Patch(facecolor=NPU_COLOR, edgecolor=BLACK, label="NPU"),
        mpl.patches.Patch(facecolor=CPU_COLOR, edgecolor=BLACK, label="CPU"),
    ]
    ax.legend(
        handles=handles,
        loc="lower right",
        fontsize=7,
        frameon=True,
        edgecolor=BLACK,
    )
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def tables(placed: list, rows: list[dict]) -> str:
    """Return the markdown tables of the figures."""
    lines = [
        "# Tables of the NPU inference bench",
        "",
        "| Placement | NPU cores | p50 ms | p95 ms | p99 ms | CPU ms | "
        "System CPU % |",
        "|---|---|---|---|---|---|---|",
    ]
    for label, files in placed:
        for count, sweeps in files.items():
            lines.extend(
                f"| {label.replace(chr(10), ' ')} | {count} | "
                f"{s['p50_ms']:.2f} | {s['p95_ms']:.2f} | "
                f"{s['p99_ms']:.2f} | {s['cpu_ms_per_inference']:.2f} | "
                f"{s['system_cpu_percent']:.1f} |"
                for s in sweeps
            )
    lines += [
        "",
        "Operator types (RKNN runtime table, one inference, us):",
        "",
        "| Operator | Calls | CPU us | NPU us | Total us |",
        "|---|---|---|---|---|",
    ]
    lines.extend(
        f"| {r['op']} | {r['calls']} | {r['cpu']} | {r['npu']} | "
        f"{r['total']} |"
        for r in sorted(rows, key=lambda r: r["total"], reverse=True)
    )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    """Draw the figures, return the exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--core7", type=Path, required=True)
    parser.add_argument("--core0", type=Path, required=True)
    parser.add_argument("--core1", type=Path, required=True)
    parser.add_argument("--free", type=Path, nargs="+", required=True)
    parser.add_argument("--c012-core7", type=Path)
    parser.add_argument("--c012-core0", type=Path)
    parser.add_argument("--perf", type=Path, required=True)
    args = parser.parse_args()
    placed = cells(args)
    rows = operators(args.perf)
    args.out.mkdir(parents=True, exist_ok=True)
    placement_figure(
        placed,
        "p50_ms",
        "Latency p50 (ms)",
        args.out / "fig1_infer_latency.png",
    )
    placement_figure(
        placed,
        "cpu_ms_per_inference",
        "CPU time per inference (ms)",
        args.out / "fig2_infer_cpu.png",
    )
    operator_figure(rows, args.out / "fig3_infer_operators.png")
    (args.out / "tables.md").write_text(tables(placed, rows), encoding="utf-8")
    captions = [
        "# Captions of the NPU inference bench",
        "",
        f"**Figure 1.** Latency (p50 of 300 inferences) of one inference "
        f"by the CPU core that the calling thread runs on and the number "
        f"of NPU cores of the model; unpinned runs are marked Free (one "
        f"point per run). {NOTE}",
        "",
        f"**Figure 2.** CPU time of the process per inference (all "
        f"threads, waiting not counted), same cells as figure 1. {NOTE}",
        "",
        f"**Figure 3.** Time of one inference by operator type, from the "
        f"per-operator table of the RKNN runtime (one profiled run on "
        f"one NPU core). Blue operators run on the NPU, red ones on the "
        f"CPU. {NOTE}",
        "",
    ]
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
