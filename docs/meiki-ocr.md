# Experimental Meiki OCR

Meiki is optional local OCR for selected horizontal dialogue.
Select `ocr_backend = meiki` explicitly. It runs only on request, including
when an older config says `ocr_mode = auto`, and errors never upload an image
to Lens. Lens remains the default; the existing ncnn backend remains available.

Select one horizontal line with Square and save it with Cross. Keep a narrow
margin around its glyphs, include ruby belonging to that line, and exclude
adjacent dialogue, speaker labels and decorations. It recognizes that exact
selection without a white-text threshold, so dark or dim text is accepted.
This is the default `meiki_layout = single_line` path. Alternatively, select
the dialogue area and set `meiki_layout = dialogue_box` to run Meiki's existing
320x192 detector, then recognize its crops in vertical order. It accepts at
most eight crops and refuses a larger result rather than publishing partial
text. Keep unrelated UI outside the selection. Vertical writing is unsupported.
Both paths bind existing author-trained Meiki models.

## Build

The Meiki source and MNN pins, model hashes, conversion instructions and local
accuracy results are in [vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr/tree/2eaa0592a96583cd73fac7e21374cfc63dc4009e/ports/meiki).
That source repository is private, so building this optional backend requires
access. Hosted CI exercises the default backend; the optional Meiki module
and portable adapter are checked locally.

Use its patched MNN 3.2.0 static build with `MNN_ENABLE_EXCEPTIONS=ON` and
`MNN_LOW_MEMORY=ON`. Build in private directories; models and screenshots do
not belong in Git. The build checks the imported source files against their
evaluated hashes; [meiki-dependencies.json](meiki-dependencies.json) records
the immutable source and model pins.

```sh
git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr
git -C /path/to/vita-vn-ocr checkout 2eaa0592a96583cd73fac7e21374cfc63dc4009e
cmake -S vita -B build/vita-meiki \
  -DVJO_WITH_NCNN=OFF -DVJO_WITH_MEIKI=ON \
  -DVJO_MEIKI_SOURCE_DIR=/path/to/vita-vn-ocr/ports/meiki \
  -DMNN_SOURCE=/path/to/patched/MNN \
  -DMNN_LIBRARY=/path/to/mnn-vita/libMNN.a
cmake --build build/vita-meiki --target release
```

The optional release contains `ur0:data/VitaJPOverlay/meiki-engine.suprx`.
The models are separate: convert them with the pinned instructions and copy the
5,392,856-byte `meiki-stream-int8.mnn` to
`ux0:data/VitaJPOverlay/meiki/`. Shell checks its size and SHA-256 before MNN
parses it. Dialogue-box mode also requires the 4,119,220-byte
`meiki-detect-int8.mnn` in that directory, checked against its own SHA-256.
Other model files are rejected before the inference buffers are allocated.

```ini
ocr_backend = meiki
ocr_model_dir = ux0:data/VitaJPOverlay/meiki
ocr_mode = on_press
meiki_layout = single_line
```

For local OCR and dictionary lookup, configure `dictionary = local` and
install a [local dictionary](local-dictionaries.md). Other dictionaries and
Anki retain their own network requirements.

## Memory and validation boundary

Shell borrows a 16 MiB neural workspace, a 4 MiB private newlib/C++ heap,
and 382,080 bytes for single-line preprocessing (750,720 bytes for dialogue-box
mode) from its existing Paf heap, retaining a
2 MiB Paf reserve. A dedicated module thread performs all SDK C++ operations
and avoids replacing newlib TLS on Shell's threads. No C++ object crosses
the module boundary. The detector releases its model/session before the
recognizer reuses the same workspace. All three buffers are released after successful
stop/unload, before dictionary lookup. If cleanup cannot be established,
the buffers remain owned and another module load is refused.
Shell also refuses its own unload until worker joins and Meiki/Anki cleanup
succeed, retaining the remaining resources so stopping can be retried.

Those reservations are not total RAM usage. Module code, thread stacks,
the kernel capture buffer (approximately 2 MiB), result arenas and heap
fragmentation are additional. Allocation failure returns an OCR error.
The measured module ELF sections total 1,777,240 bytes and its dedicated engine
stack is 128 KiB; page and kernel overhead are unmeasured.
`log_file = on` records pool use, metadata break highwater, line counts and
job duration. The control thread prevents idle sleep while a job runs.

Host preprocessing matches upstream Meiki on all 451 hand-checked crops.
The direct selected-line path gives 36/2,564 character errors (1.404%) on
Vita crops and 89/2,510 (3.546%) on PSP crops. Doubling crop margins increases
those errors to 3.861% and 6.853%; precise selection matters. The earlier
white-text line finder produced 40.822%/65.060% errors on complete selected
Vita/PSP regions, so this backend bypasses it.

The bounded dialogue-box path gives 166/2,506 errors (6.624%) on 82 wholly
known Vita regions and 312/2,510 (12.430%) on 93 PSP regions. One PSP region
is refused because it exceeds eight detected crops; its missing text counts
as errors. Two additional Vita regions contain uncertain reference text and
are exercised separately. These are PC results from two complete passes:
176/177 regions succeed, all predictions repeat, and no neural arena remains
allocated. Detection and recognition share a peak of 13,235,648 bytes. Region
accuracy depends on detection, crop bounds and reading order as well as the
recognizer. The ARM detector can shift crop bounds by one pixel, so these
whole-region PC figures must not be presented as native accuracy.

The complete ARM recognizer run in Vita3K gives 37/2,564 errors (1.443%) on
Vita crops and 92/2,510 (3.665%) on PSP crops. All 451 C input tensors match
upstream; all inferences succeed and release their neural allocations. Fifteen
texts differ from PC output. Native neural peak is 13,090,176 bytes and the
separate metadata break highwater is 2,764,800 bytes, with no per-job growth.
That full matrix uses the frozen recognizer-only ABI1 module; the final ABI2
module separately reproduces 16 baseline raw outputs and completes detector
and recognizer jobs within the same bounds. An oversized detector's allocation
failure also stops/unloads cleanly before its caller-owned heaps are released.
Host crop accuracy and ARM emulator results do not establish available Paf
memory, latency or stability beside a game on a physical Vita. Test the
standalone manual app first, then this optional overlay build manually.

The host adapter can be built without Vita SDK or MNN; its injected-engine
tests cover cancellation, unsupported input, partial-result suppression and
memory failures. To also build `vjo-meiki-ocr`, pass a patched host
`MNN_SOURCE` and `MNN_LIBRARY`.

```sh
cmake -S . -B build/host-meiki -DVJO_WITH_NCNN=OFF -DVJO_WITH_MEIKI=ON \
  -DVJO_MEIKI_SOURCE_DIR=/path/to/vita-vn-ocr/ports/meiki
cmake --build build/host-meiki
ctest --test-dir build/host-meiki --output-on-failure
```
