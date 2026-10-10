#!/usr/bin/env python3
"""Patch only this overlay's taiHEN entries; preserve other plugins and CRLF."""
import argparse
from pathlib import Path

KERNEL = "ur0:tai/VitaJPOverlay_Kernel.skprx"
SHELL = "ur0:tai/VitaJPOverlay_Shell.suprx"
TEXT = "ur0:tai/VitaJPOverlay_Text.suprx"
OCR = "ur0:tai/VitaJPOverlay_OCR.suprx"


def patch(raw: bytes, extra_kernel=(), uninstall=False) -> bytes:
    source = raw.decode("utf-8")
    newline = "\r\n" if "\r\n" in source else "\n"
    lines = source.splitlines()
    if uninstall:
        lines = [line for line in lines if line.strip() not in (KERNEL, SHELL, TEXT, OCR)]
    else:
        def add(section, plugin):
            begin = next((i for i, line in enumerate(lines) if line.strip() == section), None)
            if begin is None:
                lines.extend(["", section, plugin])
                return
            end = next((i for i in range(begin + 1, len(lines)) if lines[i].strip().startswith("*")), len(lines))
            if not any(line.strip() == plugin for line in lines[begin + 1:end]):
                lines.insert(begin + 1, plugin)
        add("*KERNEL", KERNEL)
        for plugin in extra_kernel:
            add("*KERNEL", plugin)
        add("*main", SHELL)
        # Keep this branch's per-game OCR registrations active. Native text
        # extraction belongs to its own branch; preserve its entries as comments.
        lines = [("# Meiki OCR only: " + line) if line.strip() == TEXT else line
                 for line in lines]
    return (newline.join(lines) + newline).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--kernel-plugin", action="append", default=[])
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    try:
        args.output.write_bytes(patch(args.source.read_bytes(), args.kernel_plugin, args.uninstall))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
