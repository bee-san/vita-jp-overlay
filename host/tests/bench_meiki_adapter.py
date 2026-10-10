#!/usr/bin/env python3
"""Measure the real overlay Meiki adapter on private hand-checked VN fixtures.

Runs annotated line crops and complete player-selected dialogue regions.
Segmentation refusals/blank outputs count as missing text in CER; this measures
the whole supported adapter, rather than recognition on supplied line boxes.
Copyrighted fixtures and predictions remain in --out, outside Git.
"""

import argparse
import hashlib
import json
import platform
import subprocess
import sys
from collections import Counter
from pathlib import Path

import numpy as np

DETECTOR_FILENAME = "meiki-detect-int8.mnn"
DETECTOR_SHA256 = "f0865ed291846a130905527f67c33f17e64d0f2c5eb38378ed1faea57f74b1ba"


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def detection_summary(counts):
    if not counts:
        return {}
    result = dict(counts)
    result["line_recall"] = counts["matched_known_lines"] / max(1, counts["known_lines"])
    result["line_precision"] = counts["matched_known_lines"] / max(1, counts["published_detections"])
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ocr-repo", type=Path, required=True)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--model-dir", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=2)
    layout = parser.add_mutually_exclusive_group()
    layout.add_argument("--single-line", action="store_true",
                        help="recognize each entire selection as one line")
    layout.add_argument("--dialogue-box", action="store_true",
                        help="use the existing Meiki320x192 detector and recognize at most8 crops")
    parser.add_argument("--modes", nargs="+", choices=("crops", "regions"),
                        default=("crops", "regions"))
    parser.add_argument("--crop-pad-factor", type=int, default=1,
                        help="multiply annotated crop padding (Vita4/PSP2) to measure margin sensitivity")
    args = parser.parse_args()
    if args.repetitions < 1:
        raise ValueError("repetitions must be positive")
    if args.crop_pad_factor < 1:
        raise ValueError("crop pad factor must be positive")
    args.ocr_repo = args.ocr_repo.resolve()
    args.cli = args.cli.resolve()
    args.model_dir = args.model_dir.resolve()
    args.out = args.out.resolve()
    sys.path.insert(0, str(args.ocr_repo / "eval"))
    from finders import match
    from metrics import score_lines
    from sets import load_real

    data_dir = args.data_dir or args.ocr_repo / "data/eval"
    args.out.mkdir(parents=True, exist_ok=False)
    input_path = args.out / "input.rgba"
    sets, exclusions = {}, {}
    for name in ("vita-test", "psp-test"):
        pad = (4 if name == "vita-test" else 2) * args.crop_pad_factor
        all_lines, cache = load_real(name, data_dir, pad=pad, include_uncertain=True)
        crops = [line for line in all_lines if not line.uncertain]
        grouped = {}
        for line in all_lines:
            if line.region_id:
                grouped.setdefault(line.region_id, []).append(line)
        regions = []
        uncertain = 0
        for region_id, lines in grouped.items():
            has_uncertain = any(line.uncertain for line in lines)
            if has_uncertain:
                uncertain += 1
                if not args.dialogue_box:
                    continue
            ordered = sorted(lines, key=lambda line: (line.box[1], line.box[0]))
            first = ordered[0]
            x, y, w, h = first.region
            rgb = cache.get(first.image)
            regions.append(
                {
                    "id": region_id,
                    "rgb": np.ascontiguousarray(rgb[y:y + h, x:x + w]),
                    "text": "\n".join(line.text for line in ordered),
                    "styles": sorted({tag for line in ordered for tag in line.styles}),
                    "vn": first.vn,
                    "annotated_lines": len(ordered),
                    "uncertain": has_uncertain,
                    "gt": [{"box_r": (line.box[0] - x, line.box[1] - y,
                                       line.box[2], line.box[3]),
                            "uncertain": line.uncertain} for line in ordered],
                }
            )
        sets[name] = {
            "crops": [
                {"id": line.id, "rgb": line.crop_rgb, "text": line.text,
                 "styles": line.styles, "vn": line.vn, "annotated_lines": 1}
                for line in crops
            ],
            "regions": regions,
        }
        exclusions[name] = {"uncertain_regions": uncertain}

    dependencies = json.loads((args.ocr_repo / "ports/meiki/dependencies.json").read_text())
    model = args.model_dir / dependencies["converted"]["filename"]
    if sha256(model) != dependencies["converted"]["sha256"]:
        raise ValueError("model does not match the pinned Meiki conversion")
    detector = args.model_dir / DETECTOR_FILENAME
    if args.dialogue_box and sha256(detector) != DETECTOR_SHA256:
        raise ValueError("detector does not match the pinned Meiki320x192 conversion")
    summary = {
        "scope": "host actual overlay C adapter, C preprocessing, bounded MNN callback and C decoder; not console RAM or latency",
        "input_mode": "dialogue-box" if args.dialogue_box else
                      "single-line" if args.single_line else "projected-lines",
        "crop_pad_factor": args.crop_pad_factor,
        "detected_line_limit": 8 if args.dialogue_box else None,
        "environment": {"platform": platform.platform(), "machine": platform.machine()},
        "repetitions": args.repetitions,
        "cli_sha256": sha256(args.cli),
        "model_sha256": sha256(model),
        "detector_sha256": sha256(detector) if args.dialogue_box else None,
        "uncertain_policy": "whole-region CER excludes any region with uncertain annotations; those regions are exercised and reported separately",
        "manifest_sha256": {
            name: sha256(args.ocr_repo / f"eval/manifests/{name}.jsonl") for name in sets
        },
        "exclusions": exclusions,
        "sets": {},
    }
    with (args.out / "engine.log").open("w") as log, (args.out / "cases.jsonl").open("w") as records:
        command = [str(args.cli)]
        if args.single_line:
            command.append("--single-line")
        elif args.dialogue_box:
            command.append("--dialogue-box")
        command.append(str(args.model_dir))
        process = subprocess.Popen(
            command, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=log, text=True,
        )
        try:
            for name, modes in sets.items():
                summary["sets"][name] = {}
                for mode, fixtures in modes.items():
                    if mode not in args.modes:
                        continue
                    scored, statuses = [], Counter()
                    unknown_statuses = Counter()
                    detection = Counter()
                    unknown_detection = Counter()
                    repeat_differences = peak = remaining = calls = failed_allocations = 0
                    for index, fixture in enumerate(fixtures):
                        rgb = fixture["rgb"]
                        h, w = rgb.shape[:2]
                        rgba = np.empty((h, w, 4), dtype=np.uint8)
                        rgba[:, :, :3], rgba[:, :, 3] = rgb, 255
                        rgba.tofile(input_path)
                        predictions = []
                        for repetition in range(args.repetitions):
                            process.stdin.write(f"{w}\t{h}\t{input_path}\n")
                            process.stdin.flush()
                            while True:
                                line = process.stdout.readline()
                                if not line:
                                    raise RuntimeError(f"engine terminated ({process.poll()}); see engine.log")
                                if line.startswith('{"rc":'):
                                    result = json.loads(line)
                                    break
                                log.write(line)
                            pred = bytes.fromhex(result["text_hex"]).decode("utf-8")
                            predictions.append(pred)
                            calls += 1
                            peak = max(peak, result["heap_peak"])
                            remaining = max(remaining, result["heap_remaining"])
                            failed_allocations += bool(result["allocation_failed"])
                            records.write(json.dumps(
                                {"set": name, "mode": mode, "id": fixture["id"],
                                 "repetition": repetition, "annotated_lines": fixture["annotated_lines"],
                                 "text": fixture["text"], "prediction": pred, "runtime": result},
                                ensure_ascii=False,
                            ) + "\n")
                            if repetition == 0:
                                statuses[str(result["rc"])] += 1
                                if fixture.get("uncertain"):
                                    unknown_statuses[str(result["rc"])] += 1
                                if mode == "regions" and "boxes" in result:
                                    gt = fixture["gt"]
                                    boxes = result["boxes"] if not result["rc"] else []
                                    pairs, unmatched = match(boxes, gt, .5)
                                    unknown_pairs = sum(gt[j]["uncertain"] for _, j in pairs)
                                    target = unknown_detection if fixture["uncertain"] else detection
                                    target["known_lines"] += sum(not row["uncertain"] for row in gt)
                                    target["uncertain_lines"] += sum(row["uncertain"] for row in gt)
                                    target["matched_known_lines"] += len(pairs) - unknown_pairs
                                    target["published_detections"] += len(boxes) - unknown_pairs
                                    target["unmatched_detections"] += len(unmatched)
                        repeat_differences += any(pred != predictions[0] for pred in predictions[1:])
                        if not fixture.get("uncertain"):
                            scored.append({"pred": predictions[0], "text": fixture["text"],
                                           "styles": fixture["styles"], "vn": fixture["vn"]})
                        if (index + 1) % 25 == 0:
                            print(f"{name} {mode}: {index + 1}/{len(fixtures)}", flush=True)
                    summary["sets"][name][mode] = {
                        "fixtures": len(fixtures), "inference_requests": calls,
                        "scored_fixtures": len(scored),
                        "statuses": dict(sorted(statuses.items())),
                        "repeat_text_differences": repeat_differences,
                        "peak_model_tensor_bytes": peak, "max_retained_pool_bytes": remaining,
                        "allocation_failures": failed_allocations,
                        "score": score_lines(scored),
                        "detection": detection_summary(detection),
                        "unknown_regions": {"fixtures": sum(unknown_statuses.values()),
                                            "statuses": dict(unknown_statuses),
                                            "detection": detection_summary(unknown_detection)},
                    }
                    (args.out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        finally:
            process.stdin.close()
            if process.poll() is None:
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.terminate()
                    process.wait(timeout=20)
            process.stdout.close()
        if process.returncode:
            raise RuntimeError(f"engine exited {process.returncode}")
    input_path.unlink()
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
