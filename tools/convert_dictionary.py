#!/usr/bin/env python3
"""Convert Yomitan v3 term ZIPs to a bounded-memory Vita dictionary.

Python 3.9+ standard library only. ZIP banks are streamed one row at a time;
SQLite sorts the index on disk, and definitions are never retained wholesale.
"""
import argparse
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import sqlite3
import struct
import sys
import tempfile
import zipfile
from html.parser import HTMLParser

MAGIC = b"VJDICT1\0"
HEADER = struct.Struct("<8sIIIIQQQQQ")
RECORD = struct.Struct("<QQHHI")  # Version 1 lexical index.
MAGIC_V2 = b"VJDICT2\0"
PAGE_SIZE = 4096
HASH_RECORD = struct.Struct("<QQIHH")
POSTING = struct.Struct("<QIHH")
PAGE_HEADER = struct.Struct("<4sIQ")
HASH_PAGE_RECORDS = (PAGE_SIZE - PAGE_HEADER.size) // HASH_RECORD.size
MAX_HASH_CHAIN = 64
MAX_KEY = 192
MAX_SCAN_CHARS = 32
MAX_ENTRY = 8192
MAX_ROW_CHARS = 8 * 1024 * 1024
MAX_INDEX_BYTES = 1024 * 1024
RULES = {"v1": 1, "v5": 2, "vs": 4, "vk": 8, "vz": 16, "adj-i": 32, "iru": 64}


def normalize(text):
    """Must match core/local_dict.c, without lossy/cross-platform collation."""
    chars = []
    for char in text:
        cp = ord(char)
        if 0x30A1 <= cp <= 0x30F6:
            cp -= 0x60
        if 0xFF01 <= cp <= 0xFF5E:
            cp -= 0xFEE0
        chars.append(chr(cp))
    return "".join(chars).encode("utf-8")


def clean(text):
    return "".join(c for c in text if c in "\n\t" or ord(c) >= 0x20).replace("\x7f", "")


def utf8_clip(text, size):
    return text.encode("utf-8")[:size].decode("utf-8", "ignore")


class PlainHTML(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.parts = []
        self.hidden = 0

    def handle_starttag(self, tag, attrs):
        if tag in ("script", "style"):
            self.hidden += 1
        if not self.hidden and tag in ("br", "div", "p", "li", "tr"):
            self.parts.append("\n")
        if tag == "img" and not self.hidden:
            self.parts.append(dict(attrs).get("alt", "[image omitted]"))

    def handle_endtag(self, tag):
        if tag in ("script", "style"):
            self.hidden = max(0, self.hidden - 1)
        if not self.hidden and tag in ("div", "p", "li", "tr"):
            self.parts.append("\n")

    def handle_data(self, data):
        if not self.hidden:
            self.parts.append(data)


def glossary_text(value, depth=0):
    if depth > 64:
        raise ValueError("structured glossary nesting exceeds 64 levels")
    if isinstance(value, str):
        if re.search(r"</?[A-Za-z][^>]*>", value):
            parser = PlainHTML()
            parser.feed(value)
            return "".join(parser.parts)
        return value
    if isinstance(value, list):
        return "".join(glossary_text(v, depth + 1) for v in value)
    if isinstance(value, dict):
        kind, tag = value.get("type"), value.get("tag")
        if kind == "image" or tag == "img":
            return str(value.get("alt", value.get("title", "[image omitted]")))
        if kind == "text":
            return glossary_text(value.get("text", ""), depth + 1)
        if tag in ("script", "style", "rt", "rp"):
            return ""
        if tag == "br":
            return "\n"
        result = glossary_text(value.get("content", ""), depth + 1)
        if tag in ("div", "p", "li", "ul", "ol", "tr", "table"):
            return "\n" + result + "\n"
        if tag in ("td", "th"):
            return result + " "
        return result
    raise ValueError("glossary must contain strings or structured content")


def stream_array(stream, chunk_size=65536):
    """Incrementally parse a JSON array, including strict separator/EOF checks."""
    decoder = json.JSONDecoder()
    buf, pos, eof = "", 0, False

    def more():
        nonlocal buf, pos, eof
        buf = buf[pos:]
        pos = 0
        part = stream.read(chunk_size)
        eof = not part
        buf += part
        if len(buf) > MAX_ROW_CHARS:
            raise ValueError("one JSON row exceeds the 8 MiB conversion limit")

    def skip():
        nonlocal pos
        while True:
            while pos < len(buf) and buf[pos].isspace():
                pos += 1
            if pos < len(buf) or eof:
                return
            more()

    skip()
    if pos >= len(buf) or buf[pos] != "[":
        raise ValueError("term bank must be a JSON array")
    pos += 1
    skip()
    if pos < len(buf) and buf[pos] == "]":
        pos += 1
    else:
        while True:
            skip()
            while True:
                try:
                    value, end = decoder.raw_decode(buf, pos)
                    break
                except json.JSONDecodeError as exc:
                    if eof:
                        raise ValueError("invalid or truncated term bank JSON") from exc
                    more()
            pos = end
            yield value
            skip()
            if pos >= len(buf):
                raise ValueError("unterminated term bank array")
            char = buf[pos]
            pos += 1
            if char == "]":
                break
            if char != ",":
                raise ValueError("expected comma between term bank rows")
    skip()
    if pos < len(buf):
        raise ValueError("unexpected content after term bank array")


def rule_flags(value):
    flags = 0
    for token in value.split():
        if token.startswith("v5"):
            token = "v5"
        elif token.startswith("vs"):
            token = "vs"
        flags |= RULES.get(token, 0)
    return flags


def read_index(archive):
    info = archive.getinfo("index.json")
    if info.file_size > MAX_INDEX_BYTES:
        raise ValueError("index.json exceeds 1 MiB")
    data = json.loads(archive.read(info).decode("utf-8-sig"))
    if not isinstance(data, dict) or data.get("format") != 3:
        raise ValueError("expected a Yomitan format 3 dictionary")
    title = data.get("title")
    if not isinstance(title, str) or not title.strip():
        raise ValueError("index.json has no dictionary title")
    return utf8_clip(clean(title.strip()).replace("\n", " "), 128)


def key_hash(key):
    """FNV-1a-64 plus avalanche; identical to core/local_dict.c."""
    mask = (1 << 64) - 1
    h = 14695981039346656037
    for byte in key:
        h = ((h ^ byte) * 1099511628211) & mask
    h = ((h ^ (h >> 33)) * 0xff51afd7ed558ccd) & mask
    h = ((h ^ (h >> 33)) * 0xc4ceb9fe1a85ec53) & mask
    return h ^ (h >> 33)


def write_v2(db, data, final, count):
    # Sorting and all per-key metadata live on the computer's disk, not the Vita.
    db.execute("CREATE INDEX lookup_order ON lookups(key, priority, score, seq)")
    unique = db.execute("SELECT COUNT(DISTINCT key) FROM lookups").fetchone()[0]
    buckets = 1
    # About 82% target occupancy; rare overflow pages keep padding modest.
    while buckets * 140 < unique:
        buckets *= 2
    counts = [0] * buckets
    db.execute("CREATE TABLE hash_keys(key BLOB PRIMARY KEY, hash BLOB, bucket INTEGER, n INTEGER, off INTEGER, rules INTEGER)")
    for (key,) in db.execute("SELECT DISTINCT key FROM lookups ORDER BY key"):
        h = key_hash(key)
        bucket = h & (buckets - 1)
        counts[bucket] += 1
        # Big endian blob order is unsigned integer order in SQLite.
        db.execute("INSERT INTO hash_keys VALUES (?,?,?,0,0,0)", (key, h.to_bytes(8, "big"), bucket))
    page_counts = [max(1, (n + HASH_PAGE_RECORDS - 1) // HASH_PAGE_RECORDS) for n in counts]
    if max(page_counts) > MAX_HASH_CHAIN:
        raise ValueError("hash bucket chain exceeds the supported limit")
    pages = sum(page_counts)
    data_off = PAGE_SIZE * (1 + pages)
    final.seek(data_off)
    data.seek(0)
    shutil.copyfileobj(data, final, length=1024 * 1024)
    current, group_off, n, rules_union = None, 0, 0, 0
    for key, entry_off, rules, length in db.execute(
            "SELECT key, entry_off, rules, size FROM lookups ORDER BY key, priority, score, seq"):
        if key != current:
            if current is not None:
                db.execute("UPDATE hash_keys SET n=?, off=?, rules=? WHERE key=?", (n, group_off, rules_union, current))
            current, group_off, n, rules_union = key, final.tell(), 0, 0
            final.write(key)
        final.write(POSTING.pack(data_off + entry_off, length, rules, 0))
        n += 1
        rules_union |= rules
    db.execute("UPDATE hash_keys SET n=?, off=?, rules=? WHERE key=?", (n, group_off, rules_union, current))
    size = final.tell()
    if size > 0x7FFFFFFFFFFFFFFF:
        raise ValueError("dictionary exceeds the supported file size")
    final.seek(0)
    final.write(HEADER.pack(MAGIC_V2, 2, HEADER.size, count, HASH_RECORD.size,
                           PAGE_SIZE, data_off, size, buckets, pages))
    records = iter(db.execute("SELECT hash, off, n, key, rules FROM hash_keys ORDER BY bucket, hash, key"))
    overflow = buckets
    for bucket, number in enumerate(page_counts):
        left = counts[bucket]
        page_number = bucket
        for page_in_bucket in range(number):
            take = min(left, HASH_PAGE_RECORDS)
            following = 0 if page_in_bucket + 1 == number else PAGE_SIZE * (1 + overflow)
            block = bytearray(PAGE_SIZE)
            PAGE_HEADER.pack_into(block, 0, b"VJHP", take, following)
            for slot in range(take):
                h, group_off, n, key, flags = next(records)
                HASH_RECORD.pack_into(block, PAGE_HEADER.size + slot * HASH_RECORD.size,
                                      int.from_bytes(h, "big"), group_off, n, len(key), flags)
            final.seek(PAGE_SIZE * (1 + page_number))
            final.write(block)
            left -= take
            if following:
                page_number = overflow
                overflow += 1
    return {"bytes": size, "unique_keys": unique, "hash_buckets": buckets, "index_pages": pages}


def convert(inputs, output, max_definition_bytes=4096, temp_dir=None, format_version=2):
    if format_version not in (1, 2):
        raise ValueError("format-version must be 1 or 2")
    output = Path(output)
    if output.suffix != ".vjdict":
        raise ValueError("output filename must end in .vjdict")
    if not 256 <= max_definition_bytes <= 7000:
        raise ValueError("max-definition-bytes must be between 256 and 7000")
    if any(Path(p).resolve() == output.resolve() for p in inputs):
        raise ValueError("output must not replace an input archive")
    output.parent.mkdir(parents=True, exist_ok=True)
    stats = {"format_version": format_version, "entries": 0, "index_records": 0, "truncated_definitions": 0,
             "skipped_long_headwords": 0, "dictionaries": []}
    # The final temporary file is beside the destination, for atomic replacement.
    final_name = None
    try:
        with tempfile.TemporaryDirectory(prefix="vjo-dict-", dir=temp_dir) as tmp:
            db = sqlite3.connect(str(Path(tmp) / "index.sqlite"))
            try:
                db.executescript("""
                    PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;
                    PRAGMA temp_store=FILE; PRAGMA cache_size=-4096;
                    CREATE TABLE lookups (key BLOB, priority INTEGER, score REAL,
                        seq INTEGER, key_off INTEGER, entry_off INTEGER, rules INTEGER, size INTEGER);
                """)
                with (Path(tmp) / "payload.bin").open("w+b") as data:
                    for priority, source in enumerate(inputs):
                        try:
                            with zipfile.ZipFile(source) as archive:
                                names = archive.namelist()
                                if len(names) != len(set(names)):
                                    raise ValueError("ZIP contains duplicate filenames")
                                title = read_index(archive)
                                stats["dictionaries"].append(title)
                                banks = sorted((n for n in names if re.fullmatch(r"term_bank_[0-9]+\.json", n)),
                                               key=lambda n: int(n[10:-5]))
                                if not banks:
                                    raise ValueError("no term_bank_*.json files (frequency/pitch/kanji-only dictionaries are unsupported)")
                                for name in banks:
                                    with archive.open(name) as raw, io.TextIOWrapper(raw, encoding="utf-8-sig") as bank:
                                        for rownum, row in enumerate(stream_array(bank), 1):
                                            context = f"{name}, row {rownum}"
                                            if not isinstance(row, list) or len(row) != 8:
                                                raise ValueError(f"{context}: expected an 8-field Yomitan term row")
                                            spelling, reading, _, rules, score, gloss, _, _ = row
                                            if (not isinstance(spelling, str) or not spelling or not isinstance(reading, str)
                                                    or not isinstance(rules, str) or not isinstance(gloss, list)
                                                    or not isinstance(score, (int, float)) or isinstance(score, bool)
                                                    or not math.isfinite(score)):
                                                raise ValueError(f"{context}: invalid spelling, reading, rules, score, or glossary")
                                            if clean(spelling) != spelling or clean(reading) != reading:
                                                raise ValueError(f"{context}: headwords contain control characters")
                                            spelling.encode("utf-8")  # Reject lone surrogates before writing.
                                            reading.encode("utf-8")
                                            keys = list(dict.fromkeys(normalize(x) for x in (spelling, reading) if x))
                                            if (max(len(spelling), len(reading)) > MAX_SCAN_CHARS
                                                    or max(len(spelling.encode()), len(reading.encode())) > MAX_KEY
                                                    or any(len(k) > MAX_KEY for k in keys)):
                                                stats["skipped_long_headwords"] += 1
                                                continue
                                            text = "\n".join(glossary_text(g).strip() for g in gloss)
                                            text = re.sub(r"\n[ \t]*\n+", "\n", clean(text)).strip()
                                            definition = f"[{title}]\n{text or '(No text definition)'}"
                                            if len(definition.encode()) > max_definition_bytes:
                                                suffix = "\n[definition truncated]"
                                                definition = utf8_clip(definition, max_definition_bytes - len(suffix)) + suffix
                                                stats["truncated_definitions"] += 1
                                            payload = (spelling + "\0" + reading + "\0" + definition + "\0").encode()
                                            if len(payload) > MAX_ENTRY:
                                                raise ValueError(f"{context}: converted entry is too large")
                                            entry_off = data.tell()
                                            data.write(payload)
                                            flags = rule_flags(rules)
                                            for key in keys:
                                                key_off = data.tell()
                                                if format_version == 1:
                                                    data.write(key)
                                                db.execute("INSERT INTO lookups VALUES (?,?,?,?,?,?,?,?)",
                                                           (key, priority, -score, stats["entries"], key_off, entry_off, flags, len(payload)))
                                                stats["index_records"] += 1
                                            stats["entries"] += 1
                                    db.commit()
                        except (ValueError, KeyError, UnicodeError, zipfile.BadZipFile, RecursionError, OverflowError) as exc:
                            raise ValueError(f"{source}: {exc}") from exc
                    count = stats["index_records"]
                    if not count:
                        raise ValueError("no installable term entries found")
                    if count > 0xFFFFFFFF:
                        raise ValueError("dictionary has too many index records")
                    data_off = HEADER.size + count * RECORD.size
                    size = data_off + data.tell()
                    if size > 0x7FFFFFFFFFFFFFFF:
                        raise ValueError("dictionary exceeds the supported file size")
                    with tempfile.NamedTemporaryFile(prefix=output.name + ".", suffix=".tmp", dir=output.parent,
                                                     delete=False) as final:
                        final_name = Path(final.name)
                        if format_version == 2:
                            stats.update(write_v2(db, data, final, count))
                            size = stats["bytes"]
                        else:
                            final.write(HEADER.pack(MAGIC, 1, HEADER.size, count, RECORD.size,
                                                    HEADER.size, data_off, size, 0, 0))
                            for key, key_off, entry_off, flags, length in db.execute(
                                    "SELECT key, key_off, entry_off, rules, size FROM lookups ORDER BY key, priority, score, seq"):
                                final.write(RECORD.pack(key_off + data_off, entry_off + data_off, len(key), flags, length))
                            data.seek(0)
                            shutil.copyfileobj(data, final, length=1024 * 1024)
                        final.flush()
                        os.fsync(final.fileno())
                    os.replace(final_name, output)
                    final_name = None
                    stats["bytes"] = size
            finally:
                db.close()
    finally:
        if final_name is not None:
            final_name.unlink(missing_ok=True)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", nargs="+", type=Path, help="Yomitan term ZIP(s), in priority order")
    parser.add_argument("-o", "--output", required=True, type=Path, help="output .vjdict file")
    parser.add_argument("--max-definition-bytes", type=int, default=4096)
    parser.add_argument("--format-version", type=int, choices=(1, 2), default=2,
                        help="2: page hash index (default); 1: legacy binary-search format")
    parser.add_argument("--temp-dir", type=Path, help="scratch disk for sorting large dictionaries")
    args = parser.parse_args()
    try:
        stats = convert(args.inputs, args.output, args.max_definition_bytes, args.temp_dir, args.format_version)
    except (ValueError, OSError, sqlite3.Error, RuntimeError) as exc:
        parser.exit(1, f"Conversion failed: {exc}\n")
    print(json.dumps(stats, ensure_ascii=False, indent=2))
    print(f"Saved {args.output}. Copy to ux0:data/VitaJPOverlay/dictionaries/ and enable dictionary = local.")


if __name__ == "__main__":
    main()
