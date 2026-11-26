"""Draw the figures and tables of the encode bench (stage 3).

Inputs: the JSON of ``bench_encode`` (MPP, the hardware encoder) and of
``tools/accuracy/run_encode_x265.py`` (x265 on the CPU, and the PSNR of
every stream against the clip).

* Figure 1: encode throughput, frames per second, one point per run.
* Figure 2: CPU time per frame (all threads, waiting not counted).
* Figure 3: quality (PSNR of the luma, dB) against throughput, the
  trade-off in one picture.
* Figure 4: quality (PSNR of the luma, dB) against the bitrate that the
  stream reached.

The figures follow the look of the other stages
(``plot_bench_draw.py``): one column wide (3.5 in), serif text, a full
frame with inward ticks, no title (the caption goes under the figure,
see ``captions.md``), PNG 300 dpi. ``tables.md`` holds the numbers.

Usage: plot_bench_encode.py --out DIR --mpp mpp.json --x265 x265.json
"""

import argparse
import json
import sys
from pathlib import Path

import matplotlib as mpl

mpl.use("Agg")
import matplotlib.pyplot as plt

FIGURE_SIZE = (3.5, 2.6)
BLACK = "#000000"
GRID = "#d0d0d0"
STYLE = {
    "mpp": ("#1a9a3c", "^"),
    "ultrafast:8": ("#1f3fd1", "o"),
    "veryfast:8": ("#e86a00", "s"),
    "ultrafast:1": ("#d91f26", "D"),
}
SHORT = {"mpp": "MPP", "ultrafast:8": "uf-8", "veryfast:8": "vf-8"}
FULL = {
    "mpp": "MPP (hardware)",
    "ultrafast:8": "x265 ultrafast, 8 threads",
    "ultrafast:1": "x265 ultrafast, 1 thread",
    "veryfast:8": "x265 veryfast, 8 threads",
}
NOTE = (
    "MPP is the H.265 encoder of the board (VideoEncoder, constant"
    " bitrate 20 Mbps). uf and vf are x265 (FFmpeg libx265) with the"
    " preset ultrafast and veryfast and the number of threads. Every"
    " method encodes the same 90-frame 3840x2160 clip, 30 FPS, a key"
    " frame every 30 frames."
)


def short(case: str) -> str:
    """Return the axis label of a case."""
    if case in SHORT:
        return SHORT[case]
    preset, threads = case.split(":")
    return f"{'uf' if preset == 'ultrafast' else 'vf'}-{threads}"


def style_axes(ax: plt.Axes) -> None:
    """Give an axes the paper look."""
    for side in ax.spines.values():
        side.set_color(BLACK)
        side.set_linewidth(0.8)
    ax.tick_params(direction="in", top=True, right=True, labelsize=8)
    ax.grid(color=GRID, linewidth=0.5)
    ax.set_axisbelow(True)


def rows(mpp: dict, x265: dict) -> list[dict]:
    """Return one row per method: label, runs of fps and CPU, kbps."""
    out = [
        {
            "case": "mpp",
            "fps": [r["encode_fps"] for r in mpp["per_run"]],
            "cpu": [r["cpu_ms_per_frame"] for r in mpp["per_run"]],
            "kbps": [r["kbps"] for r in mpp["per_run"]][-1],
        }
    ]
    out.extend(
        {
            "case": case["case"],
            "fps": [r["encode_fps"] for r in case["runs"]],
            "cpu": [r["cpu_ms_per_frame"] for r in case["runs"]],
            "kbps": case["kbps"],
        }
        for case in x265["cases"]
    )
    return out


def dot_figure(
    items: list[dict], key: str, ylabel: str, path: Path, where: str
) -> None:
    """Draw one point per run for every method, on a log axis."""
    plt.rcParams["font.family"] = "serif"
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for index, item in enumerate(items):
        color, marker = STYLE[item["case"]]
        values = item[key]
        ax.plot(
            [index] * len(values),
            values,
            linestyle="none",
            marker=marker,
            color=color,
            markersize=6,
            label=FULL[item["case"]],
        )
        mean = sum(values) / len(values)
        ax.annotate(
            f"{mean:.0f}" if mean >= 100 else f"{mean:.3g}",
            (index, mean),
            textcoords="offset points",
            xytext=(0, 7),
            ha="center",
            fontsize=8,
        )
    ax.set_yscale("log")
    ax.set_xticks(range(len(items)))
    ax.set_xticklabels([short(i["case"]) for i in items])
    ax.set_xlim(-0.6, len(items) - 0.4)
    low = min(min(i[key]) for i in items)
    high = max(max(i[key]) for i in items)
    ax.set_ylim(low / 2.5, high * 4)
    ax.set_xlabel("Method", fontsize=9)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.legend(loc=where, fontsize=7, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def quality_figure(items: list[dict], x265: dict, path: Path) -> None:
    """Draw luma PSNR against the bitrate reached."""
    plt.rcParams["font.family"] = "serif"
    psnr = {q["label"]: q["psnr"] for q in x265["quality"]}
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for item in items:
        label = "mpp" if item["case"] == "mpp" else f"x265 {item['case']}"
        color, marker = STYLE[item["case"]]
        value = psnr[label]["y"]
        ax.plot(
            item["kbps"] / 1000,
            value,
            linestyle="none",
            marker=marker,
            color=color,
            markersize=7,
            label=FULL[item["case"]],
        )
    ax.set_xlabel("Bitrate reached (Mbps)", fontsize=9)
    ax.set_ylabel("PSNR of the luma (dB)", fontsize=9)
    ax.set_xlim(13.5, 19)
    ax.set_ylim(37.5, 44.5)
    ax.legend(loc="upper left", fontsize=8, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def tradeoff_figure(items: list[dict], x265: dict, path: Path) -> None:
    """Draw luma PSNR against throughput (log), one point per method."""
    plt.rcParams["font.family"] = "serif"
    psnr = {q["label"]: q["psnr"] for q in x265["quality"]}
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for item in items:
        label = "mpp" if item["case"] == "mpp" else f"x265 {item['case']}"
        color, marker = STYLE[item["case"]]
        speed = sum(item["fps"]) / len(item["fps"])
        ax.plot(
            speed,
            psnr[label]["y"],
            linestyle="none",
            marker=marker,
            color=color,
            markersize=7,
            label=FULL[item["case"]],
        )
    ax.set_xscale("log")
    ax.set_xlim(0.4, 150)
    ax.set_ylim(37.5, 44.5)
    ax.set_xlabel("Encode throughput (frames/s)", fontsize=9)
    ax.set_ylabel("PSNR of the luma (dB)", fontsize=9)
    ax.legend(loc="center left", fontsize=8, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def tables(items: list[dict], mpp: dict, x265: dict) -> str:
    """Return the markdown tables of the figures."""
    psnr = {q["label"]: q["psnr"] for q in x265["quality"]}
    lines = [
        "# Tables of the encode bench",
        "",
        "| Method | Encode fps (runs) | CPU ms per frame (runs) | Mbps | "
        "PSNR Y | PSNR U | PSNR V |",
        "|---|---|---|---|---|---|---|",
    ]
    for item in items:
        label = "mpp" if item["case"] == "mpp" else f"x265 {item['case']}"
        quality = psnr[label]
        lines.append(
            f"| {label} | {', '.join(f'{v:.2f}' for v in item['fps'])} | "
            f"{', '.join(f'{v:.2f}' for v in item['cpu'])} | "
            f"{item['kbps'] / 1000:.2f} | {quality['y']:.2f} | "
            f"{quality['u']:.2f} | {quality['v']:.2f} |"
        )
    lines += [
        "",
        f"MPP encode latency: p50 {mpp['p50_ms']:.2f} ms, "
        f"p95 {mpp['p95_ms']:.2f} ms, p99 {mpp['p99_ms']:.2f} ms.",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    """Draw the figures, return the exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--mpp", type=Path, required=True)
    parser.add_argument("--x265", type=Path, required=True)
    args = parser.parse_args()
    mpp = json.loads(args.mpp.read_text(encoding="utf-8"))
    x265 = json.loads(args.x265.read_text(encoding="utf-8"))
    items = rows(mpp, x265)
    args.out.mkdir(parents=True, exist_ok=True)
    dot_figure(
        items,
        "fps",
        "Encode throughput (frames/s)",
        args.out / "fig1_encode_fps.png",
        "upper right",
    )
    dot_figure(
        items,
        "cpu",
        "CPU time per frame (ms)",
        args.out / "fig2_encode_cpu.png",
        "lower right",
    )
    tradeoff_figure(items, x265, args.out / "fig3_encode_tradeoff.png")
    quality_figure(items, x265, args.out / "fig4_encode_quality.png")
    (args.out / "tables.md").write_text(
        tables(items, mpp, x265), encoding="utf-8"
    )
    captions = [
        "# Captions of the encode bench",
        "",
        f"**Figure 1.** Encode throughput, one point per run (3840x2160 "
        f"clip of {mpp['frames_per_run']} frames, back to back, mean in "
        f"the label, log scale). {NOTE}",
        "",
        f"**Figure 2.** CPU time of the process per frame, one point per "
        f"run (all threads, waiting not counted, log scale). {NOTE}",
        "",
        f"**Figure 3.** PSNR of the luma of the decoded stream against "
        f"the clip, over all frames, against the encode throughput (log "
        f"scale); the bitrates reached differ (figure 4). {NOTE}",
        "",
        f"**Figure 4.** PSNR of the luma of the decoded stream against "
        f"the clip, over all frames, against the bitrate that the stream "
        f"reached. The bitrates differ, so the methods are not compared "
        f"at an equal rate. {NOTE}",
        "",
    ]
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
