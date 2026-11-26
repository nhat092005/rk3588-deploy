"""Measure the sampling shift of the RGA letterbox (probe_rga_shift).

``probe_rga_shift OUTDIR`` runs the 4K NV12 to 640x640 job (rotation
180) on a sine pattern (period 120 source pixels) on each core, with
the source rectangle moved or shortened, and writes ``results.csv`` and
one ``<axis>_<core>_<variant>.rgb`` per run. This script fits a sine to
each output profile and prints the phase error, in output pixels,
against the centre aligned mapping, at three places along the axis. A
variant ``A<n>`` is the rectangle x=n, width=W-n, ``B<n>`` is x=0,
width=W-n (the same for y and the height).

Usage: check_rga_shift.py OUTDIR
Exit code 0 on success, 2 when a file is missing.
"""

import argparse
import csv
import sys
from pathlib import Path

import numpy as np

WIDTH, HEIGHT = 3840, 2160
SIDE = 640
PERIOD = 120.0  # source pixels, as in the probe
SCALE = 6  # 3840 / 640
PAD_TOP = 140
ROWS = 360
FREQUENCY_SPAN = 0.02
FREQUENCY_STEPS = 401
CORES = ("R2", "R30", "Ra")
VARIANTS = ("A0", "A2", "A4", "A6", "A8", "B2", "B4", "B6", "B8")
PLACES = {"x": (40, 320, 600), "y": (160, 320, 480)}
EXIT_MISSING = 2


def parse_args() -> argparse.Namespace:
    """Return the command line arguments."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump", type=Path, help="folder written by the probe")
    return parser.parse_args()


def profile(picture: np.ndarray, axis: str) -> tuple[np.ndarray, np.ndarray]:
    """Return the mean grey profile along the axis and its positions."""
    grey = picture.mean(axis=2)
    if axis == "x":
        return grey[PAD_TOP : PAD_TOP + ROWS].mean(axis=0), np.arange(SIDE)
    rows = np.arange(PAD_TOP, PAD_TOP + ROWS)
    return grey.mean(axis=1)[PAD_TOP : PAD_TOP + ROWS], rows


def fit(
    values: np.ndarray, positions: np.ndarray
) -> tuple[float, float, float]:
    """Fit ``a sin(2 pi f x + phi) + c``; return (f, phi, a)."""
    best: tuple[float, float, np.ndarray] | None = None
    base = 1.0 / (PERIOD / SCALE)
    for freq in np.linspace(
        base * (1 - FREQUENCY_SPAN),
        base * (1 + FREQUENCY_SPAN),
        FREQUENCY_STEPS,
    ):
        design = np.stack(
            [
                np.sin(2 * np.pi * freq * positions),
                np.cos(2 * np.pi * freq * positions),
                np.ones(positions.shape),
            ],
            axis=1,
        )
        coef = np.linalg.lstsq(design, values, rcond=None)[0]
        error = float(((design @ coef - values) ** 2).sum())
        if best is None or error < best[0]:
            best = (error, freq, coef)
    assert best is not None
    _, freq, coef = best
    return (
        freq,
        float(np.arctan2(coef[1], coef[0])),
        float(np.hypot(coef[0], coef[1])),
    )


def phase_error(axis: str, place: float, freq: float, phi: float) -> float:
    """Return the shift in output pixels against the centre mapping.

    Output pixel p shows the source at t = N - (p + 0.5) * 6 - 0.5 (the
    rotation reverses the axis). A positive value means the content of
    position p + shift is shown at p.
    """
    if axis == "x":
        size, offset = WIDTH, place
    else:
        size, offset = HEIGHT, place - PAD_TOP
    source = size - (offset + 0.5) * SCALE - 0.5
    # sin(c - a) = sin(pi - c + a): the pattern runs backwards
    ideal = np.pi - 2 * np.pi * source / PERIOD
    measured = 2 * np.pi * freq * place + phi
    wrapped = (measured - ideal + np.pi) % (2 * np.pi) - np.pi
    return float(wrapped / (2 * np.pi * freq))


def main() -> int:
    """Print the table, return the exit code."""
    args = parse_args()
    results = args.dump / "results.csv"
    if not results.is_file():
        print(f"{results}: missing")
        return EXIT_MISSING
    status = {
        (row["axis"], row["core"], row["variant"]): row["status"]
        for row in csv.DictReader(results.open())
    }
    print(
        "axis core variant  shift in output px at (low, middle, high)"
        "  amplitude  scale"
    )
    for axis, places in PLACES.items():
        for core in CORES:
            for variant in VARIANTS:
                if status.get((axis, core, variant)) != "ok":
                    continue
                raw = np.fromfile(
                    args.dump / f"{axis}_{core}_{variant}.rgb", dtype=np.uint8
                )
                picture = raw.reshape(SIDE, SIDE, 3).astype(float)
                values, positions = profile(picture, axis)
                freq, phi, amplitude = fit(
                    values - values.mean(), positions.astype(float)
                )
                shifts = [phase_error(axis, p, freq, phi) for p in places]
                text = " ".join(f"{s:+.2f}" for s in shifts)
                print(
                    f"{axis} {core:3} {variant:4}  {text}  {amplitude:6.1f}  "
                    f"{freq * PERIOD / SCALE:.4f}"
                )
    return 0


if __name__ == "__main__":
    sys.exit(main())
