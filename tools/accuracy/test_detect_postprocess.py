"""Tests of detect_postprocess (run: pytest deploy/tools/accuracy)."""

import numpy as np
import pytest
from detect_postprocess import (
    CLASSES,
    COCO_IDS,
    DFL_BINS,
    IMGSZ,
    decode_yolov8,
    detect,
    dfl,
    nms,
    to_coco,
)

# coco_id_list of examples/yolov8/python/yolov8.py (zoo v2.3.2).
ZOO_IDS = [
    *[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15, 16, 17, 18, 19, 20],
    *[21, 22, 23, 24, 25, 27, 28, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40],
    *[41, 42, 43, 44, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58],
    *[59, 60, 61, 62, 63, 64, 65, 67, 70, 72, 73, 74, 75, 76, 77, 78, 79],
    *[80, 81, 82, 84, 85, 86, 87, 88, 89, 90],
]


def test_coco_ids_match_the_zoo() -> None:
    """The 80 classes map to the same COCO ids as the zoo."""
    assert len(COCO_IDS) == CLASSES
    assert ZOO_IDS == COCO_IDS


def test_dfl_is_the_expected_bin() -> None:
    """A peaked distribution gives its bin, a flat one the middle."""
    logits = np.zeros((1, 4 * DFL_BINS, 2, 3), dtype=np.float32)
    logits[0, 5, :, :] = 60.0  # side 0: bin 5
    distances = dfl(logits)
    assert distances.shape == (1, 4, 2, 3)
    assert distances[0, 0] == pytest.approx(5.0, abs=1e-4)
    assert distances[0, 1] == pytest.approx((DFL_BINS - 1) / 2)


def test_nms_keeps_the_best_of_an_overlap() -> None:
    """Of two boxes on the same place the higher score stays."""
    boxes = np.array(
        [[0, 0, 10, 10], [1, 1, 11, 11], [50, 50, 60, 60]], dtype=float
    )
    scores = np.array([0.5, 0.9, 0.4])
    assert sorted(nms(boxes, scores, 0.65)) == [1, 2]


def test_decode_yolov8_places_the_box_on_its_cell() -> None:
    """A cell with distances of 2 cells gives a box of 4 strides."""
    outputs = []
    for stride in (8, 16, 32):
        cells = IMGSZ // stride
        box = np.zeros((1, 4 * DFL_BINS, cells, cells), dtype=np.float32)
        box[0, [2, 16 + 2, 32 + 2, 48 + 2]] = 60.0  # every side: bin 2
        outputs += [
            box,
            np.zeros((1, CLASSES, cells, cells), dtype=np.float32),
            np.zeros((1, 1, cells, cells), dtype=np.float32),
        ]
    boxes, objectness, classes = decode_yolov8(outputs)
    assert classes.shape[1] == CLASSES
    assert len(boxes) == len(objectness) == 80 * 80 + 40 * 40 + 20 * 20
    # cell (0, 0) of the stride 8 level: centre 0.5, 2 cells each side
    assert boxes[0] == pytest.approx([-12.0, -12.0, 20.0, 20.0], abs=1e-3)


def test_detect_finds_one_object() -> None:
    """One strong class score gives one detection of that class."""
    outputs = []
    for stride in (8, 16, 32):
        cells = IMGSZ // stride
        box = np.zeros((1, 4 * DFL_BINS, cells, cells), dtype=np.float32)
        box[0, [2, 16 + 2, 32 + 2, 48 + 2]] = 60.0
        scores = np.full((1, CLASSES, cells, cells), -20.0, dtype=np.float32)
        outputs += [box, scores, np.zeros((1, 1, cells, cells), np.float32)]
    outputs[1][0, 7, 10, 10] = 0.9  # class 7 at the cell (10, 10), level 0
    boxes, classes, scores = detect("yolov8", outputs)
    assert classes.tolist() == [7]
    assert scores[0] == pytest.approx(0.9)
    assert boxes[0] == pytest.approx([68.0, 68.0, 100.0, 100.0], abs=1e-3)


def test_to_coco_maps_back_and_clips() -> None:
    """Remove the padding, divide by the gain, clip to the picture."""
    boxes = np.array([[30.0, 40.0, 130.0, 100.0], [0.0, 0.0, 5.0, 5.0]])
    rows = to_coco(
        boxes, np.array([0, 0]), np.array([0.8, 0.7]), (0.5, 10, 20), (50, 60)
    )
    # the second box is in the padding: clipped to nothing, dropped
    assert rows == [[40.0, 40.0, 10.0, 20.0, 0.8, 1]]
