#!/usr/bin/env python3
"""Compare real lookup time, file I/O and exact result digests; no Vita claims.

Application caches reset each lookup. The host OS file cache is not evicted.
Per-read latency models are arithmetic, not physical-device measurements.
"""
import argparse
import json
from pathlib import Path
import statistics
import subprocess


def measure(binary, directory, dictionaries, text, runs):
    result = subprocess.run([str(binary.resolve()), str(directory), dictionaries, text, str(runs + 1)],
                            capture_output=True, text=True, check=True, timeout=120)
    rows = [json.loads(line) for line in result.stdout.splitlines()]
    # Discard the first timing (host OS cache warmup), but application caches
    # still start empty on every measured request, as in the production worker.
    timed = rows[1:]
    if len({r["digest"] for r in rows}) != 1:
        raise RuntimeError("nondeterministic lookup results")
    return {"median_us": round(statistics.median(r["lookup_us"] for r in timed), 3),
            "p95_us": sorted(r["lookup_us"] for r in timed)[min(len(timed) - 1, int(.95 * len(timed)))],
            **{k: rows[-1][k] for k in ("read_calls", "read_bytes", "nonsequential_reads", "arena_peak", "entries", "digest")}}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", type=Path, required=True)
    parser.add_argument("--after", type=Path, required=True)
    parser.add_argument("--directory", type=Path, required=True)
    parser.add_argument("--before-dicts", default="jitendex-v1.vjdict")
    parser.add_argument("--after-dicts", default="jitendex-v2.vjdict")
    parser.add_argument("--cases", type=Path, default=Path(__file__).with_name("lookup_benchmark_cases.json"))
    parser.add_argument("--runs", type=int, default=30)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("runs must be positive")
    rows = []
    for case in json.loads(args.cases.read_text()):
        before = measure(args.before, args.directory, args.before_dicts, case["text"], args.runs)
        after = measure(args.after, args.directory, args.after_dicts, case["text"], args.runs)
        if before["digest"] != after["digest"]:
            raise RuntimeError(f"lookup behavior changed for {case['name']}: {before} vs {after}")
        rows.append({**case, "before": before, "after": after,
                     "read_reduction_percent": round(100 * (1 - after["read_calls"] / before["read_calls"]), 2),
                     "host_speedup": round(before["median_us"] / after["median_us"], 2)})
    print(json.dumps({"runs_per_case": args.runs,
                      "method": "timed lookup only; empty application caches; warm host OS cache; not Vita hardware",
                      "before_dicts": args.before_dicts, "after_dicts": args.after_dicts,
                      "cases": rows}, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
