# Lens connections and response framing

Lens now uses the upstream connection pool and early scene-triggered warming.
Repeated requests can reuse the DNS/TCP/TLS work. The request still streams a
single software JPEG, with a small protobuf prefix and suffix, through BearSSL.
BearSSL already buffers these writes before receiving the response; no extra
image copy or upload retry was added.

The request's Japanese locale now puts `JP` in region field 2 and
`Asia/Tokyo` in time-zone field 3. These fields follow the
[pinned GSM schema](https://github.com/bpwhelan/GameSentenceMiner/blob/a2f15fc670460002e7fffac11f8a6685339eedc7/GameSentenceMiner/owocr/owocr/lens_betterproto.py#L265-L276).
GSM also leases persistent connections exclusively between scans, as seen in
[its transport implementation](https://github.com/bpwhelan/GameSentenceMiner/blob/a2f15fc670460002e7fffac11f8a6685339eedc7/GameSentenceMiner/owocr/owocr/ocr.py#L1685-L1714).
This fork's one network worker uses its own bounded pool.

Keeping a connection requires a complete, unambiguous HTTP response. The
parser now rejects truncated or overlong lines, invalid lengths/transfer
coding, malformed chunk sizes, and missing chunk/trailer terminators. It
recognizes `close` in a connection-token list and consumes bounded `100`/`103`
informational responses before the final reply. Malformed responses close
their connection. Framing follows
[RFC 9112](https://www.rfc-editor.org/rfc/rfc9112.html#section-6.3).
Lens logs include image dimensions, JPEG bytes, and protobuf bytes; recognized
text, request contents, and headers are excluded.

## Live host measurement, 2026-10-10

The production C Lens/BearSSL path sent two dialogue crops from the private,
hand-checked Vita/PSP manifests. Each crop was encoded once by the production
software JPEG encoder at quality 80. Fresh and pooled request order alternated
within each pair. Each case had six fresh requests, one pooled cold request,
and five pooled repeat requests, with 500 ms between requests.

| Crop | JPEG bytes | Fresh median (range), ms | Pooled repeat median (range), ms |
| --- | ---: | ---: | ---: |
| Vita, 409 × 104 | 10,070 | 344.053 (303.501–425.685) | 278.372 (268.716–318.125) |
| PSP, 168 × 74 | 4,164 | 396.393 (362.733–435.172) | 348.912 (335.065–403.327) |

All 24 responses succeeded and had identical output hashes within each case.
All ten pooled repeats reused their connection with zero new connections.
Fresh requests spent 14–32 ms connecting and 42–51 ms on TLS in this run.
Protobuf added 88–89 bytes to each JPEG. The random UUID's varint length explains
the one-byte variation.

These are Linux x86-64 WAN measurements. Server response time varies; the
overlapping ranges do not establish a fixed speedup. Capture and encoding time
are excluded, and repeat parity is not a CER measurement. Physical Vita
performance remains unmeasured for this change.

The host arena peaked at 34,216 bytes for fresh requests and 10,528 bytes for
pooled requests. The pool separately retained 23,800 bytes on this host ABI.
Pooling moves TLS state out of the per-request arena; these numbers do not
show a reduction in total memory. Screenshots and OCR text stay private.
Hashes, every timing sample, source hashes, and environment are in
[the numeric results](benchmarks/lens-connections-20261010.json).

Build and repeat with private, pre-encoded JPEGs:

```sh
cmake -S . -B build-host -G Ninja -DVJO_WITH_NCNN=OFF
cmake --build build-host --target vjo-lens-bench vjo-lens-transport-tests
ctest --test-dir build-host -R lens_transport --output-on-failure
python3 tools/benchmark_lens.py --cli build-host/vjo-lens-bench \
  --case vita-test-dialogue PRIVATE_VITA_JPEG 409 104 \
  --case psp-test-dialogue PRIVATE_PSP_JPEG 168 74 \
  --runs 6 --out lens-connections.json
```

The live benchmark stops on the first failed response, changed output hash, or
repeat that did not reuse a connection. It performs no dictionary lookup.
