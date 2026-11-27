"""Draw the figures and tables of the JPEG bench (stage 4).

Inputs: the JSON of ``bench_jpeg`` (MPP, the hardware encoder, and
libjpeg-turbo on one CPU thread) for a quality sweep, and the JSON of
``tools/accuracy/check_jpeg_bench.py`` (PSNR of the decoded files).

* Figure 1: latency of one image (p50) against the quality factor.
* Figure 2: CPU time of the process per image against the quality.
* Figure 3: quality (PSNR of the luma, dB) against the bits per pixel
  of the file, one point per quality factor.
* Figure 4: latency (p50) of the quality 90 image by where the calling
  thread runs: a big core, a little core, or left to the scheduler.

The figures follow the look of the other stages
(``plot_bench_draw.py``): one column wide (3.5 in), serif text, a full
frame with inward ticks, no title (the caption goes under the figure,
see ``captions.md``), PNG 300 dpi. ``tables.md`` holds the numbers.

Usage: plot_bench_jpeg.py --out DIR --big big.json --little little.json
--free free1.json [free2.json ...] --check check.json
[--small small.json --check-small check_small.json]
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
    "turbo": ("#1f3fd1", "o"),
}
FULL = {
    "mpp": "MPP (hardware)",
    "turbo": "libjpeg-turbo, 1 thread",
}
NOTE = (
    "MPP is the JPEG encoder of the board (JpegEncoder, quality factor"
    " as set). libjpeg-turbo is the CPU encoder on one thread, from the"
    " NV12 planes, 4:2:0. Both encode the same 8 images of a 3840x2160"
    " COCO mosaic clip, 300 timed encodes per point, frequencies locked."
)
APP_QUALITY = 90


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


def series(sweep: dict, method: str, key: str) -> tuple[list, list]:
    """Return the qualities and the values of ``key`` of one method."""
    cases = sorted(
        (c for c in sweep["cases"] if c["method"] == method),
        key=lambda c: c["quality"],
    )
    return [c["quality"] for c in cases], [c[key] for c in cases]


def finish(fig: plt.Figure, ax: plt.Axes, path: Path, where: str) -> None:
    """Add the legend and the paper look, save the figure."""
    ax.legend(loc=where, fontsize=7, frameon=True, edgecolor=BLACK)
    style_axes(ax)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def sweep_figure(
    sweep: dict, key: str, ylabel: str, path: Path, where: str
) -> None:
    """Draw one value of the quality sweep for both methods."""
    plt.rcParams["font.family"] = "serif"
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for method in ("mpp", "turbo"):
        color, marker = STYLE[method]
        x, y = series(sweep, method, key)
        ax.plot(
            x,
            y,
            marker=marker,
            color=color,
            markersize=6,
            linewidth=1.0,
            label=FULL[method],
        )
    ax.set_xlabel("Quality factor", fontsize=9)
    ax.set_ylabel(ylabel, fontsize=9)
    ax.set_ylim(bottom=0)
    ax.set_xticks([c["quality"] for c in sweep["cases"]][::2])
    finish(fig, ax, path, where)


def rate_figure(sweep: dict, check: dict, path: Path) -> None:
    """Draw luma PSNR against the bits per pixel of the file."""
    plt.rcParams["font.family"] = "serif"
    pixels = sweep["width"] * sweep["height"]
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    for method in ("mpp", "turbo"):
        color, marker = STYLE[method]
        cases = sorted(
            (c for c in check["cases"] if c["method"] == method),
            key=lambda c: c["quality"],
        )
        ax.plot(
            [c["mean_bytes"] * 8 / pixels for c in cases],
            [c["psnr_y"] for c in cases],
            marker=marker,
            color=color,
            markersize=6,
            linewidth=1.0,
            label=FULL[method],
        )
    ax.set_xlabel("File size (bits per pixel)", fontsize=9)
    ax.set_ylabel("PSNR of the luma (dB)", fontsize=9)
    finish(fig, ax, path, "lower right")


def place_figure(
    big: dict, little: dict, free: list[dict], path: Path
) -> None:
    """Draw the quality 90 latency by where the thread runs."""
    plt.rcParams["font.family"] = "serif"
    fig, ax = plt.subplots(figsize=FIGURE_SIZE)
    groups = [("Big core", [big]), ("Little core", [little]), ("Free", free)]
    for method in ("mpp", "turbo"):
        color, marker = STYLE[method]
        xs, ys = [], []
        for index, (_, files) in enumerate(groups):
            for sweep in files:
                case = next(
                    c
                    for c in sweep["cases"]
                    if c["method"] == method and c["quality"] == APP_QUALITY
                )
                xs.append(index)
                ys.append(case["p50_ms"])
        ax.plot(
            xs,
            ys,
            linestyle="none",
            marker=marker,
            color=color,
            markersize=6,
            label=FULL[method],
        )
    ax.set_yscale("log")
    ticks = [20, 30, 50, 100]
    ax.set_yticks(ticks)
    ax.set_yticklabels([str(t) for t in ticks])
    ax.yaxis.set_minor_formatter(mpl.ticker.NullFormatter())
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels([g[0] for g in groups])
    ax.set_xlim(-0.5, len(groups) - 0.5)
    ax.set_ylim(15, 400)
    ax.set_xlabel("Where the calling thread runs", fontsize=9)
    ax.set_ylabel("Latency p50 (ms)", fontsize=9)
    finish(fig, ax, path, "upper left")


def tables(sweep: dict, check: dict, extra: dict) -> str:
    """Return the markdown tables of the figures."""
    psnr = {(c["method"], c["quality"]): c for c in check["cases"]}
    lines = [
        "# Tables of the JPEG bench",
        "",
        f"{sweep['width']}x{sweep['height']}, big core, frequencies "
        f"locked ({sweep['runs']} timed encodes per row).",
        "",
        "| Method | Quality | p50 ms | p95 ms | p99 ms | CPU ms | "
        "Mean bytes | bits/px | PSNR Y | PSNR C |",
        "|---|---|---|---|---|---|---|---|---|---|",
    ]
    pixels = sweep["width"] * sweep["height"]
    for case in sorted(
        sweep["cases"], key=lambda c: (c["quality"], c["method"])
    ):
        quality = psnr[(case["method"], case["quality"])]
        lines.append(
            f"| {case['method']} | {case['quality']} | "
            f"{case['p50_ms']:.2f} | {case['p95_ms']:.2f} | "
            f"{case['p99_ms']:.2f} | {case['cpu_ms_per_image']:.2f} | "
            f"{case['mean_bytes']:.0f} | "
            f"{case['mean_bytes'] * 8 / pixels:.3f} | "
            f"{quality['psnr_y']:.2f} | {quality['psnr_c']:.2f} |"
        )
    lines += ["", f"Quantization tables equal: {check['tables_equal']}.", ""]
    for label, data in extra.items():
        lines += [
            f"## {label}",
            "",
            "| Method | Quality | p50 ms | p95 ms | CPU ms |",
        ]
        lines.append("|---|---|---|---|---|")
        lines.extend(
            f"| {c['method']} | {c['quality']} | {c['p50_ms']:.2f} | "
            f"{c['p95_ms']:.2f} | {c['cpu_ms_per_image']:.2f} |"
            for c in data["cases"]
        )
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    """Draw the figures, return the exit code."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--big", type=Path, required=True)
    parser.add_argument("--little", type=Path, required=True)
    parser.add_argument("--free", type=Path, nargs="+", required=True)
    parser.add_argument("--check", type=Path, required=True)
    parser.add_argument("--small", type=Path)
    args = parser.parse_args()
    big, little = load(args.big), load(args.little)
    free = [load(p) for p in args.free]
    check = load(args.check)
    args.out.mkdir(parents=True, exist_ok=True)
    sweep_figure(
        big,
        "p50_ms",
        "Latency p50 (ms)",
        args.out / "fig1_jpeg_latency.png",
        "upper left",
    )
    sweep_figure(
        big,
        "cpu_ms_per_image",
        "CPU time per image (ms)",
        args.out / "fig2_jpeg_cpu.png",
        "upper left",
    )
    rate_figure(big, check, args.out / "fig3_jpeg_quality.png")
    place_figure(big, little, free, args.out / "fig4_jpeg_placement.png")
    extra = {"Little core, quality 90": little}
    extra.update({f"Free run {i + 1}": f for i, f in enumerate(free)})
    if args.small:
        extra["1280x720, big core"] = load(args.small)
    (args.out / "tables.md").write_text(
        tables(big, check, extra), encoding="utf-8"
    )
    captions = [
        "# Captions of the JPEG bench",
        "",
        f"**Figure 1.** Latency of one 3840x2160 image (p50 of "
        f"{big['runs']} encodes) against the quality factor, the calling "
        f"thread on a big core. {NOTE}",
        "",
        f"**Figure 2.** CPU time of the process per image (all threads, "
        f"waiting not counted) against the quality factor, the calling "
        f"thread on a big core. {NOTE}",
        "",
        f"**Figure 3.** PSNR of the luma of the decoded JPEG against the "
        f"source frame (mean of {check['cases'][0]['images']} images) "
        f"against the file size in bits per pixel, one point per "
        f"quality factor. Both files are decoded by FFmpeg. {NOTE}",
        "",
        f"**Figure 4.** Latency (p50) of the quality {APP_QUALITY} image "
        f"by where the calling thread runs: core 7 (big), core 0 "
        f"(little), or left to the scheduler (one point per run). "
        f"{NOTE}",
        "",
    ]
    (args.out / "captions.md").write_text(
        "\n".join(captions), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
