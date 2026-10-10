#!/usr/bin/env python3
"""Run a bounded live Lens comparison; save only hashes, counts and timings.

Inputs remain private. Each case sends exactly the same pre-encoded JPEG over
fresh and pooled connections, with alternating pair order. This measures host
WAN/network behavior, not Vita speed, screenshot CER, or image encoding time.
"""
import argparse
import hashlib
import json
import platform
import statistics
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def summary(samples):
    times = [sample["elapsed_ms"] for sample in samples]
    return {
        "samples": len(samples),
        "median_ms": round(statistics.median(times), 3),
        "minimum_ms": min(times),
        "maximum_ms": max(times),
        "new_connections": sum(sample["new_connections"] for sample in samples),
        "kept": sum(sample["kept"] for sample in samples),
        "arena_peak_max_bytes": max(sample["arena_peak_bytes"] for sample in samples),
        "pool_reserved_max_bytes": max(sample["pool_reserved_bytes"] for sample in samples),
        "protobuf_min_bytes": min(sample["protobuf_bytes"] for sample in samples),
        "protobuf_max_bytes": max(sample["protobuf_bytes"] for sample in samples),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, required=True)
    parser.add_argument("--case", action="append", nargs=4, metavar=("ID", "JPEG", "WIDTH", "HEIGHT"), required=True)
    parser.add_argument("--runs", type=int, default=6)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if not 2 <= args.runs <= 20:
        parser.error("--runs must be between 2 and 20")
    root = Path(__file__).resolve().parents[1]
    cases = []
    for identity, image, width, height in args.case:
        completed = subprocess.run(
            [str(args.cli.resolve()), image, width, height, str(args.runs)],
            text=True, capture_output=True, check=False,
        )
        samples = [json.loads(line) for line in completed.stdout.splitlines() if line.strip()]
        if completed.returncode or len(samples) != args.runs * 2:
            raise RuntimeError(f"case {identity}: failed after {len(samples)} samples, exit={completed.returncode}")
        if len({s["jpeg_sha256"] for s in samples}) != 1 or len({s["text_sha256"] for s in samples}) != 1:
            raise RuntimeError(f"case {identity}: image/output hashes changed")
        fresh = [s for s in samples if s["mode"] == "fresh"]
        repeat = [s for s in samples if s["mode"] == "pooled" and s["sample"] > 1]
        if any(s["rc"] or s["http_status"] != 200 or not s["same_text"] for s in samples):
            raise RuntimeError(f"case {identity}: failed response/parity")
        if any(s["new_connections"] or not s["kept"] for s in repeat):
            raise RuntimeError(f"case {identity}: repeat request did not reuse its connection")
        cases.append({
            "id": identity,
            "width": int(width), "height": int(height),
            "jpeg_sha256": samples[0]["jpeg_sha256"],
            "jpeg_bytes": samples[0]["jpeg_bytes"],
            "text_sha256": samples[0]["text_sha256"],
            "text_bytes": samples[0]["text_bytes"],
            "fresh": summary(fresh), "pooled_repeat": summary(repeat),
            "samples": samples,
        })
    sources = ("core/lens.c", "core/client.c", "core/http.c", "core/net.c", "core/tls.c", "host/net_posix.c", "host/benchmark_lens.c")
    result = {
        "measured_at_utc": datetime.now(timezone.utc).isoformat(),
        "environment": {"platform": platform.platform(), "machine": platform.machine()},
        "revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
        "source_sha256": {name: hashlib.sha256((root/name).read_bytes()).hexdigest() for name in sources},
        "endpoint": "https://lensfrontend-pa.googleapis.com/v1/crupload",
        "runs_per_mode": args.runs,
        "pair_order": "fresh/pooled then pooled/fresh, alternating",
        "limits": ["Host WAN measurements; no physical Vita speed or memory claim.",
                   "First pooled sample includes connection/TLS setup and is excluded from repeat summary.",
                   "JPEG already encoded once; encoding and capture time excluded.",
                   "Raw TLS byte counts include framing/handshake overhead; pool and arena are separate reservations.",
                   "Equal output hashes establish repeat parity, not real-screenshot CER."],
        "cases": cases,
    }
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2)+"\n")
    for case in cases:
        print(f"{case['id']}: fresh median={case['fresh']['median_ms']}ms, "
              f"pooled repeat median={case['pooled_repeat']['median_ms']}ms; "
              f"{case['pooled_repeat']['kept']} reused; {len(case['samples'])} equal output hashes")


if __name__ == "__main__":
    main()
