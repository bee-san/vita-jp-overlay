# Disk lookup benchmark and index design

Compared with the first local backend (`23fd3e0`), the new index cuts the two
sentence workloads from 3,005 / 3,267 file reads to 116 / 106 (96.1% / 96.8%
fewer). The returned definitions, ordering and highlight ranges match exactly.
Host lookup time improves about 15–17× for those sentences. These are **host
measurements**, not measured Vita or SD-card latency.

## What changed

1. **Page-sized hash buckets.** A normalized term hashes directly to a 4 KiB
   index page containing full 64-bit hashes. An ordinary miss needs one page
   read rather than a binary search repeatedly fetching scattered keys.
   A hit verifies the full key and reads its adjacent posting list; hashes
   are never accepted as proof of equality. Rare overflow pages handle
   crowded buckets. Dictionary size affects storage, not allocated RAM.
2. **Four shared LRU pages.** All enabled files share 16 KiB of page buffers.
   Repeated searches can reuse pages; this does not multiply per dictionary.
3. **A 64-slot absent-hash cache.** Proven misses are remembered for the current
   request. Filtered senses and duplicate results are not negative-cache entries,
   and a hash collision cannot create a false negative.
4. **Suffix dispatch.** Deinflection visits only rules with the matching final
   UTF-8 byte, preserving their previous order and all 569 source rules. It no
   longer scans the entire rule table for every candidate.

## Results

Jitendex Yomitan release **2026.10.03.0**, 436,549 imported entries / 662,343
index records / 558,655 unique normalized keys. The converter reports the same
13 truncated definitions and 6 skipped long headwords in both formats.

| Query | Old reads | New reads | Old median | New median | Host speedup |
| --- | ---: | ---: | ---: | ---: | ---: |
| 図書館 | 36 | 5 | 13.310 µs | 4.346 µs | 3.06× |
| としょかん | 32 | 4 | 13.155 µs | 3.966 µs | 3.32× |
| 食べませんでした | 186 | 9 | 56.059 µs | 6.890 µs | 8.14× |
| 今日は学校に行って、友達と話しました。 | 3,005 | 116 | 923.822 µs | 61.057 µs | 15.13× |
| 彼女の名前を思い出せなかった。 | 3,267 | 106 | 1004.102 µs | 59.555 µs | 16.86× |
| 猫。猫。猫。猫。猫。 | 168 | 5 | 50.305 µs | 5.137 µs | 9.79× |
| 霻霻霻霻霻霻霻霻 | 1,261 | 11 | 394.447 µs | 8.142 µs | 48.45× |

Lookup arena peaks range from **110,192 to 110,288 bytes (107.6–107.7 KiB)**
for these queries, versus 100,824–100,920 bytes before. The additional 9,368
bytes fit inside the existing **112 KiB lookup cap**: 64 KiB result capacity
plus at most 48 KiB scratch, checked at compile time. The worker still uses
two 384 KiB arenas. These figures do not include all process memory or stacks.

The tradeoff is storage: **92,845,647 → 103,097,379 bytes** (88.5 → 98.3 MiB,
about 11.0% larger) for this Jitendex conversion. The primary index has 4,096
buckets plus 15 overflow pages. A target occupancy of 140 out of 170 slots
keeps padding modest while avoiding long overflow chains. Conversion does
more work on the computer to build the grouped, hashed index.

## Method and limits

- `vjo-local-bench` calls the real overlay-from-text pipeline, with file callbacks
  that count reads, requested bytes, nonsequential offsets, and handle closure.
  The timer excludes process startup, OCR, terminal rendering and network calls.
- Each case has one unreported timing warmup and 100 measured requests. The
  application page/miss caches start empty on **every request**. The Linux OS
  file cache is warm; counted read calls are not claimed to be physical reads.
- A digest covers every displayed entry and its first-token/highlight coordinates.
  The comparison aborts if behavior differs; all seven cases match.
- Fixed caches are released after lookup. There is no across-screen resident
  dictionary cache or in-memory index whose size grows with installed data.
- SD-card random I/O is the hardware risk these changes address. For illustration
  only, an assumed 0.2 ms cost per read would contribute 601 ms before versus
  23.2 ms after for the first sentence. That is an arithmetic model, **not a Vita
  prediction**; actual device measurements are still needed.
- Tests cover v1 compatibility, mixed formats, deliberately colliding hashes,
  overflow buckets, invalid chains/offsets/postings, result equality, I/O reduction
  and the memory limit. ASan/UBSan also pass (LeakSanitizer is disabled because
  the execution environment restricts its process inspection).

[Raw measurements and source checksum](benchmarks/local-lookup-jitendex.json).
The benchmark dataset is not bundled. [Jitendex source release](https://github.com/stephenmk/stephenmk.github.io/releases/tag/2026.10.03.0).

## Reproduce

Build the current code and benchmark tool. Build the baseline with the same
compiler/options and the same instrumented driver:

```sh
git worktree add --detach .refs/lookup-before 23fd3e0
cp host/local_bench.c .refs/lookup-before/host/
cat >> .refs/lookup-before/CMakeLists.txt <<'CMAKE'
add_executable(vjo-local-bench host/local_bench.c)
target_link_libraries(vjo-local-bench vjo_host)
CMAKE
cmake -S .refs/lookup-before -B .refs/lookup-before/build/host -G Ninja
cmake --build .refs/lookup-before/build/host
cmake -S . -B build/host -G Ninja
cmake --build build/host
```

Place the pinned Jitendex ZIP in `build/lookup-benchmark/`, verify its SHA-256
against the raw report, and convert the exact same input into both formats:

```sh
python3 tools/convert_dictionary.py build/lookup-benchmark/jitendex.zip \
  --format-version 1 -o build/lookup-benchmark/jitendex-v1.vjdict
python3 tools/convert_dictionary.py build/lookup-benchmark/jitendex.zip \
  -o build/lookup-benchmark/jitendex-v2.vjdict
python3 tools/compare_lookup_benchmarks.py \
  --before .refs/lookup-before/build/host/vjo-local-bench \
  --after build/host/vjo-local-bench \
  --directory build/lookup-benchmark --runs 100
```

The query set is in `tools/lookup_benchmark_cases.json`; `--cases` accepts
another set. `--before-dicts` and `--after-dicts` accept comma-separated files
for multi-dictionary comparisons. `tools/benchmark_local_dictionary.py` remains
a reproducible synthetic size/memory stress test and now uses the timed driver
instead of timing CLI process startup.
