#!/usr/bin/env python3
"""Reject compiler-generated VFP/NEON code in an ARM kernel plugin ELF."""
import argparse
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--objdump", required=True)
    parser.add_argument("elf")
    args = parser.parse_args()
    output = subprocess.check_output([args.objdump, "-d", args.elf], text=True)
    instruction = re.compile(r"^\s*[0-9a-f]+:\s+(?:[0-9a-f]{4,8}\s+)+([a-z][a-z0-9.]*)\s", re.I)
    count = 0
    invalid = []
    for line in output.splitlines():
        match = instruction.match(line)
        if not match:
            continue
        count += 1
        if match.group(1).lower().startswith("v"):
            invalid.append(line.strip())
    if not count:
        parser.exit(1, "No ARM instructions decoded; kernel verification failed.\n")
    if invalid:
        parser.exit(1, f"Kernel contains {len(invalid)} FPU/NEON instructions:\n" + "\n".join(invalid[:12]) + "\n")
    print(f"Kernel verified: {count} ARM instructions, no FPU/NEON instructions")


if __name__ == "__main__":
    main()
