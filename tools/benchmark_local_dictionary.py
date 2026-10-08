#!/usr/bin/env python3
"""Reproducible host-only disk dictionary benchmark; does not measure Vita latency."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import tempfile
import time
import zipfile

from convert_dictionary import convert


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=Path("build/host/vjo-cli"))
    parser.add_argument("--bench", type=Path, help="vjo-local-bench (defaults to the CLI directory)")
    parser.add_argument("--entries", type=int, default=100000)
    parser.add_argument("--runs", type=int, default=10)
    args = parser.parse_args()
    if args.entries < 1 or args.runs < 1:
        parser.error("entries and runs must be positive")
    with tempfile.TemporaryDirectory(prefix="vjo-benchmark-") as tmp:
        folder = Path(tmp)
        archive = folder / "synthetic.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
            z.writestr("index.json", '{"format":3,"title":"Synthetic benchmark"}')
            for start in range(0, args.entries, 10000):
                with z.open(f"term_bank_{start // 10000 + 1}.json", "w") as bank:
                    bank.write(b"[")
                    for i in range(start, min(args.entries, start + 10000)):
                        row = [f"項目{i:09d}", "", "", "", 0,
                               ["Synthetic term used only to measure indexed disk lookup. " * 2], i, ""]
                        if i > start:
                            bank.write(b",")
                        bank.write(json.dumps(row, ensure_ascii=False).encode())
                    bank.write(b"]")
            z.writestr("term_bank_999999.json", json.dumps([
                ["猫", "ねこ", "", "", 0, ["cat"], 1, ""],
                ["食べる", "たべる", "", "v1", 0, ["eat"], 2, ""],
                ["行く", "いく", "", "v5", 0, ["go"], 3, ""],
            ], ensure_ascii=False))
        start = time.perf_counter()
        stats = convert([archive], folder / "main.vjdict")
        conversion_s = time.perf_counter() - start
        bench = args.bench or args.cli.with_name("vjo-local-bench")
        proc = subprocess.run([str(bench.resolve()), tmp, "main.vjdict",
                               "猫は食べました。行った。", str(args.runs + 1)],
                              check=True, capture_output=True, text=True)
        samples = [json.loads(line) for line in proc.stdout.splitlines()]
        if any(row["entries"] != 3 for row in samples):
            raise RuntimeError("benchmark lookup returned incomplete results")
        print(json.dumps({**stats, "conversion_seconds": round(conversion_s, 3),
                          "lookup_median_us": round(statistics.median(r["lookup_us"] for r in samples[1:]), 3),
                          "read_calls": samples[-1]["read_calls"], "read_bytes": samples[-1]["read_bytes"],
                          "lookup_arena_peak_bytes": max(r["arena_peak"] for r in samples), "runs": args.runs,
                          "environment": "host lookup only; empty application caches; warm OS cache; not Vita hardware"}, indent=2))


if __name__ == "__main__":
    main()
