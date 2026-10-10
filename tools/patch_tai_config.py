#!/usr/bin/env python3
"""Patch only this overlay's taiHEN entries; preserve other plugins and CRLF."""
import argparse
from pathlib import Path
import re

KERNEL = "ur0:tai/VitaJPOverlay_Kernel.skprx"
SHELL = "ur0:tai/VitaJPOverlay_Shell.suprx"
TEXT = "ur0:tai/VitaJPOverlay_Text.suprx"


def patch(raw: bytes, titles=(), extra_kernel=(), uninstall=False) -> bytes:
    for title in titles:
        if not re.fullmatch(r"[A-Z0-9]{9}", title):
            raise ValueError(f"invalid Vita title ID: {title}")
    source = raw.decode("utf-8")
    newline = "\r\n" if "\r\n" in source else "\n"
    lines = source.splitlines()
    if uninstall:
        lines = [line for line in lines if line.strip() not in (KERNEL, SHELL, TEXT)]
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
        # A pre-existing global entry already covers every title.
        global_begin = next((i for i, line in enumerate(lines) if line.strip() == "*ALL"), None)
        global_text = False
        if global_begin is not None:
            end = next((i for i in range(global_begin + 1, len(lines)) if lines[i].strip().startswith("*")), len(lines))
            global_text = any(line.strip() == TEXT for line in lines[global_begin + 1:end])
        if not global_text:
            for title in titles:
                add("*" + title, TEXT)
    return (newline.join(lines) + newline).encode("utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--text-title", action="append", default=[])
    parser.add_argument("--kernel-plugin", action="append", default=[])
    parser.add_argument("--uninstall", action="store_true")
    args = parser.parse_args()
    try:
        args.output.write_bytes(patch(args.source.read_bytes(), args.text_title, args.kernel_plugin, args.uninstall))
    except (OSError, ValueError) as error:
        parser.error(str(error))


if __name__ == "__main__":
    main()
