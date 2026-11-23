"""Draw figure 5: change of detection mAP per preprocess method.

Input: ``map.json`` of ``tools/accuracy/eval_map.py``. For each of the
six methods the figure shows how much the mAP@0.5:0.95 of a detector
changes when the method makes its input instead of the reference input
(``model_io.preprocess`` on the CPU): the mAP with the method minus the
mAP with the reference, both scored against the COCO labels, in mAP
points. One bar per model (four colours), a bar below zero is a loss.

* (a) every COCO picture of the run (long side 640 px for most of them,
  so the preprocess does not scale them, plus the scaled-up ones);
* (b) only the pictures that the preprocess scales up (long side under
  640 px, about 1.28 times).

Writes ``fig5_map_change.png`` (300 dpi), ``fig5_tables.md`` (the
numbers) and ``fig5_caption.md`` to ``--out``. Same look as
``plot_bench_preprocess.py``, which it imports.

Usage: plot_map.py --map map.json --out DIR
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.axes import Axes
from plot_bench_preprocess import GRID, METHODS, md_table, paper_style

REFERENCE = "ref"
FP32_SUFFIX = "_fp32"
METRIC = "map5095"
PERCENT = 100.0
PAIR_SIZE = (7.0, 2.9)
ZERO_LINE = "#555555"
GROUP_WIDTH = 0.8
# One colour per model, the same hues as figures 1-4.
MODEL_STYLES = {
    "yolov8n": {"color": "#1f3fd1"},
    "yolov5s": {"color": "#e86a00"},
    "yolov5s_relu": {"color": "#1a9a3c"},
    "yolox_s": {"color": "#d91f26"},
}


def panels(doc: dict) -> list[tuple[str, str, str]]:
    """Return (group, caption under the panel, table title) of each."""
    frames = doc["frames"]
    return [
        (
            "all",
            f"(a) all {frames['all']} pictures",
            f"all {frames['all']} pictures",
        ),
        (
            "upscaled",
            f"(b) the {frames['upscaled']} pictures scaled up",
            f"the {frames['upscaled']} pictures scaled up (long side"
            " under 640 px)",
        ),
    ]


def deltas(scores: dict, group: str) -> dict[str, dict[str, float]]:
    """Return {method: {model: change of mAP@0.5:0.95 in points}}."""
    out: dict[str, dict[str, float]] = {m: {} for m in METHODS}
    for model, row in scores.items():
        if model.endswith(FP32_SUFFIX):
            continue
        base = row[REFERENCE][group][METRIC]
        for method in METHODS:
            if method in row:
                out[method][model] = (
                    row[method][group][METRIC] - base
                ) * PERCENT
    return out


def bars(
    ax: Axes, change: dict[str, dict[str, float]], *, legend: bool
) -> None:
    """Draw, at each method, one bar per model."""
    models = [
        m for m in MODEL_STYLES if any(m in row for row in change.values())
    ]
    width = GROUP_WIDTH / len(models)
    for i, method in enumerate(METHODS):
        for k, model in enumerate(models):
            if model not in change[method]:
                continue
            ax.bar(
                i + (k - (len(models) - 1) / 2) * width,
                change[method][model],
                width=width,
                edgecolor="black",
                linewidth=0.5,
                label=model if i == 0 else None,
                zorder=3,
                **MODEL_STYLES[model],
            )
    ax.axhline(0, color="black", linewidth=0.8, zorder=4)
    ax.set_ylabel("mAP@0.5:0.95 change vs reference (points)")
    ax.set_xticks(range(len(METHODS)), METHODS)
    ax.set_xlim(-0.6, len(METHODS) - 0.4)
    ax.grid(color=GRID, linewidth=0.5, linestyle="--", axis="y")
    ax.set_axisbelow(True)
    if legend:
        ax.legend(
            loc="lower left",
            ncol=2,
            frameon=False,
            handlelength=1.4,
            handletextpad=0.4,
            columnspacing=0.8,
            labelspacing=0.3,
            borderaxespad=0.3,
        )


def table(
    scores: dict, group: str, change: dict[str, dict[str, float]]
) -> str:
    """Return the numbers of a panel: reference, row per method."""
    models = sorted({m for row in change.values() for m in row})
    base = ", ".join(
        f"{m} {scores[m][REFERENCE][group][METRIC] * PERCENT:.2f}"
        for m in models
    )
    rows = [
        [
            method,
            *(f"{change[method][m]:+.2f}" for m in models),
            f"{statistics.fmean(change[method].values()):+.2f}",
        ]
        for method in METHODS
        if change[method]
    ]
    return f"mAP@0.5:0.95 of the reference: {base}.\n\n" + md_table(
        ["method", *models, "mean"], rows
    )


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    return parser.parse_args()


def main() -> int:
    """Draw figure 5 and write its table and caption."""
    args = parse_args()
    doc = json.loads(args.map.read_text())
    args.out.mkdir(parents=True, exist_ok=True)
    paper_style()
    fig, axes = plt.subplots(1, 2, figsize=PAIR_SIZE, sharey=True)
    sections = []
    for ax, (group, caption, title) in zip(axes, panels(doc), strict=True):
        change = deltas(doc["scores"], group)
        bars(ax, change, legend=group == "all")
        ax.annotate(
            caption,
            xy=(0.5, 0.0),
            xycoords="axes fraction",
            xytext=(0, -26),
            textcoords="offset points",
            ha="center",
            va="top",
        )
        sections.append(
            f"## mAP@0.5:0.95 change vs reference, {title}\n\n"
            + table(doc["scores"], group, change)
        )
    fig.tight_layout(pad=0.4, w_pad=1.2)
    fig.savefig(args.out / "fig5_map_change.png", dpi=300)
    plt.close(fig)
    (args.out / "fig5_tables.md").write_text("\n".join(sections))
    (args.out / "fig5_caption.md").write_text(
        "**fig5_map_change**: Change of the detection mAP@0.5:0.95 when"
        " each preprocess method makes the model input instead of the"
        " reference (CPU, `model_io.preprocess`): the mAP with the method"
        " minus the mAP with the reference, both scored against the COCO"
        " val2017 labels, in mAP points. One bar per model."
        " (a) all pictures of the run (most have a long"
        " side of 640 px and are not scaled); (b) only the pictures that"
        " the preprocess scales up (long side under 640 px, about 1.28"
        " times). Zero means no change; a method below zero lowers the"
        " mAP.\n"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
