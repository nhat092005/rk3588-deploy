"""Draw the two figures of the RGA3 source rectangle crop test.

* ``--shift DIR`` (output of ``probe_rga_shift OUTDIR``, sine pattern):
  the sampling shift of the output along the axis, in output pixels,
  for R2, R30 and R30 with the source rectangle cut by 4 pixels
  (``check_rga_shift.py`` holds the fit).
* ``--boot FILE`` (output of ``boot_map.py``): the change of
  mAP@0.5:0.95 against the reference input of R2, R30 and R30 cut by 4
  pixels, per model and as the mean of the models with its 95 %
  interval.

The look is that of ``plot_bench_preprocess.py`` (one column, serif,
framed, legend inside, no title); the captions go to ``captions.md``
and the numbers of both figures to ``tables.md``.

Usage: plot_rga_crop.py --out DIR [--shift DIR] [--boot FILE]
Exit code 0 on success, 2 when a file is missing.
"""

import argparse
import json
import sys
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.lines import Line2D
from plot_bench_preprocess import GRID, md_table, paper_style

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "check"))
import check_rga_shift as shift

FIGURE_SIZE = (3.5, 2.6)
EXIT_MISSING = 2
PERCENT = 100.0
LOW, HIGH = 2.5, 97.5
REFERENCE = "ref"
MODELS = ("yolov8n", "yolov5s", "yolov5s_relu", "yolox_s")
MODEL_COLOURS = {
    "yolov8n": "#1f3fd1",
    "yolov5s": "#e86a00",
    "yolov5s_relu": "#1a9a3c",
    "yolox_s": "#d91f26",
}
# What each curve is: the probe file variant, label, colour, marker.
CONFIGS = (
    ("R2", "A0", "R2", "#1a9a3c", "^"),
    ("R30", "A0", "R30, whole frame", "#d91f26", "v"),
    ("R30", "A4", "R30, cut by 4 pixels", "#7e3fa6", "D"),
)
BOOT_INPUTS = (
    ("R2", "R2"),
    ("R30", "R30\nwhole frame"),
    ("R30c4", "R30\ncut by 4 pixels"),
)
ZERO_LINE = "#555555"
OFFSET = 0.12


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--shift", type=Path)
    parser.add_argument("--boot", type=Path)
    return parser.parse_args(argv)


def shift_curves(dump: Path) -> dict[tuple[str, str], tuple]:
    """Return {(axis, config): (positions, shifts)} from the probe."""
    curves = {}
    for axis in ("x", "y"):
        for core, variant, _, _, _ in CONFIGS:
            raw = np.fromfile(
                dump / f"{axis}_{core}_{variant}.rgb", dtype=np.uint8
            )
            picture = raw.reshape(shift.SIDE, shift.SIDE, 3).astype(float)
            values, positions = profile(picture, axis)
            freq, phi, _ = shift.fit(
                values - values.mean(), positions.astype(float)
            )
            places = positions.astype(float)
            errors = [shift.phase_error(axis, p, freq, phi) for p in places]
            origin = shift.PAD_TOP if axis == "y" else 0
            curves[(axis, f"{core}{variant}")] = (places - origin, errors)
    return curves


def profile(picture: np.ndarray, axis: str) -> tuple[np.ndarray, np.ndarray]:
    """Return the grey profile along the axis and its positions."""
    return shift.profile(picture, axis)


def draw_shift(dump: Path, out: Path) -> tuple[str, str]:
    """Draw the shift figure; return its table and caption."""
    curves = shift_curves(dump)
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    ax.axhline(0, color=ZERO_LINE, linewidth=0.8, zorder=1)
    rows = []
    for core, variant, label, colour, marker in CONFIGS:
        for axis, line in (("x", "-"), ("y", "--")):
            places, errors = curves[(axis, f"{core}{variant}")]
            ax.plot(
                places,
                errors,
                color=colour,
                linestyle=line,
                linewidth=1.3,
                marker=marker,
                markevery=max(len(places) // 6, 1),
                markersize=4,
                zorder=3,
            )
            rows.append(
                [
                    label,
                    axis,
                    f"{errors[0]:+.2f}",
                    f"{errors[len(errors) // 2]:+.2f}",
                    f"{errors[-1]:+.2f}",
                ]
            )
    ax.set_xlabel("Position along the axis (output pixels)")
    ax.set_ylabel("Sampling shift (output pixels)")
    ax.grid(True, color=GRID, linewidth=0.5, zorder=0)
    entries = [
        Line2D([], [], color=c, marker=m, markersize=4, linewidth=1.3)
        for _, _, _, c, m in CONFIGS
    ]
    names = [label for _, _, label, _, _ in CONFIGS]
    entries += [
        Line2D([], [], color="#444444", linestyle="-", linewidth=1.3),
        Line2D([], [], color="#444444", linestyle="--", linewidth=1.3),
    ]
    names += ["x axis", "y axis"]
    ax.set_ylim(-0.05, 1.5)
    ax.legend(entries, names, loc="upper left", frameon=True, framealpha=1)
    fig.tight_layout()
    fig.savefig(out / "fig9_rga3_shift.png", dpi=300)
    plt.close(fig)
    table = md_table(
        ["Config", "Axis", "First pixel", "Middle", "Last pixel"], rows
    )
    caption = (
        "Sampling shift of the 4K NV12 to 640x640 letterbox job (rotation"
        " 180) along each axis, from a sine pattern of period 120 source"
        " pixels, against the centre aligned mapping. A positive value"
        " means that the content of a later pixel is shown. R2 follows the"
        " mapping; R30 (RGA3 core 0) drifts by about 0.7 pixel over the"
        " axis, a scale error; cutting the source rectangle by 4 pixels"
        " (x=4, width=3836, or y=4, height=2156) brings it within 0.15"
        " pixel."
    )
    return table, caption


def draw_boot(path: Path, out: Path) -> tuple[str, str]:
    """Draw the mAP change figure; return its table and caption."""
    samples = json.loads(path.read_text())
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    ax.axhline(0, color=ZERO_LINE, linewidth=0.8, zorder=1)
    rows = []
    for index, (label, _) in enumerate(BOOT_INPUTS):
        deltas = {
            m: (
                np.array(samples[f"{m}|{label}"])
                - np.array(samples[f"{m}|{REFERENCE}"])
            )
            * PERCENT
            for m in MODELS
        }
        mean = np.mean([deltas[m] for m in MODELS], axis=0)
        low, high = np.percentile(mean, [LOW, HIGH])
        for slot, model in enumerate(MODELS):
            x = index + (slot - 1.5) * OFFSET * 0.6 - OFFSET * 2.5
            ax.plot(
                x,
                deltas[model].mean(),
                marker="o",
                markersize=3.5,
                color=MODEL_COLOURS[model],
                linestyle="none",
                zorder=4,
            )
        ax.errorbar(
            index,
            mean.mean(),
            yerr=[[mean.mean() - low], [high - mean.mean()]],
            marker="s",
            markersize=6,
            color="#000000",
            capsize=3,
            linewidth=1.2,
            linestyle="none",
            zorder=5,
        )
        low_row, high_row = f"{low:+.2f}", f"{high:+.2f}"
        rows.append(
            [label]
            + [f"{deltas[m].mean():+.2f}" for m in MODELS]
            + [f"{mean.mean():+.2f}", f"[{low_row}, {high_row}]"]
        )
    ax.set_xticks(range(len(BOOT_INPUTS)))
    ax.set_xticklabels([text for _, text in BOOT_INPUTS])
    ax.set_xlim(-0.6, len(BOOT_INPUTS) - 0.4)
    ax.set_ylabel("Change of mAP@0.5:0.95 (points)")
    ax.set_ylim(-0.75, 1.0)
    ax.grid(True, axis="y", color=GRID, linewidth=0.5, zorder=0)
    entries = [
        Line2D(
            [],
            [],
            color=MODEL_COLOURS[m],
            marker="o",
            markersize=3.5,
            linestyle="none",
        )
        for m in MODELS
    ]
    names = list(MODELS)
    entries.append(
        Line2D(
            [], [], color="#000000", marker="s", markersize=6, linewidth=1.2
        )
    )
    names.append("mean of 4 models, 95 % interval")
    ax.legend(
        entries,
        names,
        loc="upper center",
        ncol=2,
        frameon=True,
        framealpha=1,
        fontsize=6.5,
    )
    fig.tight_layout()
    fig.savefig(out / "fig10_map_crop.png", dpi=300)
    plt.close(fig)
    table = md_table(
        ["Input", *MODELS, "Mean", "95 % interval of the mean"], rows
    )
    caption = (
        "Change of mAP@0.5:0.95 against the reference input (OpenCV) at"
        " the 4K case, 2000 COCO pictures enlarged into a 3840x2160 frame,"
        " NV12 limited range straight into the RGA letterbox. Small dots:"
        " one model; large square: mean of the four models with the 95 %"
        " interval of a paired bootstrap over the pictures (200 samples)."
        " R30 without the crop loses 0.40 and its interval excludes 0;"
        " cut by 4 pixels it does not differ from R2 or from the"
        " reference."
    )
    return table, caption


def main(argv: list[str]) -> int:
    """Draw what the inputs allow, return the exit code."""
    args = parse_args(argv)
    paper_style()
    args.out.mkdir(parents=True, exist_ok=True)
    tables, captions = [], []
    if args.shift:
        if not (args.shift / "results.csv").is_file():
            print(f"{args.shift}: no results.csv")
            return EXIT_MISSING
        table, caption = draw_shift(args.shift, args.out)
        tables.append(
            "### Figure 9: sampling shift (output pixels)\n\n" + table
        )
        captions.append("**Figure 9.** " + caption + "\n")
    if args.boot:
        if not args.boot.is_file():
            print(f"{args.boot}: missing")
            return EXIT_MISSING
        table, caption = draw_boot(args.boot, args.out)
        tables.append(
            "### Figure 10: change of mAP@0.5:0.95 (points)\n\n" + table
        )
        captions.append("**Figure 10.** " + caption + "\n")
    (args.out / "tables.md").write_text("\n".join(tables), encoding="utf-8")
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
