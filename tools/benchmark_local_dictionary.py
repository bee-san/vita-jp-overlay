#!/usr/bin/env python3
"""Reproducible host-only disk dictionary benchmark; does not measure Vita latency."""
import argparse
import json
from pathlib import Path
import re
import statistics
import subprocess
import tempfile
import time
import zipfile

from convert_dictionary import convert


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", type=Path, default=Path("build/host/vjo-cli"))
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
        times, peaks = [], []
        for _ in range(args.runs):
            start = time.perf_counter()
            proc = subprocess.run([str(args.cli.resolve()), "--dict", "local", "--local-dir", tmp,
                                   "--text", "猫は食べました。行った。", "--stats"], check=True,
                                  capture_output=True, text=True)
            times.append((time.perf_counter() - start) * 1000)
            peaks.append(int(re.search(r"arena peak: (\d+)", proc.stderr).group(1)))
            if not all(word in proc.stdout for word in ("cat", "eat", "go")):
                raise RuntimeError("benchmark lookup returned incomplete results")
        print(json.dumps({**stats, "conversion_seconds": round(conversion_s, 3),
                          "cli_wall_median_ms": round(statistics.median(times), 3),
                          "lookup_arena_peak_bytes": max(peaks), "runs": args.runs,
                          "environment": "host CLI; wall time includes process startup; not Vita hardware"}, indent=2))


if __name__ == "__main__":
    main()
