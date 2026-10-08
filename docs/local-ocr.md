# Experimental local OCR (CPU-only ncnn)

This fork can run the **PP-OCRv5 mobile recognizer** directly on the Vita CPU.
It needs no ONNX runtime, Vulkan, OpenCV, phone, or OCR server. Google Lens
remains the default. Select `ncnn` explicitly to use local recognition;
errors never fall back to uploading the screenshot.

**Status:** host recognition tests and VitaSDK cross-builds are supported.
On-console speed, available SceShell memory, and stability while Clannad,
AIR, or Corpse Party is running still need testing on a physical Vita 1000.
This is an experimental option, not a claim of real-time OCR or confirmed
compatibility with every game.

## Install and enable

1. Install the plugins from this fork's ncnn-enabled build. The default build
   includes ncnn; `-DVJO_WITH_NCNN=OFF` produces a Lens-only build.
2. On your computer, download the pinned, checksum-verified model:

   ```sh
   python3 tools/prepare_ocr.py --output ocr
   ```

3. Copy `ocr/PP_OCRv5_mobile_rec.ncnn.bin` to
   `ux0:data/VitaJPOverlay/ocr/`. Only this **8,242,276-byte** file is needed
   on the Vita. The graph and character dictionary are compiled into the
   plugin. Arbitrary replacement models are not supported.
4. Set these options in `ux0:data/VitaJPOverlay/config.ini`:

   ```ini
   ocr_backend = ncnn
   ocr_model_dir = ux0:data/VitaJPOverlay/ocr
   ocr_mode = on_press
   ```

5. In the game, open the overlay, press **Square**, draw a tight box around
   the dialogue text, then press **Cross** to save. Reopen the overlay to
   recognize it. An initial “select a region” message is expected until a
   region has been saved. Exclude portraits, speaker labels and frame borders.

For offline OCR **and** word lookup, also install a
[local dictionary](local-dictionaries.md) and set `dictionary = local`.
Hachidori/JPDB/Jiten and Anki retain their own network requirements. Keep
`anki_host` and `log_host` empty if those network features are not wanted.

The model is loaded only for a recognition request and released before
dictionary lookup. The ncnn backend always uses manual recognition, even
if an older config still says `ocr_mode = auto`. Subtitles recognize once
when enabled; toggle them off and on to refresh. There is no continuous
local subtitle refresh in this version.

## Suitable dialogue regions

The lightweight line finder is intended for **horizontal white/near-white
text on a dark dialogue box**, such as typical visual-novel dialogue.
It finds rows by projecting bright pixels; it is not a neural scene-text
detector. It supports up to eight lines in a region no larger than 960×544.
Each detected line must be at most 88 pixels tall before padding. Tiny
components under eight pixels high are ignored.

Mincho fonts, outlines, translucent boxes, portraits behind the text,
furigana, colored speaker names, and menu decorations can confuse the line
finder. Black text on white backgrounds, vertical text and arbitrary
full-screen layouts are unsupported. A blank or unsupported image may
produce no text; this does not guarantee there is no text on screen.
Try a tighter region or switch explicitly to `ocr_backend = lens`.

The same engine was smoke-tested on 13 dialogue-line crops from Clannad,
AIR, and Corpse Party: Blood Drive on a host CPU. This checks recognition,
not Vita game compatibility. Some characters and punctuation were wrong.
The checked-in integration test uses the repository's synthetic dialogue
image and tests multiline recognition, blank input, and invalid models.

## Memory and execution

- One CPU thread, ARM NEON on Vita, no Vulkan or OpenMP.
- FP32 calculations; fp16 arithmetic/storage/packing and BF16 are disabled.
  The downloaded compact weights are decoded as needed when loading.
- A private **64 MiB** memblock holds weights and tensor allocations for
  one job. Failure to reserve it produces an error, without cloud fallback.
  Small ncnn C++ metadata allocations use the existing Paf allocator;
  plugin code, metadata, capture buffers and result arenas are additional
  memory. The 64 MiB figure is not total process memory usage.
- Lines are resized to height 48 and width at most 1024. Very long lines
  are compressed horizontally to enforce the limit, which may hurt accuracy.
- The worker releases the memblock on every success/error path and checks
  cancellation between input/model reads and lines. A running neural layer
  is not interrupted halfway through; shutdown can wait for that layer.
- `log_file = on` records elapsed job time, line count and peak tensor-heap
  use. Host timings do not predict Vita latency. The tested host crops used
  approximately 32–43 MiB of the tensor workspace; ARM may differ.

## Build and test

Both CMake projects default to `VJO_WITH_NCNN=ON`. Configuration fetches ncnn
at a pinned commit; the build downloads the pinned graph, weights and
vocabulary and verifies their SHA-256 hashes. Model preparation needs Python
3; host integration tests also need Pillow. Nothing is downloaded by the
Vita plugin at runtime.

```sh
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host -j4
ctest --test-dir build/host --output-on-failure

# After tools/setup_toolchain.sh and exporting VITASDK:
cmake -S vita -B build/vita -DCMAKE_BUILD_TYPE=Release
cmake --build build/vita -j4
cmake --build build/vita --target release
```

For offline/repeated builds, supply an existing checkout using
`-DFETCHCONTENT_SOURCE_DIR_NCNN=/path/to/ncnn` at the pinned revision and
prepopulate `build/host/ocr-model/` or `build/vita/ocr-model/` with the three
verified assets listed in `tools/prepare_ocr.py`. The host `vjo-ocr` tool
accepts a model directory, width, height and a tightly packed RGBA8 file.
The JPEG-only `vjo-cli` OCR path refuses `ocr_backend = ncnn` instead of
uploading to Lens; use `vjo-ocr` to test recognition and `vjo-cli --text`
to test dictionary lookup separately.

The port keeps SceShell's existing `-nostdlib -nostartfiles` build. ncnn's
minimal STL/math implementations are used, global new/delete stay with
Paf, and numeric allocation is routed to the private workspace. Only the
model's operators and required helper operators are enabled. Shape
expressions are deliberately rejected: the pinned graph uses integer
reshape dimensions. The graph and vocabulary are compiled in, while the
external weights are size- and SHA-256-checked before parsing.

Source revisions, attribution and bundled license notices are recorded in
[third_party/README.md](../third_party/README.md).
