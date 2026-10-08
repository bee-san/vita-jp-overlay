#!/usr/bin/env python3
"""Upload .vjdict files through VitaShell FTP and optionally enable local lookup.

Uses versioned filenames: a failed transfer cannot replace an active dictionary.
Run while games are closed. Existing config is backed up before --enable.
"""
import argparse
import ftplib
import hashlib
import io
from pathlib import Path
import re
import secrets
import sys
import time

from convert_dictionary import HEADER, MAGIC, RECORD

DATA_DIR = "/ux0:/data/VitaJPOverlay"
DICT_DIR = DATA_DIR + "/dictionaries"


def inspect(path):
    with path.open("rb") as f:
        raw = f.read(HEADER.size)
        if len(raw) != HEADER.size:
            raise ValueError(f"{path}: incomplete dictionary header")
        magic, version, header, count, stride, index, data, size, r1, r2 = HEADER.unpack(raw)
        if (magic != MAGIC or version != 1 or header != HEADER.size or stride != RECORD.size
                or not count or index != HEADER.size or data != header + count * stride
                or data > size or size != path.stat().st_size or r1 or r2):
            raise ValueError(f"{path}: incompatible or incomplete .vjdict file")
        sha = hashlib.sha256(raw)
        for block in iter(lambda: f.read(1024 * 1024), b""):
            sha.update(block)
    stem = re.sub(r"[^A-Za-z0-9_-]", "_", path.stem)[:28] or "dictionary"
    return f"{stem}-{sha.hexdigest()[:12]}.vjdict"


def config_with_local(text, names):
    if not names or len(names) > 8:
        raise ValueError("select between 1 and 8 dictionaries")
    values = {"dictionary": "local", "local_dictionary_dir": "ux0:data/VitaJPOverlay/dictionaries",
              "local_dictionaries": ",".join(names)}
    lines, used = [], set()
    for line in text.splitlines():
        match = re.match(r"\s*([a-zA-Z_]+)\s*=", line)
        key = match.group(1).lower() if match else None
        if key in values:
            # Collapse duplicates so the last setting cannot undo the change.
            if key in used:
                continue
            line = f"{key} = {values[key]}"
            used.add(key)
        lines.append(line)
    lines.extend(f"{key} = {value}" for key, value in values.items() if key not in used)
    return "\n".join(lines) + "\n"


def mkdirs(ftp):
    for directory in ("/ux0:/data", DATA_DIR, DICT_DIR):
        try:
            ftp.mkd(directory)
        except ftplib.error_perm:
            # Confirm it is an existing directory, not an ignored permission error.
            ftp.cwd(directory)


def transfer(ftp, local, remote):
    partial = remote + ".part"
    with local.open("rb") as stream:
        ftp.storbinary("STOR " + partial, stream, blocksize=65536)
    # A disconnect leaves only .part, never a configured active file.
    ftp.rename(partial, remote)


def enable(ftp, names):
    old = io.BytesIO()
    # Refuse to overwrite an unreadable config (including a missing config).
    # The user can open the installed overlay once to create it.
    ftp.retrbinary("RETR " + DATA_DIR + "/config.ini", old.write)
    raw = old.getvalue()
    text = raw.decode("utf-8-sig")
    updated = config_with_local(text, names).encode()
    backup = DATA_DIR + "/config.ini.before-local-" + str(time.time_ns())
    ftp.storbinary("STOR " + backup, io.BytesIO(raw))
    ftp.storbinary("STOR " + DATA_DIR + "/config.ini.local.tmp", io.BytesIO(updated))
    # VitaShell rename can reject an existing destination. Keep the old file
    # under a second name, then restore it if the replacement fails.
    previous = backup + ".active"
    ftp.rename(DATA_DIR + "/config.ini", previous)
    try:
        ftp.rename(DATA_DIR + "/config.ini.local.tmp", DATA_DIR + "/config.ini")
    except ftplib.all_errors:
        ftp.rename(previous, DATA_DIR + "/config.ini")
        raise
    print("Enabled local dictionaries. Previous settings: " + backup)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("vita", help="Vita IP address (VitaShell FTP)")
    parser.add_argument("files", nargs="+", type=Path, help="converted .vjdict files, in priority order")
    parser.add_argument("--port", type=int, default=1337)
    parser.add_argument("--enable", action="store_true", help="select these files in the existing config.ini")
    args = parser.parse_args()
    try:
        if len(args.files) > 8:
            raise ValueError("install at most 8 files at a time; combine more ZIPs with the converter")
        inspected = [inspect(path) for path in args.files]
        suffix = "-" + secrets.token_hex(4) + ".vjdict"
        names = [name[:-7] + suffix for name in inspected]
        if len(names) != len(set(names)):
            raise ValueError("the same dictionary was selected more than once")
        with ftplib.FTP() as ftp:
            ftp.connect(args.vita, args.port, timeout=30)
            ftp.login()
            mkdirs(ftp)
            for path, name in zip(args.files, names):
                transfer(ftp, path, DICT_DIR + "/" + name)
                print("Installed " + name)
            if args.enable:
                enable(ftp, names)
            else:
                print("Set dictionary = local and local_dictionaries = " + ",".join(names))
    except (OSError, ValueError, *ftplib.all_errors) as exc:
        parser.exit(1, f"Install failed: {exc}\nIf config.ini is missing, open the installed overlay once, then retry.\n")
    print("Close VitaShell and reopen the overlay to load the dictionaries.")


if __name__ == "__main__":
    main()
