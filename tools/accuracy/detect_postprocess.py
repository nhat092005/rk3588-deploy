"""Post-processing of the RKNN detectors of rknn_model_zoo, in numpy.

A port of ``post_process`` of ``examples/yolov8|yolov5|yolox/python``
of ``airockchip/rknn_model_zoo`` (tag v2.3.2, commit bad6c73): the
decode of each family and the NMS are the same, so the numbers of the
zoo are reproduced. Differences from the zoo, all on purpose:

* ``dfl`` of yolov8 uses numpy (the zoo uses torch, which the board
  does not have).
* the thresholds are those of an mAP run (confidence 0.001, NMS IoU
  0.65): the zoo has them in a comment and runs with 0.25 and 0.45.
* at most ``MAX_CANDIDATES`` boxes enter the NMS and ``MAX_DETECTIONS``
  leave it (the COCO evaluation reads the first 100 of an image).

The boxes come out in the 640x640 input, xyxy, and ``to_coco`` maps
them to the original picture as a COCO ``bbox``.
"""

import numpy as np

IMGSZ = 640
CONFIDENCE = 0.001
NMS_IOU = 0.65
MAX_CANDIDATES = 3000
MAX_DETECTIONS = 100
CLASSES = 80
DFL_BINS = 16
BOX_CHANNELS = 4
# The pixel offset that the zoo adds to the intersection of the NMS.
NMS_EPS = 0.00001
# The 80 COCO classes of the models are these 80 of the 91 COCO ids.
COCO_IDS = [
    *range(1, 12),
    *range(13, 26),
    27,
    28,
    *range(31, 45),
    *range(46, 66),
    67,
    70,
    *range(72, 83),
    *range(84, 91),
]
# examples/yolov5/model/anchors_yolov5.txt, reshape(3, -1, 2).
YOLOV5_ANCHORS = [
    [[10.0, 13.0], [16.0, 30.0], [33.0, 23.0]],
    [[30.0, 61.0], [62.0, 45.0], [59.0, 119.0]],
    [[116.0, 90.0], [156.0, 198.0], [373.0, 326.0]],
]
FAMILIES = ("yolov8", "yolov5", "yolox")


def _grid(height: int, width: int) -> tuple[np.ndarray, np.ndarray]:
    """Return the cell grid (1, 2, h, w) and the stride (1, 2, 1, 1)."""
    col, row = np.meshgrid(np.arange(width), np.arange(height))
    grid = np.stack([col, row])[None]
    stride = np.array([IMGSZ // height, IMGSZ // width]).reshape(1, 2, 1, 1)
    return grid, stride


def dfl(position: np.ndarray) -> np.ndarray:
    """Return the box distances (n, 4, h, w) of a DFL head.

    Softmax over the 16 bins of each side, then the expected bin.
    """
    count, channels, height, width = position.shape
    bins = channels // BOX_CHANNELS
    logits = position.reshape(count, BOX_CHANNELS, bins, height, width)
    logits = logits - logits.max(axis=2, keepdims=True)
    weights = np.exp(logits)
    weights /= weights.sum(axis=2, keepdims=True)
    ramp = np.arange(bins, dtype=np.float32).reshape(1, 1, bins, 1, 1)
    return (weights * ramp).sum(axis=2)


def _center_to_corners(box: np.ndarray) -> np.ndarray:
    """Convert (n, 4, h, w) centre x y w h to corners x1 y1 x2 y2."""
    corners = np.copy(box)
    corners[:, 0] = box[:, 0] - box[:, 2] / 2
    corners[:, 1] = box[:, 1] - box[:, 3] / 2
    corners[:, 2] = box[:, 0] + box[:, 2] / 2
    corners[:, 3] = box[:, 1] + box[:, 3] / 2
    return corners


def _flatten(tensor: np.ndarray) -> np.ndarray:
    """Return (n, h, w, c) of (n, c, h, w) as rows (n * h * w, c)."""
    return tensor.transpose(0, 2, 3, 1).reshape(-1, tensor.shape[1])


def decode_yolov8(
    outputs: list[np.ndarray],
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Decode 9 tensors: per level the DFL box, the classes, a sum.

    The sum of the scores (third tensor of a level) is ignored, as in
    the zoo. Return boxes (n, 4), objectness (n,) and class scores.
    """
    boxes, classes = [], []
    for level in range(len(outputs) // 3):
        position = outputs[3 * level]
        grid, stride = _grid(*position.shape[2:4])
        distances = dfl(position)
        top_left = (grid + 0.5 - distances[:, 0:2]) * stride
        bottom_right = (grid + 0.5 + distances[:, 2:4]) * stride
        boxes.append(
            _flatten(np.concatenate([top_left, bottom_right], axis=1))
        )
        classes.append(_flatten(outputs[3 * level + 1]))
    classes_all = np.concatenate(classes)
    return (
        np.concatenate(boxes),
        np.ones(len(classes_all), dtype=np.float32),
        classes_all,
    )


def decode_yolov5(
    outputs: list[np.ndarray],
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Decode 3 tensors (1, 255, h, w): 3 anchors of 85 values each."""
    boxes, objectness, classes = [], [], []
    for tensor, anchors in zip(outputs, YOLOV5_ANCHORS, strict=True):
        view = tensor.reshape(len(anchors), -1, *tensor.shape[-2:])
        grid, stride = _grid(*view.shape[2:4])
        scale = np.array(anchors).reshape(len(anchors), 2, 1, 1)
        center = (view[:, 0:2] * 2 - 0.5 + grid) * stride
        size = (view[:, 2:4] * 2) ** 2 * scale
        boxes.append(
            _flatten(
                _center_to_corners(np.concatenate([center, size], axis=1))
            )
        )
        objectness.append(_flatten(view[:, 4:5]))
        classes.append(_flatten(view[:, 5:]))
    return (
        np.concatenate(boxes),
        np.concatenate(objectness).reshape(-1),
        np.concatenate(classes),
    )


def decode_yolox(
    outputs: list[np.ndarray],
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Decode 3 tensors (1, 85, h, w): box, objectness, 80 classes."""
    boxes, objectness, classes = [], [], []
    for tensor in outputs:
        view = tensor.reshape(1, -1, *tensor.shape[-2:])
        grid, stride = _grid(*view.shape[2:4])
        center = (view[:, 0:2] + grid) * stride
        size = np.exp(view[:, 2:4]) * stride
        boxes.append(
            _flatten(
                _center_to_corners(np.concatenate([center, size], axis=1))
            )
        )
        objectness.append(_flatten(view[:, 4:5]))
        classes.append(_flatten(view[:, 5:]))
    return (
        np.concatenate(boxes),
        np.concatenate(objectness).reshape(-1),
        np.concatenate(classes),
    )


DECODERS = {
    "yolov8": decode_yolov8,
    "yolov5": decode_yolov5,
    "yolox": decode_yolox,
}


def nms(boxes: np.ndarray, scores: np.ndarray, iou: float) -> np.ndarray:
    """Return the indices kept by a greedy IoU NMS (as the zoo)."""
    width = boxes[:, 2] - boxes[:, 0]
    height = boxes[:, 3] - boxes[:, 1]
    areas = width * height
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        best = order[0]
        keep.append(best)
        rest = order[1:]
        left = np.maximum(boxes[best, 0], boxes[rest, 0])
        top = np.maximum(boxes[best, 1], boxes[rest, 1])
        right = np.minimum(boxes[best, 2], boxes[rest, 2])
        bottom = np.minimum(boxes[best, 3], boxes[rest, 3])
        inter = np.maximum(0.0, right - left + NMS_EPS) * np.maximum(
            0.0, bottom - top + NMS_EPS
        )
        overlap = inter / (areas[best] + areas[rest] - inter)
        order = rest[overlap <= iou]
    return np.array(keep, dtype=int)


def detect(
    family: str, outputs: list[np.ndarray]
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Return boxes (n, 4) in the input, class indices and scores.

    yolov8 and yolov5 suppress per class, yolox over all classes (as the
    zoo). The score is the objectness times the best class score.
    """
    boxes, objectness, classes = DECODERS[family](outputs)
    best = classes.argmax(axis=1)
    scores = classes.max(axis=1) * objectness
    keep = np.flatnonzero(scores >= CONFIDENCE)
    if len(keep) > MAX_CANDIDATES:
        keep = keep[np.argsort(scores[keep])[::-1][:MAX_CANDIDATES]]
    boxes, best, scores = boxes[keep], best[keep], scores[keep]
    if family == "yolox":
        groups = [np.arange(len(best))]
    else:
        groups = [np.flatnonzero(best == cls) for cls in np.unique(best)]
    kept = [
        index[nms(boxes[index], scores[index], NMS_IOU)]
        for index in groups
        if len(index)
    ]
    if not kept:
        empty = np.zeros(0)
        return np.zeros((0, 4)), empty.astype(int), empty
    order = np.concatenate(kept)
    order = order[np.argsort(scores[order])[::-1][:MAX_DETECTIONS]]
    return boxes[order], best[order], scores[order]


def to_coco(
    boxes: np.ndarray,
    classes: np.ndarray,
    scores: np.ndarray,
    meta: tuple[float, int, int],
    size: tuple[int, int],
) -> list[list[float]]:
    """Map detections of the input to rows of the original picture.

    ``meta`` is (gain, left, top) of the letterbox, ``size`` the
    (width, height) of the picture that the boxes are clipped to.
    Each row is [x, y, width, height, score, COCO category id].
    """
    gain, left, top = meta
    width, height = size
    rows = []
    for box, cls, score in zip(boxes, classes, scores, strict=True):
        x1 = float(np.clip((box[0] - left) / gain, 0, width))
        y1 = float(np.clip((box[1] - top) / gain, 0, height))
        x2 = float(np.clip((box[2] - left) / gain, 0, width))
        y2 = float(np.clip((box[3] - top) / gain, 0, height))
        if x2 > x1 and y2 > y1:
            rows.append(
                [
                    round(x1, 2),
                    round(y1, 2),
                    round(x2 - x1, 2),
                    round(y2 - y1, 2),
                    round(float(score), 5),
                    COCO_IDS[int(cls)],
                ]
            )
    return rows
