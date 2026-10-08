# Local dictionaries on PS Vita

Local lookup reads dictionaries from the Vita's storage. It needs no running
computer, dictionary API key, Hachidori Relay, or dictionary network request.
**Screen recognition still uses Google Lens and requires internet.** Anki and
optional Anki audio retain their existing network requirements.

## Convert and install

1. Download a **Yomitan format 3 term dictionary ZIP**, such as a Jitendex
   Yomitan export. Leave the ZIP compressed. On your computer, use Python 3.9+
   (no pip packages required):

   ```sh
   python3 tools/convert_dictionary.py Jitendex.zip -o main.vjdict
   ```

   On Windows, use `python` if that is the installed Python command. HoshiDicts
   database/WASM files are not the input format; use the original Yomitan ZIPs.

2. Close any running game. Copy `main.vjdict` with VitaShell USB/FTP to:

   ```text
   ux0:data/VitaJPOverlay/dictionaries/main.vjdict
   ```

   Create the `dictionaries` folder if necessary. Finish copying before enabling
   it. A ZIP is not installable on the Vita until converted.

3. In `ux0:data/VitaJPOverlay/config.ini`, set:

   ```ini
   dictionary = local
   local_dictionary_dir = ux0:data/VitaJPOverlay/dictionaries
   local_dictionaries = main.vjdict
   ```

   Return to your game and open the overlay. No reboot is required for dictionary
   changes. The updated plugin binaries themselves require the usual reboot.

Alternatively, after installing the plugin and opening the overlay once to
create `config.ini`, start VitaShell's FTP server and run:

```sh
python3 tools/install_dictionary.py 192.168.1.30 main.vjdict --enable
```

Replace the IP with your Vita's. The port defaults to 1337; `--port` changes it.
The installer uploads under a new versioned filename, first as `.part`, and
renames it only after the upload succeeds. `--enable` backs up your configuration
and selects the uploaded dictionaries, preserving API keys and Anki settings.
It refuses to overwrite a configuration it cannot read. If the connection drops
during configuration replacement, restore a `config.ini.before-local-*` backup
as `config.ini` in VitaShell. Keep games closed while
installing. Without `--enable`, it only uploads and prints the configuration.
Older dictionary versions remain available; remove unused files in VitaShell
when you no longer need them. Remove a name from `local_dictionaries` to disable
that dictionary before deleting its file.

### Upgrading from the first local build

Install the updated plugin binaries and reboot, then reconvert your original
Yomitan ZIPs with the included converter. New conversions use the faster
version 2 index. Upload and select the new files as above. Existing version 1
files remain readable, including alongside version 2 files, but keep their
older index. `--format-version 1` is available when converting for an older
plugin; older plugins cannot read version 2 files.

## Several dictionaries

Up to eight `.vjdict` files can be enabled in priority order:

```ini
local_dictionaries = jitendex.vjdict,grammar.vjdict,names.vjdict
```

Filenames must contain only ASCII letters, digits, underscores, hyphens and
single dots, be at most 63 bytes, and end in `.vjdict`. No relative directory
paths or `..` are allowed in this list. The directory is configured separately.
You can also combine several ZIPs into one file, in priority order:

```sh
python3 tools/convert_dictionary.py Jitendex.zip Grammar.zip Names.zip -o main.vjdict
```

Each definition includes its source dictionary title. The converter preserves
separate senses/entries and indexes both spelling and reading without duplicating
the definition payload. Earlier dictionaries and higher Yomitan term scores
have priority for equal keys. Reconvert the original ZIPs to update a combined
file. The converter writes to a temporary file and replaces the destination
only after success, so a failed conversion preserves the previous dictionary.

## Memory and lookup behavior

The plugin does **not** unzip banks, build an in-memory map, load a whole index,
or memory-map dictionaries. A term hashes directly to a 4 KiB index bucket on
storage; a hit verifies the full key before reading its definitions. Four shared
4 KiB page buffers and a small absent-hash cache reduce repeated reads. These
caches are shared across all enabled files and released after each request.
Dictionaries are opened for a lookup and closed on both success and error;
no dictionary-sized persistent allocation is added.

| Resource | Bound |
| --- | --- |
| Lookup result arena | 64 KiB |
| Lookup scratch, including page caches and deinflection candidates | At most 48 KiB; compile-time checked |
| Temporary lookup arena total | At most 112 KiB, taken from the existing result arena |
| Existing worker result arenas | Unchanged: two 384 KiB arenas |
| Enabled dictionary files | 8 |
| Input text | 4,096 UTF-8 bytes |
| Longest scanned term | 32 Unicode code points |
| Deinflection candidates per substring | 128 |
| Results per matched position | First 8 |
| Same-key index rows examined per candidate/file | First 64 |
| Total vocabulary entries / tokens per request | 64 each |

Unused result capacity and the entire scratch allocation are released after
lookup; only the actual returned vocabulary/tokens remain for the overlay.
These numbers describe dictionary lookup, not total process memory: the plugin
also needs code, stacks, OCR/JPEG buffers, formatted overlay text and optional
Anki buffers. If the existing arena has insufficient space, lookup fails with an
explicit memory error; it never grows the worker allocation.

Longer phrases take precedence. Katakana and hiragana readings share a search
key; full-width ASCII is folded to ASCII. Yomichan's Japanese suffix rules handle
inflections such as `食べました`, `行った`, `高くなかった`, and `来ました`.
The matched original text length is retained for highlighting and Anki mining.
Part-of-speech rules from the dictionary filter deinflected candidates.
This is bounded greedy term matching, not a full Japanese morphological parser.
The caps above can omit lower-priority matches or unusually complex inflections.
Missing or corrupt enabled files fail the lookup with an actionable error;
there is no automatic relay/cloud fallback.

The 64 KiB result budget includes text. If a screen exceeds that budget or the
64-token limit, select a smaller OCR region or enable fewer dictionaries. This
avoids hiding an incomplete screen behind an apparently successful result.

## Supported content and tradeoffs

- Term dictionaries only. Frequency-only, pitch-accent-only and kanji-only ZIPs
  are rejected with an explanation. Frequency ranks/pitch data are not imported.
- Plain text, HTML glossaries and Yomitan structured content are flattened to
  readable text. Block boundaries are retained. Images use alt/title text or an
  omitted-image label; CSS, media and interactive links are not rendered.
- The converter caps each definition at 4,096 UTF-8 bytes by default, appends
  `[definition truncated]`, and reports the count. Change this with
  `--max-definition-bytes 2048` (allowed range 256–7,000) to trade detail for
  more results per screen.
- Headwords/readings beyond the supported term length are skipped and counted.
  Half-width katakana, arbitrary Unicode normalization, and custom HoshiDicts
  database files are not supported by this local format.
- Source ZIPs are read on the computer one JSON row at a time, with an 8 MiB
  per-row safety limit. SQLite sorts the index on disk with a 4 MiB page cache.
  Allow temporary disk space for the payload, sorting database and final output;
  `--temp-dir` chooses the scratch disk. Installation size is printed at the end.
- Storage latency replaces RAM usage: slow SD cards and many dictionaries can
  make lookups slower. Host measurements do not establish Vita hardware latency.

## Verification and development

```sh
cmake -S . -B build/host -G Ninja
cmake --build build/host
ctest --test-dir build/host --output-on-failure
./build/host/vjo-cli --dict local --local-dir . --local-dicts main.vjdict \
  --text '猫は食べました。' --nav --stats
python3 tools/benchmark_local_dictionary.py --entries 1000000
```

CTest runs conversion, real disk lookup, deinflection, UTF-16 highlights,
structured/HTML glossaries, dictionary priority, format corruption, interrupted
configuration replacement, and memory-size regression checks. Worker tests cover
local lookup without API keys and changing dictionaries during an in-flight job.
The generated deinflection table is checked against its pinned source.

### Host benchmark (2026-10-08)

With Jitendex (436,549 entries), the new index cuts two sentence workloads
from 3,005 / 3,267 file reads to 116 / 106. Host lookup times improve 15–17×,
with identical definitions and highlights. Lookup arena peaks stay below
108 KiB, inside the existing 112 KiB cap. The converted file grows from
88.5 MiB to 98.3 MiB (about 11%). See the [benchmark report](lookup-performance.md)
for all queries, raw measurements, methodology and reproduction commands.

The benchmark driver times the lookup pipeline without process startup, counts
file reads, and checks result equality. Application caches start empty for
each request; the host OS file cache is warm. These are **not physical Vita or
SD-card measurements**. The synthetic size/memory benchmark above uses the
same driver, and regression tests check that arena use does not grow with
dictionary size.

### Binary format, version 2 (default)

All integers are little-endian unless explicitly stated. No C structs are cast
onto disk data. The 64-byte header contains magic `VJDICT2\0`; four u32 values
(version=2, header size=64, total posting count, hash-record stride=24); then
five u64 values (index offset=4096, data offset, exact file size, primary bucket
count, total index page count). The header is padded to 4 KiB. Bucket count is
a power of two; the data region follows all index pages.

Keys use FNV-1a-64 followed by the avalanche finalizer implemented in
`key_hash()` in the converter and runtime. The low bits select a primary
bucket. Each 4 KiB index page has a 16-byte header: magic `VJHP`, u32 record
count (at most 170), and u64 next-page offset (zero for none). Overflow pages
follow all primary pages, have increasing offsets, and are limited to a
64-page chain. Records are sorted by unsigned hash within each bucket.

Each 24-byte record contains u64 hash, u64 key-group offset, u32 posting count,
u16 key length and u16 union of deinflection rule bits. A group holds the raw
UTF-8 key followed by 16-byte postings: u64 payload offset, u32 payload length,
u16 rule bits and u16 reserved=0. Postings retain dictionary priority,
descending term score and original row order. Hash collisions are resolved
by comparing the full key. All offsets are absolute.

Payloads are three UTF-8 NUL-terminated strings: spelling, reading, definition.
Spelling and reading indexes share payloads. Rule bits are v1=1, v5=2, vs=4,
vk=8, vz=16, adj-i=32, iru=64. Opening a dictionary reads only its header;
pages, offsets, bounds and payloads are validated as they are accessed.

### Binary format, version 1 (legacy, still readable)

All integers are little-endian. No C structs are cast onto disk data. The header
is 64 bytes: magic `VJDICT1\0`; four u32 values (version=1, header size=64,
record count, record stride=24); then five u64 values (index offset=64,
data offset, exact file size, reserved=0, reserved=0).

Each index record is 24 bytes: u64 key offset, u64 entry offset, u16 key byte
length, u16 deinflection rule bits, u32 entry byte length. Offsets are absolute.
Keys are sorted by unsigned UTF-8 bytes, then dictionary priority, descending
term score and original row order. The data region holds raw keys and payloads.
A payload is three UTF-8 NUL-terminated strings: spelling, reading, definition.
Rule bits are v1=1, v5=2, vs=4, vk=8, vz=16, adj-i=32, iru=64.

The runtime validates header layout, exact file length, offsets, record sizes
and string termination before use. Validation is incremental so opening a large
file never scans the whole dictionary. It is not a cryptographic integrity check.
