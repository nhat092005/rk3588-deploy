"""Tests of the canvas placement (run: pytest deploy/tools/accuracy)."""

import sys
from pathlib import Path

import numpy as np
import pytest
from detect_postprocess import compose_meta, to_coco

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "generate"))
from make_coco_nv12 import CANVAS_FILL, place_on_canvas

CANVAS = (3840, 2160)


def test_picture_fills_the_canvas_height_and_is_centred() -> None:
    """640x480 is enlarged 4.5 times to 2880x2160, centred on grey."""
    picture = np.zeros((480, 640, 3), dtype=np.uint8)
    frame, (scale, left, top) = place_on_canvas(picture, CANVAS)
    assert frame.shape == (2160, 3840, 3)
    assert scale == pytest.approx(4.5)
    assert (left, top) == (480, 0)
    assert frame[0, 0, 0] == CANVAS_FILL
    assert frame[1000, 480 + 5, 0] == 0
    assert frame[1000, 480 + 2880 - 5, 0] == 0
    assert frame[1000, 480 + 2880 + 5, 0] == CANVAS_FILL


def test_offsets_are_even() -> None:
    """Odd picture sizes still give even offsets (chroma 2x2 blocks)."""
    picture = np.zeros((333, 501, 3), dtype=np.uint8)
    _, (_, left, top) = place_on_canvas(picture, CANVAS)
    assert left % 2 == 0
    assert top % 2 == 0


def test_composed_meta_maps_the_input_to_the_picture() -> None:
    """A box of the 640 input comes back on the picture pixels."""
    picture = np.zeros((480, 640, 3), dtype=np.uint8)
    _, placement = place_on_canvas(picture, CANVAS)
    scale, left, top = placement
    gain, pad_left, pad_top = 640 / 3840, 0, 140  # letterbox of 3840x2160
    # Picture box (100, 50)-(300, 250) as the model sees it.
    box = np.array(
        [
            [
                (100 * scale + left) * gain + pad_left,
                (50 * scale + top) * gain + pad_top,
                (300 * scale + left) * gain + pad_left,
                (250 * scale + top) * gain + pad_top,
            ]
        ]
    )
    meta = compose_meta((gain, pad_left, pad_top), placement)
    rows = to_coco(box, np.array([0]), np.array([0.9]), meta, (640, 480))
    assert rows[0][:4] == pytest.approx([100, 50, 200, 200], abs=0.01)
