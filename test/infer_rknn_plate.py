#!/usr/bin/env python3
"""RKNN YOLO + LPRNet plate pipeline for a single image or image folder.

The script is intentionally CPU-postprocess heavy: RKNN runs the two neural
networks, while DFL decode, NMS, plate crop, color/type estimation, and
constrained CTC beam search stay in Python for easy validation before C++ port.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

try:
    import cv2
except Exception as exc:  # pragma: no cover - OpenCV is required for real inference.
    cv2 = None
    CV2_IMPORT_ERROR = exc
else:
    CV2_IMPORT_ERROR = None
try:
    import numpy as np
except Exception as exc:  # pragma: no cover - NumPy is required for real inference.
    np = None
    NP_IMPORT_ERROR = exc
else:
    NP_IMPORT_ERROR = None

try:
    from rknnlite.api import RKNNLite
except Exception as exc:  # pragma: no cover - this script normally runs on RKNN boards.
    RKNNLite = None
    RKNN_IMPORT_ERROR = exc
else:
    RKNN_IMPORT_ERROR = None

try:
    from PIL import Image, ImageDraw, ImageFont
except Exception:
    Image = None
    ImageDraw = None
    ImageFont = None


YOLO_CLASSES = ("plate", "person", "car", "traffic_light")
STRIDES = (8, 16, 32)
IMG_SIZE = (640, 640)  # width, height
LPR_SIZE = (94, 24)    # width, height
FONT_CANDIDATES = (
    "C:/Windows/Fonts/NotoSansSC-Regular.ttf",
    "C:/Windows/Fonts/NotoSansSC-VF.ttf",
    "C:/Windows/Fonts/msyh.ttc",
    "C:/Windows/Fonts/simsun.ttc",
    "C:/Windows/Fonts/simhei.ttf",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.otf",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.otf",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
    "/usr/share/fonts/truetype/arphic/uming.ttc",
)

# Must match LPRNet_Pytorch/data/load_data.py for unified class_num=74.
CHARS = [
    "京", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "皖", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z",
    "学", "挂", "港", "澳", "使", "领", "警", "临",
    "-",
]
BLANK_INDEX = len(CHARS) - 1
PROVINCES = set(CHARS[:31])
DIGITS = set("0123456789")
LETTERS = set("ABCDEFGHJKLMNPQRSTUVWXYZ")
ALNUM = DIGITS | LETTERS
SPECIALS = set("学挂港澳使领警临")
DEFAULT_PROVINCE_TOPK = 5
DEFAULT_PROVINCE_TIMESTEPS = 2
DEFAULT_PROVINCE_SCORE_WEIGHT = 0.02

CCPD_PROVINCES = [
    "皖", "沪", "津", "渝", "冀", "晋", "蒙", "辽", "吉", "黑",
    "苏", "浙", "京", "闽", "赣", "鲁", "豫", "鄂", "湘", "粤",
    "桂", "琼", "川", "贵", "云", "藏", "陕", "甘", "青", "宁",
    "新", "警", "学", "O",
]
CCPD_ALPHABETS = [
    "A", "B", "C", "D", "E", "F", "G", "H", "J", "K",
    "L", "M", "N", "P", "Q", "R", "S", "T", "U", "V",
    "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5",
    "6", "7", "8", "9", "O",
]


@dataclass
class Detection:
    box: np.ndarray  # xyxy in original image coords
    class_id: int
    score: float


@dataclass
class PlateResult:
    box: List[int]
    detection_index: int
    det_score: float
    estimated_type: str
    decoded_type: str
    plate_subtype: str
    plate_type: str
    plate_text: str
    valid: bool
    invalid_reason: str
    lpr_score: float
    beam_score: float
    raw_text: str
    province_verify_status: str
    verifier_province: str
    verifier_conf: float
    verifier_top3: str
    crop_path: Optional[str]
    crop_width: int
    crop_height: int
    gt_text: Optional[str]
    match: Optional[bool]


class DebugWriter:
    def __init__(self, output_dir: Path, args: argparse.Namespace):
        self.debug_dir = output_dir / "debug"
        self.debug_dir.mkdir(parents=True, exist_ok=True)
        self.files = []
        self.image_writer = self._csv_writer("images.csv", [
            "image", "gt_text", "width", "height", "detection_count", "plate_count",
            "plate_texts", "best_plate_text", "best_lpr_score", "best_det_score",
            "yolo_pre_ms", "yolo_infer_ms", "yolo_post_ms", "lpr_total_ms",
            "total_ms", "vis_path",
        ])
        self.det_writer = self._csv_writer("detections.csv", [
            "image", "detection_index", "class_id", "class_name", "score",
            "x1", "y1", "x2", "y2", "width", "height", "area",
        ])
        self.plate_writer = self._csv_writer("plates.csv", [
            "image", "plate_index", "detection_index", "gt_text", "match",
            "det_score", "estimated_type", "decoded_type", "plate_subtype", "plate_type",
            "plate_text", "valid", "invalid_reason", "raw_text", "lpr_score", "beam_score", "crop_path",
            "province_verify_status", "verifier_province", "verifier_conf", "verifier_top3",
            "crop_width", "crop_height", "x1", "y1", "x2", "y2",
        ])
        self.candidate_writer = self._csv_writer("decode_candidates.csv", [
            "image", "plate_index", "candidate_rank", "selected", "estimated_type",
        "plate_type", "plate_subtype", "target_len", "text_len", "length_ok",
        "text", "lpr_score", "beam_score", "province_aware", "province_char",
            "province_rank", "province_conf", "province_score", "province_verify_status",
        ])
        self.results_jsonl = open(self.debug_dir / "results.jsonl", "w", encoding="utf-8")
        self.files.append(self.results_jsonl)
        self.write_config(args)

    def _csv_writer(self, name: str, fieldnames: List[str]) -> csv.DictWriter:
        handle = open(self.debug_dir / name, "w", newline="", encoding="utf-8-sig")
        self.files.append(handle)
        writer = csv.DictWriter(handle, fieldnames=fieldnames, extrasaction="ignore")
        writer.writeheader()
        return writer

    def write_config(self, args: argparse.Namespace) -> None:
        config = vars(args).copy()
        config.update({
            "classes": YOLO_CLASSES,
            "chars_len": len(CHARS),
            "blank_index": BLANK_INDEX,
            "img_size": IMG_SIZE,
            "lpr_size": LPR_SIZE,
        })
        with open(self.debug_dir / "run_config.json", "w", encoding="utf-8") as handle:
            json.dump(config, handle, ensure_ascii=False, indent=2)

    def write_jsonl(self, payload: Dict) -> None:
        self.results_jsonl.write(json.dumps(payload, ensure_ascii=False) + "\n")

    def flush(self) -> None:
        for handle in self.files:
            handle.flush()

    def close(self) -> None:
        for handle in self.files:
            handle.close()


def image_files(path: Path) -> Iterable[Path]:
    if path.is_file():
        yield path
        return
    for child in sorted(path.iterdir()):
        if child.suffix.lower() in {".jpg", ".jpeg", ".png", ".bmp"}:
            yield child


def parse_ccpd_gt(image_path: Path) -> Optional[str]:
    parts = image_path.stem.split("-")
    if len(parts) < 5:
        return None
    try:
        labels = [int(v) for v in parts[4].split("_")]
    except ValueError:
        return None
    if len(labels) < 2:
        return None
    if labels[0] >= len(CCPD_PROVINCES):
        return None
    chars = [CCPD_PROVINCES[labels[0]]]
    for idx in labels[1:]:
        if idx >= len(CCPD_ALPHABETS):
            return None
        chars.append(CCPD_ALPHABETS[idx])
    return "".join(chars)


def letterbox(im: np.ndarray, new_shape: Tuple[int, int] = IMG_SIZE,
              color: Tuple[int, int, int] = (114, 114, 114)) -> Tuple[np.ndarray, float, Tuple[float, float]]:
    src_h, src_w = im.shape[:2]
    dst_w, dst_h = new_shape
    ratio = min(dst_w / src_w, dst_h / src_h)
    resized_w, resized_h = int(round(src_w * ratio)), int(round(src_h * ratio))
    pad_w = (dst_w - resized_w) / 2
    pad_h = (dst_h - resized_h) / 2

    if (src_w, src_h) != (resized_w, resized_h):
        im = cv2.resize(im, (resized_w, resized_h), interpolation=cv2.INTER_LINEAR)
    top, bottom = int(round(pad_h - 0.1)), int(round(pad_h + 0.1))
    left, right = int(round(pad_w - 0.1)), int(round(pad_w + 0.1))
    im = cv2.copyMakeBorder(im, top, bottom, left, right, cv2.BORDER_CONSTANT, value=color)
    return im, ratio, (pad_w, pad_h)


def preprocess_yolo(frame_bgr: np.ndarray, color_order: str) -> Tuple[np.ndarray, float, Tuple[float, float]]:
    img, ratio, pad = letterbox(frame_bgr, IMG_SIZE)
    if color_order == "rgb":
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    return img[np.newaxis, :].astype(np.uint8), ratio, pad


def to_nchw(arr: np.ndarray, preferred_channels: Sequence[int]) -> np.ndarray:
    arr = np.asarray(arr)
    if arr.ndim == 3:
        arr = arr[np.newaxis, :]
    if arr.ndim != 4:
        raise ValueError(f"expected 4D output, got shape={arr.shape}")
    if arr.shape[1] in preferred_channels:
        return arr.astype(np.float32, copy=False)
    if arr.shape[-1] in preferred_channels:
        return arr.transpose(0, 3, 1, 2).astype(np.float32, copy=False)
    raise ValueError(f"cannot infer NCHW layout from shape={arr.shape}, channels={preferred_channels}")


def group_yolo_outputs(outputs: Sequence[np.ndarray], num_classes: int) -> List[Tuple[np.ndarray, np.ndarray, Optional[np.ndarray], int]]:
    if len(outputs) != 9:
        raise ValueError(f"YOLO RKNN should return 9 outputs, got {len(outputs)}")

    tensors = []
    for idx, out in enumerate(outputs):
        arr = np.asarray(out)
        if arr.ndim == 3:
            arr = arr[np.newaxis, :]
        shape = arr.shape
        channel = shape[1] if shape[1] in (64, num_classes, 1) else shape[-1]
        height = shape[2] if shape[1] in (64, num_classes, 1) else shape[1]
        width = shape[3] if shape[1] in (64, num_classes, 1) else shape[2]
        tensors.append((idx, arr, int(channel), int(height), int(width)))

    groups = []
    for stride in STRIDES:
        expected_h = IMG_SIZE[1] // stride
        expected_w = IMG_SIZE[0] // stride
        same_scale = [t for t in tensors if t[3] == expected_h and t[4] == expected_w]
        bbox = next((t for t in same_scale if t[2] == 64), None)
        cls = next((t for t in same_scale if t[2] == num_classes), None)
        score = next((t for t in same_scale if t[2] == 1), None)
        if bbox is None or cls is None:
            raise ValueError(f"missing bbox/cls output for stride={stride}; outputs={[t[1].shape for t in tensors]}")
        groups.append((
            to_nchw(bbox[1], [64]),
            to_nchw(cls[1], [num_classes]),
            to_nchw(score[1], [1]) if score is not None else None,
            stride,
        ))
    return groups


def dfl_decode(box_logits: np.ndarray) -> np.ndarray:
    # box_logits: [N, 64], 64 = 4 sides * 16 bins.
    bins = box_logits.reshape(-1, 4, 16)
    bins = bins - bins.max(axis=2, keepdims=True)
    probs = np.exp(bins)
    probs /= probs.sum(axis=2, keepdims=True)
    return (probs * np.arange(16, dtype=np.float32)).sum(axis=2)


def nms_xyxy(boxes: np.ndarray, scores: np.ndarray, threshold: float) -> np.ndarray:
    if len(boxes) == 0:
        return np.empty((0,), dtype=np.int64)
    x1, y1, x2, y2 = boxes.T
    areas = np.maximum(0.0, x2 - x1) * np.maximum(0.0, y2 - y1)
    order = scores.argsort()[::-1]
    keep = []
    while order.size > 0:
        i = int(order[0])
        keep.append(i)
        xx1 = np.maximum(x1[i], x1[order[1:]])
        yy1 = np.maximum(y1[i], y1[order[1:]])
        xx2 = np.minimum(x2[i], x2[order[1:]])
        yy2 = np.minimum(y2[i], y2[order[1:]])
        inter = np.maximum(0.0, xx2 - xx1) * np.maximum(0.0, yy2 - yy1)
        union = areas[i] + areas[order[1:]] - inter
        iou = inter / np.maximum(union, 1e-9)
        order = order[np.where(iou <= threshold)[0] + 1]
    return np.asarray(keep, dtype=np.int64)


def postprocess_yolo(outputs: Sequence[np.ndarray], ratio: float, pad: Tuple[float, float],
                     original_shape: Tuple[int, int], conf_thres: float,
                     nms_thres: float) -> List[Detection]:
    boxes_all, scores_all, classes_all = [], [], []
    groups = group_yolo_outputs(outputs, len(YOLO_CLASSES))

    for bbox_feat, cls_feat, cls_sum_feat, stride in groups:
        del cls_sum_feat  # cls_sum is useful for diagnostics; final score uses max(cls).
        _, _, h, w = cls_feat.shape
        cls = cls_feat.reshape(1, len(YOLO_CLASSES), -1)[0].T
        cls_max = cls.max(axis=1)
        mask = cls_max >= conf_thres
        if not np.any(mask):
            continue

        box_logits = bbox_feat.reshape(1, 64, -1)[0].T[mask]
        scores = cls_max[mask]
        classes = np.argmax(cls[mask], axis=1)
        dist = dfl_decode(box_logits)

        gx, gy = np.meshgrid(np.arange(w), np.arange(h))
        grid = np.stack([gx, gy], axis=-1).reshape(-1, 2).astype(np.float32)[mask]
        x1 = (grid[:, 0] + 0.5 - dist[:, 0]) * stride
        y1 = (grid[:, 1] + 0.5 - dist[:, 1]) * stride
        x2 = (grid[:, 0] + 0.5 + dist[:, 2]) * stride
        y2 = (grid[:, 1] + 0.5 + dist[:, 3]) * stride
        boxes_all.append(np.stack([x1, y1, x2, y2], axis=1))
        scores_all.append(scores.astype(np.float32))
        classes_all.append(classes.astype(np.int64))

    if not boxes_all:
        return []

    boxes = np.concatenate(boxes_all, axis=0)
    scores = np.concatenate(scores_all, axis=0)
    classes = np.concatenate(classes_all, axis=0)

    # Map 640x640 letterbox coords back to original image coords.
    boxes[:, [0, 2]] = (boxes[:, [0, 2]] - pad[0]) / ratio
    boxes[:, [1, 3]] = (boxes[:, [1, 3]] - pad[1]) / ratio
    orig_h, orig_w = original_shape
    boxes[:, [0, 2]] = np.clip(boxes[:, [0, 2]], 0, orig_w - 1)
    boxes[:, [1, 3]] = np.clip(boxes[:, [1, 3]], 0, orig_h - 1)

    detections = []
    for class_id in sorted(set(classes.tolist())):
        inds = np.where(classes == class_id)[0]
        keep = nms_xyxy(boxes[inds], scores[inds], nms_thres)
        for idx in inds[keep]:
            detections.append(Detection(box=boxes[idx], class_id=int(classes[idx]), score=float(scores[idx])))
    detections.sort(key=lambda d: (-d.score, d.class_id))
    return detections


def crop_with_padding(
    image: np.ndarray,
    box: Sequence[float],
    pad_left: float,
    pad_right: float,
    pad_top: float,
    pad_bottom: float,
) -> Optional[np.ndarray]:
    h, w = image.shape[:2]
    x1, y1, x2, y2 = [float(v) for v in box]
    bw, bh = x2 - x1, y2 - y1
    if bw < 2 or bh < 2:
        return None
    x1 = int(max(0, math.floor(x1 - bw * pad_left)))
    x2 = int(min(w, math.ceil(x2 + bw * pad_right)))
    y1 = int(max(0, math.floor(y1 - bh * pad_top)))
    y2 = int(min(h, math.ceil(y2 + bh * pad_bottom)))
    if x2 - x1 < 20 or y2 - y1 < 8:
        return None
    return image[y1:y2, x1:x2].copy()


def resolve_crop_padding(args: argparse.Namespace) -> Tuple[float, float, float, float]:
    pad_left = args.crop_pad_left if args.crop_pad_left is not None else args.crop_pad_x
    pad_right = args.crop_pad_right if args.crop_pad_right is not None else args.crop_pad_x
    pad_top = args.crop_pad_top if args.crop_pad_top is not None else args.crop_pad_y
    pad_bottom = args.crop_pad_bottom if args.crop_pad_bottom is not None else args.crop_pad_y
    return pad_left, pad_right, pad_top, pad_bottom


def estimate_plate_type(crop_bgr: np.ndarray) -> str:
    if crop_bgr.size == 0:
        return "unknown_7"
    h0, w0 = crop_bgr.shape[:2]
    x1 = int(w0 * 0.08)
    x2 = int(w0 * 0.92)
    y1 = int(h0 * 0.12)
    y2 = int(h0 * 0.88)
    roi = crop_bgr[y1:y2, x1:x2]
    if roi.size == 0:
        roi = crop_bgr
    hsv = cv2.cvtColor(roi, cv2.COLOR_BGR2HSV)
    h, s, v = cv2.split(hsv)
    area = float(hsv.shape[0] * hsv.shape[1])
    blue = np.count_nonzero((h >= 90) & (h <= 140) & (s >= 45) & (v >= 45)) / area
    green = np.count_nonzero((h >= 32) & (h <= 92) & (s >= 35) & (v >= 45)) / area
    yellow = np.count_nonzero((h >= 12) & (h <= 45) & (s >= 45) & (v >= 60)) / area
    dark = np.count_nonzero(v <= 65) / area
    if dark > 0.58 and max(blue, green, yellow) < 0.28:
        return "black"
    best_type, best_score = max(
        [("blue", blue), ("green", green), ("yellow", yellow)],
        key=lambda item: item[1],
    )
    if best_score >= 0.12:
        if best_type == "green" and blue >= 0.10 and blue > green * 0.75:
            return "blue"
        return best_type
    if dark > 0.55:
        return "black"
    return "unknown_7"


def plate_color_mask(crop_bgr: np.ndarray, plate_type: str) -> np.ndarray:
    hsv = cv2.cvtColor(crop_bgr, cv2.COLOR_BGR2HSV)
    h, s, v = cv2.split(hsv)
    if plate_type == "blue":
        mask = (h >= 90) & (h <= 140) & (s >= 35) & (v >= 35)
    elif plate_type == "green":
        mask = (h >= 30) & (h <= 95) & (s >= 25) & (v >= 35)
    elif plate_type == "yellow":
        mask = (h >= 12) & (h <= 45) & (s >= 35) & (v >= 55)
    elif plate_type == "black":
        mask = (v <= 90) & (s <= 120)
    else:
        blue = (h >= 90) & (h <= 140) & (s >= 35) & (v >= 35)
        green = (h >= 30) & (h <= 95) & (s >= 25) & (v >= 35)
        yellow = (h >= 12) & (h <= 45) & (s >= 35) & (v >= 55)
        mask = blue | green | yellow
    mask = mask.astype(np.uint8) * 255
    kernel = cv2.getStructuringElement(cv2.MORPH_RECT, (5, 3))
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=2)
    mask = cv2.morphologyEx(mask, cv2.MORPH_OPEN, kernel, iterations=1)
    return mask


def refine_plate_crop_by_color(crop_bgr: np.ndarray, plate_type: str, args: argparse.Namespace) -> np.ndarray:
    if not args.refine_plate_crop or crop_bgr.size == 0:
        return crop_bgr
    h, w = crop_bgr.shape[:2]
    if w < 30 or h < 10:
        return crop_bgr

    mask = plate_color_mask(crop_bgr, plate_type)
    contours, _ = cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    if not contours:
        return crop_bgr

    image_area = float(w * h)
    candidates = []
    for contour in contours:
        x, y, bw, bh = cv2.boundingRect(contour)
        area = float(bw * bh)
        if area < image_area * args.refine_min_area_ratio:
            continue
        aspect = bw / max(1.0, float(bh))
        if aspect < args.refine_min_aspect or aspect > args.refine_max_aspect:
            continue
        # Prefer wide plate-like components near the centre.  This avoids tiny
        # green/yellow vehicle-body patches stealing the crop.
        center_y = y + bh * 0.5
        center_penalty = abs(center_y - h * 0.5) / max(1.0, h)
        score = area * (1.0 + min(aspect, 5.0) * 0.08) - image_area * center_penalty * 0.08
        candidates.append((score, x, y, bw, bh))

    if not candidates:
        return crop_bgr

    _, x, y, bw, bh = max(candidates, key=lambda item: item[0])
    px1 = int(max(0, math.floor(x - bw * args.refine_pad_left)))
    px2 = int(min(w, math.ceil(x + bw + bw * args.refine_pad_right)))
    py1 = int(max(0, math.floor(y - bh * args.refine_pad_top)))
    py2 = int(min(h, math.ceil(y + bh + bh * args.refine_pad_bottom)))
    if px2 - px1 < 20 or py2 - py1 < 8:
        return crop_bgr

    refined = crop_bgr[py1:py2, px1:px2].copy()
    refined_area = float(refined.shape[0] * refined.shape[1])
    if refined_area <= 0 or refined_area > image_area * args.refine_max_area_ratio:
        return crop_bgr
    return refined


def refine_plate_crop_aspect(crop_bgr: np.ndarray, plate_type: str, args: argparse.Namespace) -> np.ndarray:
    if not args.refine_plate_aspect or crop_bgr.size == 0:
        return crop_bgr
    if plate_type not in {"blue", "green", "yellow"}:
        return crop_bgr
    h, w = crop_bgr.shape[:2]
    if w < 30 or h < 10:
        return crop_bgr
    aspect = w / max(1.0, float(h))
    if aspect >= args.refine_aspect_min:
        return crop_bgr

    target_h = int(round(w / max(args.refine_aspect_target, 1.0)))
    target_h = max(8, min(h, target_h))
    if target_h >= h:
        return crop_bgr

    mask = plate_color_mask(crop_bgr, plate_type) > 0
    row_density = mask.mean(axis=1).astype(np.float32)
    if float(row_density.max()) > 0.05:
        weights = row_density + 1e-3
        center = int(round(float(np.sum(np.arange(h) * weights) / np.sum(weights))))
    else:
        center = h // 2

    y1 = max(0, center - target_h // 2)
    y2 = min(h, y1 + target_h)
    y1 = max(0, y2 - target_h)
    if y2 - y1 < 8:
        return crop_bgr
    return crop_bgr[y1:y2, :].copy()


def target_len_for_type(plate_type: str, plate_subtype: Optional[str] = None) -> int:
    if plate_type in {"green", "unknown_8"} or plate_subtype == "tractor_green":
        return 8
    return 7


def char_allowed(ch: str, pos: int, plate_type: str, plate_subtype: Optional[str]) -> bool:
    if pos == 0:
        return ch in PROVINCES
    if pos == 1:
        if plate_type == "special_7":
            return ch in ALNUM
        return ch in LETTERS
    if plate_type == "black":
        return ch in ALNUM or ch in SPECIALS
    if plate_subtype == "tractor_green":
        return ch in ALNUM or ch in {"学", "挂"}
    if plate_type == "yellow":
        return ch in ALNUM or ch in {"学", "挂"}
    if plate_type == "special_7":
        return ch in ALNUM or ch in SPECIALS
    return ch in ALNUM


def is_valid_prefix(prefix: Tuple[int, ...], plate_type: str, plate_subtype: Optional[str]) -> bool:
    target_len = target_len_for_type(plate_type, plate_subtype)
    if len(prefix) > target_len:
        return False
    for pos, idx in enumerate(prefix):
        if idx == BLANK_INDEX:
            return False
        if not char_allowed(CHARS[idx], pos, plate_type, plate_subtype):
            return False
    return True


def log_softmax_time(logits: np.ndarray) -> np.ndarray:
    logits = logits.astype(np.float32, copy=False)
    logits = logits - logits.max(axis=0, keepdims=True)
    logsum = np.log(np.exp(logits).sum(axis=0, keepdims=True) + 1e-12)
    return logits - logsum


def ctc_collapse(indices: Sequence[int]) -> Tuple[int, ...]:
    collapsed: List[int] = []
    prev = BLANK_INDEX
    for idx in indices:
        idx = int(idx)
        if idx == BLANK_INDEX:
            prev = BLANK_INDEX
            continue
        if idx != prev:
            collapsed.append(idx)
        prev = idx
    return tuple(collapsed)


def normalize_lpr_logits(logits: np.ndarray) -> np.ndarray:
    logits = np.asarray(logits)
    if logits.ndim == 3:
        logits = logits[0]
    if logits.ndim != 2:
        raise ValueError(f"LPR logits should be 2D/3D, got shape={logits.shape}")
    if logits.shape[0] == len(CHARS):
        return logits
    if logits.shape[1] == len(CHARS):
        return logits.T
    raise ValueError(f"LPR logits class dim should be {len(CHARS)}, got shape={logits.shape}")


def greedy_debug_decode(logits: np.ndarray) -> Tuple[str, float]:
    logits = normalize_lpr_logits(logits)
    log_probs = log_softmax_time(logits)
    raw = np.argmax(logits, axis=0)
    collapsed = ctc_collapse(raw)
    text = "".join(CHARS[i] for i in collapsed)
    conf = float(np.exp(log_probs[raw, np.arange(log_probs.shape[1])]).mean()) if raw.size else 0.0
    return text, conf


def prefix_matches_forced(prefix: Tuple[int, ...], forced_prefix: Tuple[int, ...]) -> bool:
    if not forced_prefix:
        return True
    if len(prefix) <= len(forced_prefix):
        return prefix == forced_prefix[:len(prefix)]
    return prefix[:len(forced_prefix)] == forced_prefix


def constrained_ctc_decode_one(logits: np.ndarray, plate_type: str, plate_subtype: Optional[str],
                               beam_width: int, beam_topk: int,
                               forced_prefix: Tuple[int, ...] = tuple()) -> Tuple[str, float, float]:
    logits = normalize_lpr_logits(logits)
    log_probs = log_softmax_time(logits)
    target_len = target_len_for_type(plate_type, plate_subtype)
    forced_prefix = tuple(int(idx) for idx in forced_prefix)
    if forced_prefix:
        if len(forced_prefix) > target_len or not is_valid_prefix(forced_prefix, plate_type, plate_subtype):
            return "", 0.0, float("-inf")

    beams: Dict[Tuple[int, ...], float] = {tuple(): 0.0}

    for t in range(log_probs.shape[1]):
        top = np.argsort(log_probs[:, t])[-beam_topk:].tolist()
        if BLANK_INDEX not in top:
            top.append(BLANK_INDEX)
        for idx in forced_prefix:
            if idx not in top:
                top.append(idx)
        next_beams: Dict[Tuple[int, ...], float] = {}
        for raw_seq, score in beams.items():
            for idx in top:
                new_raw = raw_seq + (int(idx),)
                prefix = ctc_collapse(new_raw)
                if len(prefix) > target_len:
                    continue
                if forced_prefix and not prefix_matches_forced(prefix, forced_prefix):
                    continue
                if not is_valid_prefix(prefix, plate_type, plate_subtype):
                    continue
                new_score = score + float(log_probs[idx, t])
                prev_score = next_beams.get(new_raw)
                if prev_score is None or new_score > prev_score:
                    next_beams[new_raw] = new_score
        if not next_beams:
            next_beams = beams
        ordered = sorted(next_beams.items(), key=lambda item: item[1], reverse=True)
        beams = dict(ordered[:beam_width])

    candidates = []
    for raw_seq, score in beams.items():
        prefix = ctc_collapse(raw_seq)
        if forced_prefix and not prefix_matches_forced(prefix, forced_prefix):
            continue
        if len(prefix) == target_len and is_valid_prefix(prefix, plate_type, plate_subtype):
            candidates.append((prefix, score))
    if not candidates:
        for raw_seq, score in beams.items():
            prefix = ctc_collapse(raw_seq)
            if forced_prefix and not prefix_matches_forced(prefix, forced_prefix):
                continue
            if prefix and is_valid_prefix(prefix, plate_type, plate_subtype):
                candidates.append((prefix, score))
    if not candidates:
        return "", 0.0, float("-inf")

    prefix, score = max(candidates, key=lambda item: item[1])
    text = "".join(CHARS[i] for i in prefix)
    norm_log_score = score / max(1, logits.shape[1])
    return text, float(math.exp(norm_log_score)), float(score)


def logsumexp(values: np.ndarray, axis: Optional[int] = None) -> np.ndarray:
    max_value = np.max(values, axis=axis, keepdims=True)
    summed = np.log(np.exp(values - max_value).sum(axis=axis, keepdims=True) + 1e-12) + max_value
    if axis is None:
        return np.squeeze(summed)
    return np.squeeze(summed, axis=axis)


def province_scores_from_logits(logits: np.ndarray, timesteps: int) -> np.ndarray:
    logits = normalize_lpr_logits(logits)
    log_probs = log_softmax_time(logits)
    window = max(1, min(int(timesteps), log_probs.shape[1]))
    province_log_probs = log_probs[:31, :window]
    # CTC may place the province on any early timestep, so logsumexp is less
    # brittle than requiring the same province to dominate every timestep.
    return logsumexp(province_log_probs, axis=1) - math.log(window)


def province_confidence(logits: np.ndarray, timesteps: int) -> float:
    scores = province_scores_from_logits(logits, timesteps)
    return float(math.exp(float(np.max(scores))))


def province_idx_from_text(text: str) -> Optional[int]:
    if not text:
        return None
    try:
        idx = CHARS.index(text[0])
    except ValueError:
        return None
    return idx if 0 <= idx < 31 else None


def first_char_is_province(text: str) -> bool:
    return bool(text) and text[0] in PROVINCES


def _province_votes_from_candidates(candidates: List[Dict]) -> Optional[float]:
    """Return fraction of length-OK candidates whose province agrees with the selected one.

    Returns None when there are fewer than 2 length-OK candidates (not enough signal).
    A value of 1.0 means all decodes agree on the province; 0.2 means only 1 out of 5.
    """
    ok = [c for c in candidates if c.get("text") and c.get("length_ok")]
    if len(ok) < 2:
        return None
    selected = next((c for c in ok if c.get("selected")), None)
    if selected is None:
        return None
    selected_prov = selected.get("province_char") or selected.get("text", "")[:1]
    if not selected_prov:
        return None
    agree = sum(1 for c in ok if (c.get("province_char") or c.get("text", "")[:1]) == selected_prov)
    return agree / len(ok)


def province_aware_combined_score(prob: float, province_score: Optional[float], province_score_weight: float) -> float:
    score = math.log(max(float(prob), 1e-12))
    if province_score is not None:
        score += float(province_score_weight) * float(province_score)
    return score


def province_aware_ctc_decode_one(logits: np.ndarray, plate_type: str, plate_subtype: Optional[str],
                                  beam_width: int, beam_topk: int, province_topk: int,
                                  province_timesteps: int, province_score_weight: float,
                                  locked_province_idx: Optional[int] = None) -> Tuple[str, float, float, Dict]:
    scores = province_scores_from_logits(logits, province_timesteps)
    province_conf = float(math.exp(float(np.max(scores))))
    top_count = max(1, min(int(province_topk), len(scores)))
    top_provinces = np.argsort(scores)[-top_count:][::-1].tolist()

    if locked_province_idx is not None:
        locked_province_idx = int(locked_province_idx)
        text, prob, raw_score = constrained_ctc_decode_one(
            logits, plate_type, plate_subtype, beam_width, beam_topk,
            forced_prefix=(locked_province_idx,)
        )
        province_score = float(scores[locked_province_idx]) if 0 <= locked_province_idx < len(scores) else None
        return text, prob, raw_score, {
            "province_aware": True,
            "province_char": CHARS[locked_province_idx] if 0 <= locked_province_idx < 31 else "",
            "province_rank": "raw_locked",
            "province_conf": province_conf,
            "province_score": province_score if province_score is not None else "",
        }

    base_text, base_prob, base_raw_score = constrained_ctc_decode_one(
        logits, plate_type, plate_subtype, beam_width, beam_topk
    )
    base_idx = province_idx_from_text(base_text)
    base_province_score = float(scores[base_idx]) if base_idx is not None else None
    best = (base_text, base_prob, base_raw_score, {
        "province_aware": True,
        "province_char": CHARS[base_idx] if base_idx is not None else (base_text[:1] if base_text else ""),
        "province_rank": "baseline",
        "province_conf": province_conf,
        "province_score": base_province_score if base_province_score is not None else "",
    })
    best_combined = province_aware_combined_score(base_prob, base_province_score, province_score_weight)

    for rank, p_idx in enumerate(top_provinces):
        if base_idx == p_idx:
            continue
        forced_prefix = (int(p_idx),)
        if not is_valid_prefix(forced_prefix, plate_type, plate_subtype):
            continue
        text, prob, raw_score = constrained_ctc_decode_one(
            logits, plate_type, plate_subtype, beam_width, beam_topk, forced_prefix=forced_prefix
        )
        if not text:
            continue
        province_score = float(scores[p_idx])
        combined = province_aware_combined_score(prob, province_score, province_score_weight)
        if combined > best_combined:
            best_combined = combined
            best = (text, prob, raw_score, {
                "province_aware": True,
                "province_char": CHARS[p_idx],
                "province_rank": rank,
                "province_conf": province_conf,
                "province_score": province_score,
            })

    return best


def candidate_special_count(text: str) -> int:
    return sum(1 for ch in text if ch in SPECIALS)


def candidate_is_plausible(candidate: Dict) -> bool:
    text = candidate.get("text", "")
    if not text or not candidate.get("length_ok"):
        return False
    special_count = candidate_special_count(text)
    if special_count > 1:
        return False
    if candidate.get("plate_type") not in {"black", "special_7", "yellow"}:
        return special_count == 0
    if candidate.get("plate_type") == "yellow":
        return all(ch not in set("港澳使领警临") for ch in text)
    return True


def candidate_selection_score(candidate: Dict, estimated_type: str, province_score_weight: float) -> float:
    """Score a decode candidate for best-of-N selection across plate-type attempts.

    Uses *normalised* lpr_score (exp of per-timestep-average log-prob) so that
    7-char and 8-char decodes are comparable.  Raw beam_score is a sum over
    time-steps that naturally penalises longer sequences — it must never be used
    for cross-type comparison.
    """
    # Primary signal: per-timestep confidence, already comparable across lengths.
    score = float(candidate.get("lpr_score", 0.0))

    # Penalise special characters that are implausible for the plate type.
    score -= 0.15 * candidate_special_count(candidate.get("text", ""))

    # Also keep province confidence in the final cross-type selection.  Without
    # this, a type bonus can still choose a candidate whose first character has
    # weak early-timestep support.
    province_score = candidate.get("province_score")
    if province_score not in (None, ""):
        score += float(province_score_weight) * float(province_score)

    # Bonus for matching the colour-estimated type — helps keep green plates
    # green when a 7-char decode has only marginally higher confidence.
    cand_type = candidate.get("plate_type", "")
    if not estimated_type.startswith("unknown"):
        if cand_type == estimated_type:
            score += 0.08
        elif cand_type == "black":
            # Black is easily confused with other types; strong penalty.
            score -= 0.25
        else:
            # Mild penalty for any other type mismatch.
            score -= 0.05

    return score


def decode_with_type_fallback(logits: np.ndarray, estimated_type: str, beam_width: int,
                              beam_topk: int, province_aware: bool = True,
                              province_topk: int = DEFAULT_PROVINCE_TOPK,
                              province_timesteps: int = DEFAULT_PROVINCE_TIMESTEPS,
                              province_score_weight: float = DEFAULT_PROVINCE_SCORE_WEIGHT,
                              locked_province_idx: Optional[int] = None) -> Tuple[str, float, float, str, str, List[Dict]]:
    """Run constrained CTC decode for each candidate plate type and select the best.

    The order matters for two reasons:
    1. The *first* length-OK plausible candidate that is selected wins the
       \"selected\" flag, but the *best* across all attempts is what the caller
       uses — so ordering only affects the debug CSV, not the final result.
    2. When estimated_type is known, start with that type + its variants, then
       fall back through all others so the user can see in the debug output
       which type produced the winning decode.
    """
    # Canonical list of all plate types we consider.
    ALL_ATTEMPTS = [
        ("blue", None),
        ("green", None),
        ("green", "tractor_green"),
        ("yellow", None),
        ("black", None),
        ("special_7", None),
        ("unknown_8", None),
    ]

    if estimated_type.startswith("unknown"):
        attempts = list(ALL_ATTEMPTS)
    else:
        # Put estimated_type + its variants first; then all others in order.
        attempts = [(estimated_type, None)]
        if estimated_type == "green":
            attempts.append(("green", "tractor_green"))
        for attempt in ALL_ATTEMPTS:
            if attempt not in attempts:
                attempts.append(attempt)

    seen = set()
    candidates: List[Dict] = []
    for plate_type, subtype in attempts:
        key = (plate_type, subtype)
        if key in seen:
            continue
        seen.add(key)
        if province_aware:
            text, prob, score, province_info = province_aware_ctc_decode_one(
                logits, plate_type, subtype, beam_width, beam_topk,
                province_topk, province_timesteps, province_score_weight,
                locked_province_idx
            )
        else:
            forced_prefix = (locked_province_idx,) if locked_province_idx is not None else tuple()
            text, prob, score = constrained_ctc_decode_one(
                logits, plate_type, subtype, beam_width, beam_topk, forced_prefix=forced_prefix
            )
            scores = province_scores_from_logits(logits, province_timesteps)
            province_idx = province_idx_from_text(text)
            province_info = {
                "province_aware": False,
                "province_char": text[:1] if text else "",
                "province_rank": "",
                "province_conf": float(math.exp(float(np.max(scores)))),
                "province_score": float(scores[province_idx]) if province_idx is not None else "",
            }
        target_len = target_len_for_type(plate_type, subtype)
        candidate = {
            "candidate_rank": len(candidates),
            "selected": False,
            "estimated_type": estimated_type,
            "plate_type": plate_type,
            "plate_subtype": subtype or "",
            "target_len": target_len,
            "text_len": len(text),
            "length_ok": len(text) == target_len,
            "text": text,
            "lpr_score": prob,
            "beam_score": score,
        }
        candidate.update(province_info)
        candidates.append(candidate)

    valid_candidates = [
        (idx, candidate) for idx, candidate in enumerate(candidates)
        if candidate["text"] and candidate["length_ok"]
    ]
    plausible_candidates = [
        (idx, candidate) for idx, candidate in valid_candidates
        if candidate_is_plausible(candidate)
    ]
    selectable = plausible_candidates or valid_candidates or [
        (idx, candidate) for idx, candidate in enumerate(candidates)
        if candidate["text"]
    ]
    if not selectable:
        return "", 0.0, float("-inf"), estimated_type, "", candidates

    best_idx, best_candidate = max(
        selectable,
        key=lambda item: candidate_selection_score(item[1], estimated_type, province_score_weight),
    )
    if best_idx >= 0:
        candidates[best_idx]["selected"] = True
    return (
        best_candidate["text"],
        best_candidate["lpr_score"],
        best_candidate["beam_score"],
        best_candidate["plate_type"],
        best_candidate["plate_subtype"],
        candidates,
    )


def preprocess_lpr(crop_bgr: np.ndarray, color_order: str) -> np.ndarray:
    img = cv2.resize(crop_bgr, LPR_SIZE, interpolation=cv2.INTER_LINEAR)
    if color_order == "rgb":
        img = cv2.cvtColor(img, cv2.COLOR_BGR2RGB)
    return img[np.newaxis, :].astype(np.uint8)


def preprocess_province(crop_bgr: np.ndarray, args: argparse.Namespace) -> np.ndarray:
    img = cv2.resize(crop_bgr, LPR_SIZE, interpolation=cv2.INTER_LINEAR)
    h, w = img.shape[:2]
    left_ratio = max(0.2, min(0.8, float(args.province_left_ratio)))
    crop_w = max(8, min(w, int(round(w * left_ratio))))
    left = img[:, :crop_w]
    left = cv2.resize(
        left,
        (int(args.province_input_width), int(args.province_input_height)),
        interpolation=cv2.INTER_LINEAR,
    )
    if args.province_color == "rgb":
        left = cv2.cvtColor(left, cv2.COLOR_BGR2RGB)
    return left[np.newaxis, :].astype(np.uint8)


def softmax_1d(values: np.ndarray) -> np.ndarray:
    values = values.astype(np.float32).reshape(-1)
    values = values - np.max(values)
    exp_values = np.exp(values)
    return exp_values / max(float(np.sum(exp_values)), 1e-12)


def province_logits_to_probs(output) -> np.ndarray:
    arr = np.asarray(output)
    arr = np.squeeze(arr)
    if arr.size != 31:
        arr = arr.reshape(-1)[:31]
    return softmax_1d(arr)


def format_province_topk(probs: np.ndarray, topk: int = 3) -> str:
    indexes = np.argsort(probs)[-max(1, topk):][::-1]
    return "|".join(f"{CHARS[int(i)]}:{float(probs[int(i)]):.4f}" for i in indexes)


def verify_or_correct_province(args: argparse.Namespace, province_model, crop_bgr: np.ndarray,
                               logits: np.ndarray, text: str, lpr_score: float, beam_score: float,
                               decoded_type: str, subtype: str, estimated_type: str,
                               candidates: List[Dict]) -> Tuple[str, float, float, str, str, List[Dict], Dict]:
    info = {
        "province_verify_status": "disabled",
        "verifier_province": "",
        "verifier_conf": 0.0,
        "verifier_top3": "",
    }
    if province_model is None or not args.province_verify:
        return text, lpr_score, beam_score, decoded_type, subtype, candidates, info

    province_input = preprocess_province(crop_bgr, args)
    output = province_model.inference(inputs=[province_input])[0]
    probs = province_logits_to_probs(output)
    top_idx = int(np.argmax(probs))
    top_conf = float(probs[top_idx])
    top_province = CHARS[top_idx]
    top_indexes = np.argsort(probs)[-max(1, int(args.province_agree_topk)):][::-1].tolist()
    current_idx = province_idx_from_text(text)

    info.update({
        "province_verify_status": "no_text" if not text else "disputed_low_conf",
        "verifier_province": top_province,
        "verifier_conf": top_conf,
        "verifier_top3": format_province_topk(probs, 3),
    })
    if not text:
        return text, lpr_score, beam_score, decoded_type, subtype, candidates, info

    if current_idx == top_idx:
        info["province_verify_status"] = "agree"
        return text, lpr_score, beam_score, decoded_type, subtype, candidates, info

    if current_idx is not None and current_idx in top_indexes:
        info["province_verify_status"] = "topk_agree"
        return text, lpr_score, beam_score, decoded_type, subtype, candidates, info

    if top_conf < args.province_correct_min_conf:
        return text, lpr_score, beam_score, decoded_type, subtype, candidates, info

    forced_text, forced_lpr_score, forced_beam_score, forced_type, forced_subtype, forced_candidates = decode_with_type_fallback(
        logits, estimated_type, args.beam_width, args.beam_topk,
        args.province_aware_decode, args.province_topk,
        args.province_timesteps, args.province_score_weight,
        locked_province_idx=top_idx,
    )
    forced_selected = next((c for c in forced_candidates if c.get("selected")), None)
    forced_len_ok = bool(forced_text) and bool(forced_selected and forced_selected.get("length_ok"))
    min_score = float(lpr_score) * float(args.province_correction_min_lpr_ratio)
    if forced_len_ok and forced_lpr_score >= min_score:
        for candidate in candidates:
            candidate["selected"] = False
        for candidate in forced_candidates:
            candidate["candidate_rank"] = len(candidates)
            candidate["province_verify_status"] = "forced_by_classifier"
            candidates.append(candidate)
        info["province_verify_status"] = "corrected"
        return forced_text, forced_lpr_score, forced_beam_score, forced_type, forced_subtype, candidates, info

    info["province_verify_status"] = "correction_rejected"
    return text, lpr_score, beam_score, decoded_type, subtype, candidates, info


def make_lpr_debug_image(crop_bgr: np.ndarray, color_order: str) -> np.ndarray:
    img = cv2.resize(crop_bgr, LPR_SIZE, interpolation=cv2.INTER_LINEAR)
    if color_order == "rgb":
        return cv2.cvtColor(img, cv2.COLOR_RGB2BGR)
    return img


def load_draw_font(font_path: Optional[str], size: int):
    if ImageFont is None:
        return None
    candidates = [font_path] if font_path else []
    candidates.extend(FONT_CANDIDATES)
    for candidate in candidates:
        if candidate and Path(candidate).exists():
            try:
                return ImageFont.truetype(candidate, size=size)
            except Exception:
                continue
    try:
        return ImageFont.load_default()
    except Exception:
        return None


def draw_text_utf8(image: np.ndarray, text: str, pos: Tuple[int, int], color: Tuple[int, int, int],
                   font_path: Optional[str], size: int = 22) -> np.ndarray:
    if not text:
        return image
    if Image is None or ImageDraw is None:
        safe_text = text.encode("ascii", errors="replace").decode("ascii")
        cv2.putText(image, safe_text, pos, cv2.FONT_HERSHEY_SIMPLEX, 0.7, color, 2, cv2.LINE_AA)
        return image

    font = load_draw_font(font_path, size)
    rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    pil_img = Image.fromarray(rgb)
    draw = ImageDraw.Draw(pil_img)
    bgr = color
    draw.text(pos, text, fill=(bgr[2], bgr[1], bgr[0]), font=font)
    return cv2.cvtColor(np.asarray(pil_img), cv2.COLOR_RGB2BGR)


def draw_detections(image: np.ndarray, detections: Sequence[Detection], plates: Sequence[PlateResult],
                    font_path: Optional[str] = None) -> np.ndarray:
    vis = image.copy()
    colors = {
        0: (0, 220, 0),
        1: (255, 160, 0),
        2: (0, 180, 255),
        3: (0, 0, 255),
    }
    image_h, image_w = vis.shape[:2]
    for det in detections:
        x1, y1, x2, y2 = [int(round(v)) for v in det.box]
        color = colors.get(det.class_id, (220, 220, 220))
        cv2.rectangle(vis, (x1, y1), (x2, y2), color, 2)
        if det.class_id == 0:
            continue
        label = f"{YOLO_CLASSES[det.class_id]} {det.score:.2f}"
        cv2.putText(vis, label, (x1, max(18, y1 - 6)), cv2.FONT_HERSHEY_SIMPLEX, 0.55, color, 2, cv2.LINE_AA)
    for plate in plates:
        x1, y1, _, y2 = plate.box
        x1 = max(0, min(x1, image_w - 1))
        status = "" if plate.valid else " INVALID"
        label = f"{plate.plate_text or 'INVALID'} {plate.lpr_score:.2f} [{plate.plate_type}]{status}"
        label_y = y2 + 6
        if label_y + 28 > image_h:
            label_y = max(0, y1 - 30)
        vis = draw_text_utf8(vis, label, (x1, label_y), (40, 255, 40), font_path)
    return vis


def load_rknn(path: str, core_mask: Optional[int]) -> RKNNLite:
    if RKNNLite is None:
        raise RuntimeError(f"rknnlite import failed: {RKNN_IMPORT_ERROR}")
    model = RKNNLite(verbose=False)
    ret = model.load_rknn(path)
    if ret != 0:
        raise RuntimeError(f"load_rknn failed: {path}")
    if core_mask is None:
        ret = model.init_runtime()
    else:
        ret = model.init_runtime(core_mask=core_mask)
    if ret != 0:
        raise RuntimeError(f"init_runtime failed: {path}")
    return model


def parse_core_mask(name: str) -> Optional[int]:
    if RKNNLite is None or name == "default":
        return None
    mapping = {
        "core0": RKNNLite.NPU_CORE_0,
        "core1": RKNNLite.NPU_CORE_1,
        "core2": RKNNLite.NPU_CORE_2,
        "core01": RKNNLite.NPU_CORE_0_1,
        "core012": RKNNLite.NPU_CORE_0_1_2,
        "auto": RKNNLite.NPU_CORE_AUTO,
    }
    return mapping[name]


def write_debug_rows(debug: Optional[DebugWriter], image_path: Path, gt_text: Optional[str],
                     frame_shape: Tuple[int, int], detections: Sequence[Detection],
                     plates: Sequence[PlateResult], candidates_by_plate: Dict[int, List[Dict]],
                     timings: Dict[str, float], vis_path: Optional[str]) -> None:
    if debug is None:
        return

    image = str(image_path)
    height, width = frame_shape
    valid_plates = [p for p in plates if p.valid]
    best_plate = max(valid_plates, key=lambda p: p.lpr_score, default=None)
    debug.image_writer.writerow({
        "image": image,
        "gt_text": gt_text or "",
        "width": width,
        "height": height,
        "detection_count": len(detections),
        "plate_count": len(plates),
        "plate_texts": "|".join(p.plate_text for p in plates if p.valid),
        "best_plate_text": best_plate.plate_text if best_plate and best_plate.valid else "",
        "best_lpr_score": best_plate.lpr_score if best_plate else "",
        "best_det_score": best_plate.det_score if best_plate else "",
        "yolo_pre_ms": timings.get("yolo_pre_ms", 0.0),
        "yolo_infer_ms": timings.get("yolo_infer_ms", 0.0),
        "yolo_post_ms": timings.get("yolo_post_ms", 0.0),
        "lpr_total_ms": timings.get("lpr_total_ms", 0.0),
        "total_ms": timings.get("total_ms", 0.0),
        "vis_path": vis_path or "",
    })

    for det_idx, det in enumerate(detections):
        x1, y1, x2, y2 = [int(round(v)) for v in det.box]
        w = max(0, x2 - x1)
        h = max(0, y2 - y1)
        debug.det_writer.writerow({
            "image": image,
            "detection_index": det_idx,
            "class_id": det.class_id,
            "class_name": YOLO_CLASSES[det.class_id],
            "score": det.score,
            "x1": x1,
            "y1": y1,
            "x2": x2,
            "y2": y2,
            "width": w,
            "height": h,
            "area": w * h,
        })

    for plate_idx, plate in enumerate(plates):
        row = asdict(plate)
        x1, y1, x2, y2 = plate.box
        row.update({
            "image": image,
            "plate_index": plate_idx,
            "x1": x1,
            "y1": y1,
            "x2": x2,
            "y2": y2,
        })
        debug.plate_writer.writerow(row)
        for candidate in candidates_by_plate.get(plate_idx, []):
            c_row = candidate.copy()
            c_row.update({"image": image, "plate_index": plate_idx})
            debug.candidate_writer.writerow(c_row)
    debug.flush()


def run_image(args, yolo, lpr, province, image_path: Path, output_dir: Path,
              debug: Optional[DebugWriter] = None) -> List[PlateResult]:
    total_start = time.perf_counter()
    frame = cv2.imread(str(image_path))
    if frame is None:
        print(f"skip unreadable image: {image_path}")
        return []

    gt_text = parse_ccpd_gt(image_path)
    t0 = time.perf_counter()
    yolo_input, ratio, pad = preprocess_yolo(frame, args.yolo_color)
    yolo_pre_ms = (time.perf_counter() - t0) * 1000.0

    t0 = time.perf_counter()
    yolo_outputs = yolo.inference(inputs=[yolo_input])
    yolo_infer_ms = (time.perf_counter() - t0) * 1000.0

    t0 = time.perf_counter()
    detections = postprocess_yolo(yolo_outputs, ratio, pad, frame.shape[:2], args.conf_thres, args.nms_thres)
    yolo_post_ms = (time.perf_counter() - t0) * 1000.0

    crop_dir = output_dir / "crops"
    if args.save_crops:
        crop_dir.mkdir(parents=True, exist_ok=True)
    lpr_input_dir = output_dir / "lpr_inputs"
    if args.save_lpr_inputs:
        lpr_input_dir.mkdir(parents=True, exist_ok=True)

    plate_results: List[PlateResult] = []
    candidates_by_plate: Dict[int, List[Dict]] = {}
    lpr_total_ms = 0.0
    crop_pad_left, crop_pad_right, crop_pad_top, crop_pad_bottom = resolve_crop_padding(args)
    for det_idx, det in enumerate(detections):
        if det.class_id != 0:
            continue
        crop = crop_with_padding(frame, det.box, crop_pad_left, crop_pad_right, crop_pad_top, crop_pad_bottom)
        if crop is None:
            continue
        estimated_type = estimate_plate_type(crop)
        crop = refine_plate_crop_by_color(crop, estimated_type, args)
        crop = refine_plate_crop_aspect(crop, estimated_type, args)
        estimated_type = estimate_plate_type(crop)
        crop_h, crop_w = crop.shape[:2]
        lpr_input = preprocess_lpr(crop, args.lpr_color)

        t0 = time.perf_counter()
        logits = lpr.inference(inputs=[lpr_input])[0]
        raw_text, _ = greedy_debug_decode(logits)
        locked_province_idx = None
        if args.preserve_raw_province:
            raw_province_idx = province_idx_from_text(raw_text)
            if raw_province_idx is not None:
                province_scores = province_scores_from_logits(logits, args.province_timesteps)
                raw_province_conf = float(math.exp(float(province_scores[raw_province_idx])))
                if raw_province_conf >= args.raw_province_lock_min_conf:
                    locked_province_idx = raw_province_idx
        text, lpr_score, beam_score, decoded_type, subtype, candidates = decode_with_type_fallback(
            logits, estimated_type, args.beam_width, args.beam_topk,
            args.province_aware_decode, args.province_topk,
            args.province_timesteps, args.province_score_weight,
            locked_province_idx
        )
        text, lpr_score, beam_score, decoded_type, subtype, candidates, verifier_info = verify_or_correct_province(
            args, province, crop, logits, text, lpr_score, beam_score,
            decoded_type, subtype, estimated_type, candidates
        )
        lpr_total_ms += (time.perf_counter() - t0) * 1000.0

        selected_candidate = next((c for c in candidates if c.get("selected")), None)
        valid = bool(text) and bool(selected_candidate and selected_candidate.get("length_ok"))
        invalid_reason = "" if valid else "invalid_length_or_empty"
        if valid and lpr_score < args.min_lpr_score:
            valid = False
            invalid_reason = "low_lpr_score"

        # --- Province quality checks (all optional, controlled by CLI) ---
        province_conf = float(selected_candidate.get("province_conf") or 0.0) if selected_candidate else 0.0
        raw_has_province = first_char_is_province(raw_text)
        raw_province = raw_text[0] if raw_has_province else ""
        final_province = text[0] if text else ""

        # 1. Hallucinated province: raw had no province char, decode forced one in.
        if valid and args.reject_hallucinated_province and not raw_has_province:
            if args.reject_any_hallucinated_province or province_conf < args.hallucinated_province_min_conf:
                valid = False
                invalid_reason = "hallucinated_province"

        # 2. Province mismatch: raw had province X, constrained decode picked different province Y.
        #    This means beam search overrode the model's greedy province — a sign of uncertainty.
        if valid and args.reject_province_mismatch and raw_has_province and final_province and raw_province != final_province:
            if args.reject_any_province_mismatch or province_conf < args.province_mismatch_min_conf:
                valid = False
                invalid_reason = f"province_mismatch(raw={raw_province},final={final_province})"

        # 3. Low province confidence: model is uncertain about the province identity.
        if valid and args.min_province_conf > 0 and province_conf < args.min_province_conf:
            valid = False
            invalid_reason = "low_province_conf"

        # 4. Province instability: different plate-type decodes disagree on the province.
        if valid and args.reject_province_unstable:
            prov_votes = _province_votes_from_candidates(candidates)
            if prov_votes is not None and prov_votes < args.province_stability_min_agreement:
                valid = False
                invalid_reason = f"province_unstable(agreement={prov_votes})"

        if not valid:
            lpr_score = 0.0

        decoded_type_label = decoded_type if not subtype else f"{decoded_type}:{subtype}"
        if estimated_type in {"blue", "green", "yellow", "black"}:
            plate_type = estimated_type if not subtype else f"{estimated_type}:{subtype}"
        else:
            plate_type = decoded_type_label
        crop_path = None
        if args.save_crops:
            crop_path = str(crop_dir / f"{image_path.stem}_plate{len(plate_results)}.jpg")
            cv2.imwrite(crop_path, crop)
        if args.save_lpr_inputs:
            lpr_input_path = lpr_input_dir / f"{image_path.stem}_plate{len(plate_results)}_94x24.jpg"
            cv2.imwrite(str(lpr_input_path), make_lpr_debug_image(crop, args.lpr_color))

        match = None if gt_text is None or not valid else text == gt_text
        plate_results.append(PlateResult(
            box=[int(round(v)) for v in det.box],
            detection_index=det_idx,
            det_score=det.score,
            estimated_type=estimated_type,
            decoded_type=decoded_type,
            plate_subtype=subtype,
            plate_type=plate_type,
            plate_text=text,
            valid=valid,
            invalid_reason=invalid_reason,
            lpr_score=lpr_score,
            beam_score=beam_score,
            raw_text=raw_text,
            province_verify_status=verifier_info["province_verify_status"],
            verifier_province=verifier_info["verifier_province"],
            verifier_conf=verifier_info["verifier_conf"],
            verifier_top3=verifier_info["verifier_top3"],
            crop_path=crop_path,
            crop_width=crop_w,
            crop_height=crop_h,
            gt_text=gt_text,
            match=match,
        ))
        candidates_by_plate[len(plate_results) - 1] = candidates

    vis_path = None
    if args.save_vis:
        output_dir.mkdir(parents=True, exist_ok=True)
        vis = draw_detections(frame, detections, plate_results, args.font_path)
        vis_path = str(output_dir / f"{image_path.stem}_vis.jpg")
        cv2.imwrite(vis_path, vis)

    timings = {
        "yolo_pre_ms": yolo_pre_ms,
        "yolo_infer_ms": yolo_infer_ms,
        "yolo_post_ms": yolo_post_ms,
        "lpr_total_ms": lpr_total_ms,
        "total_ms": (time.perf_counter() - total_start) * 1000.0,
    }
    payload = {
        "image": str(image_path),
        "gt_text": gt_text,
        "timings": timings,
        "detections": [
            {"class": YOLO_CLASSES[d.class_id], "score": d.score, "box": [int(round(v)) for v in d.box]}
            for d in detections
        ],
        "plates": [asdict(plate) for plate in plate_results],
    }
    print(json.dumps(payload, ensure_ascii=False))
    if debug is not None:
        debug.write_jsonl(payload)
        write_debug_rows(debug, image_path, gt_text, frame.shape[:2], detections, plate_results,
                         candidates_by_plate, timings, vis_path)
    return plate_results

def main() -> None:
    parser = argparse.ArgumentParser(description="Run RKNN YOLO + LPRNet plate inference with constrained CTC decode.")
    parser.add_argument("--image", required=True, help="Input image path or image folder.")
    parser.add_argument("--yolo-model", default="fenqusai/rknn/best.rknn")
    parser.add_argument("--lpr-model", default="fenqusai/rknn/lprnet_unified_p15_focus_fp.rknn")
    parser.add_argument("--province-model", default="",
                        help="Optional 31-class province classifier RKNN. Empty disables province verification.")
    parser.add_argument("--output-dir", default="fenqusai/tools/pipeline/result_rknn_plate")
    parser.add_argument("--conf-thres", type=float, default=0.25)
    parser.add_argument("--nms-thres", type=float, default=0.45)
    parser.add_argument("--beam-width", type=int, default=10)
    parser.add_argument("--beam-topk", type=int, default=8)
    parser.add_argument("--province-aware-decode", action="store_true", default=True,
                        help="Fix a top-K province prefix first, then decode the suffix with constrained CTC.")
    parser.add_argument("--no-province-aware-decode", dest="province_aware_decode", action="store_false")
    parser.add_argument("--province-topk", type=int, default=DEFAULT_PROVINCE_TOPK,
                        help="Number of early-timestep province candidates to try.")
    parser.add_argument("--province-timesteps", type=int, default=DEFAULT_PROVINCE_TIMESTEPS,
                        help="How many early CTC timesteps to use for province scoring.")
    parser.add_argument("--province-score-weight", type=float, default=DEFAULT_PROVINCE_SCORE_WEIGHT,
                        help="Weight of province log-score when choosing among fixed-province decodes.")
    parser.add_argument("--province-verify", action="store_true", default=False,
                        help="Use --province-model as a conservative province verifier/reranker.")
    parser.add_argument("--no-province-verify", dest="province_verify", action="store_false")
    parser.add_argument("--province-input-width", type=int, default=48,
                        help="Province classifier input width.")
    parser.add_argument("--province-input-height", type=int, default=24,
                        help="Province classifier input height.")
    parser.add_argument("--province-left-ratio", type=float, default=0.45,
                        help="Left crop ratio from the 94x24 plate image for province classifier.")
    parser.add_argument("--province-color", choices=["bgr", "rgb"], default="bgr",
                        help="Color order fed to province classifier RKNN.")
    parser.add_argument("--province-correct-min-conf", type=float, default=0.35,
                        help="Minimum province classifier confidence before trying forced-prefix correction.")
    parser.add_argument("--province-agree-topk", type=int, default=2,
                        help="Treat LPR province as weakly verified if it is in classifier top-K.")
    parser.add_argument("--province-correction-min-lpr-ratio", type=float, default=0.75,
                        help="Accept forced-prefix correction only if its LPR score keeps at least this ratio of the original score.")
    parser.add_argument("--min-province-conf", type=float, default=0.0,
                        help="Mark selected candidates below this province confidence as invalid. 0 disables. Suggested: 0.25–0.35 for aggressive filtering, 0.15 for gentle.")
    parser.add_argument("--reject-province-mismatch", action="store_true", default=False,
                        help="Reject results where raw CTC province differs from constrained decode province.")
    parser.add_argument("--allow-province-mismatch", dest="reject_province_mismatch", action="store_false")
    parser.add_argument("--reject-any-province-mismatch", action="store_true", default=False,
                        help="Reject every province-mismatch result regardless of confidence.")
    parser.add_argument("--allow-confident-province-mismatch", dest="reject_any_province_mismatch", action="store_false",
                        help="Only reject province-mismatch results when province_conf is below --province-mismatch-min-conf.")
    parser.add_argument("--province-mismatch-min-conf", type=float, default=0.30,
                        help="Minimum province_conf to allow a province-mismatch result through.")
    parser.add_argument("--reject-province-unstable", action="store_true", default=False,
                        help="Reject results where plate-type decodes disagree on the province identity.")
    parser.add_argument("--allow-province-unstable", dest="reject_province_unstable", action="store_false")
    parser.add_argument("--province-stability-min-agreement", type=float, default=0.5,
                        help="Minimum fraction of plate-type decodes that must agree on the selected province.")
    parser.add_argument("--preserve-raw-province", action="store_true", default=False,
                        help="When raw CTC starts with a high-confidence province, force final constrained decode to keep that province.")
    parser.add_argument("--allow-raw-province-change", dest="preserve_raw_province", action="store_false")
    parser.add_argument("--raw-province-lock-min-conf", type=float, default=0.45,
                        help="Only preserve raw CTC province when its early-timestep confidence is at least this value.")
    parser.add_argument("--reject-hallucinated-province", action="store_true", default=True,
                        help="Mark low-confidence province-filled results invalid when raw CTC did not start with a province.")
    parser.add_argument("--allow-hallucinated-province", dest="reject_hallucinated_province", action="store_false")
    parser.add_argument("--reject-any-hallucinated-province", action="store_true", default=True,
                        help="Reject every province-filled result whose raw CTC text did not start with a province.")
    parser.add_argument("--allow-confident-hallucinated-province", dest="reject_any_hallucinated_province", action="store_false",
                        help="Use --hallucinated-province-min-conf instead of rejecting all province-filled results.")
    parser.add_argument("--hallucinated-province-min-conf", type=float, default=0.02,
                        help="Minimum province confidence for accepting a province inserted by constrained decode.")
    parser.add_argument("--min-lpr-score", type=float, default=0.10,
                        help="Mark decoded plates below this LPR confidence as invalid.")
    parser.add_argument("--crop-pad-x", type=float, default=0.08)
    parser.add_argument("--crop-pad-y", type=float, default=0.15)
    parser.add_argument("--crop-pad-left", type=float, default=0.25,
                        help="Extra left padding ratio for plate crop. Province chars are on the left, so this is larger by default.")
    parser.add_argument("--crop-pad-right", type=float, default=0.10,
                        help="Extra right padding ratio for plate crop.")
    parser.add_argument("--crop-pad-top", type=float, default=0.18,
                        help="Extra top padding ratio for plate crop.")
    parser.add_argument("--crop-pad-bottom", type=float, default=0.15,
                        help="Extra bottom padding ratio for plate crop.")
    parser.add_argument("--refine-plate-crop", action="store_true", default=False,
                        help="Refine YOLO crop to the coloured plate region before LPRNet.")
    parser.add_argument("--no-refine-plate-crop", dest="refine_plate_crop", action="store_false")
    parser.add_argument("--refine-pad-left", type=float, default=0.04,
                        help="Left padding ratio after colour-based plate crop refinement.")
    parser.add_argument("--refine-pad-right", type=float, default=0.03,
                        help="Right padding ratio after colour-based plate crop refinement.")
    parser.add_argument("--refine-pad-top", type=float, default=0.08,
                        help="Top padding ratio after colour-based plate crop refinement.")
    parser.add_argument("--refine-pad-bottom", type=float, default=0.08,
                        help="Bottom padding ratio after colour-based plate crop refinement.")
    parser.add_argument("--refine-min-area-ratio", type=float, default=0.18,
                        help="Minimum candidate colour-region area ratio in the loose crop.")
    parser.add_argument("--refine-max-area-ratio", type=float, default=0.95,
                        help="Reject refinement if it keeps almost the whole loose crop.")
    parser.add_argument("--refine-min-aspect", type=float, default=1.8,
                        help="Minimum aspect ratio for a colour-region plate candidate.")
    parser.add_argument("--refine-max-aspect", type=float, default=8.5,
                        help="Maximum aspect ratio for a colour-region plate candidate.")
    parser.add_argument("--refine-plate-aspect", action="store_true", default=False,
                        help="If a coloured plate crop is too tall, trim vertical background while preserving full width.")
    parser.add_argument("--no-refine-plate-aspect", dest="refine_plate_aspect", action="store_false")
    parser.add_argument("--refine-aspect-min", type=float, default=3.2,
                        help="Only apply vertical aspect refinement when crop aspect is below this value.")
    parser.add_argument("--refine-aspect-target", type=float, default=4.2,
                        help="Target plate aspect ratio for vertical refinement.")
    parser.add_argument("--yolo-color", choices=["rgb", "bgr"], default="rgb",
                        help="Color order fed to YOLO RKNN after letterbox. Confirm against convert.py/export.")
    parser.add_argument("--lpr-color", choices=["bgr", "rgb"], default="bgr",
                        help="Color order fed to LPRNet RKNN. Current convert.py expects uint8 BGR crop when mean/std is in RKNN.")
    parser.add_argument("--core-mask", choices=["default", "core0", "core1", "core2", "core01", "core012", "auto"],
                        default="default")
    parser.add_argument("--lpr-core-mask", choices=["default", "core0", "core1", "core2", "core01", "core012", "auto"],
                        default="default")
    parser.add_argument("--province-core-mask", choices=["default", "core0", "core1", "core2", "core01", "core012", "auto"],
                        default="default")
    parser.add_argument("--save-vis", action="store_true", default=True)
    parser.add_argument("--no-save-vis", dest="save_vis", action="store_false")
    parser.add_argument("--save-crops", action="store_true", default=True)
    parser.add_argument("--no-save-crops", dest="save_crops", action="store_false")
    parser.add_argument("--save-lpr-inputs", action="store_true", default=False,
                        help="Save the exact 94x24 image fed to LPRNet for crop/resize debugging.")
    parser.add_argument("--font-path", default=None,
                        help="Optional Chinese font path for visualization, e.g. /usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc.")
    parser.add_argument("--export-debug", action="store_true", default=True,
                        help="Write debug CSV/JSONL files under <output-dir>/debug.")
    parser.add_argument("--no-export-debug", dest="export_debug", action="store_false")
    args = parser.parse_args()

    if cv2 is None:
        raise RuntimeError(f"opencv-python import failed: {CV2_IMPORT_ERROR}")
    if np is None:
        raise RuntimeError(f"numpy import failed: {NP_IMPORT_ERROR}")

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    if args.province_model:
        args.province_verify = True

    debug = DebugWriter(output_dir, args) if args.export_debug else None
    yolo = load_rknn(args.yolo_model, parse_core_mask(args.core_mask))
    lpr = load_rknn(args.lpr_model, parse_core_mask(args.lpr_core_mask))
    province = None
    if args.province_model:
        province = load_rknn(args.province_model, parse_core_mask(args.province_core_mask))
        args.province_verify = True
    try:
        for img_path in image_files(Path(args.image)):
            run_image(args, yolo, lpr, province, img_path, output_dir, debug)
    finally:
        yolo.release()
        lpr.release()
        if province is not None:
            province.release()
        if debug is not None:
            debug.close()


if __name__ == "__main__":
    main()
