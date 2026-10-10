# vita-vn-ocr: local OCR on the Vita CPU

`ocr_backend = vocr` reads the selected dialogue region on the console with
[vita-vn-ocr](https://github.com/bee-san/vita-vn-ocr): a small Japanese CTC line
recognizer made for visual-novel dialogue, and its line finder, both plain C99
with no `malloc`. Nothing leaves the Vita: no image upload, no Lens connection
and no cloud fallback. With `dictionary = local` the whole lookup is offline.

**Status: experimental, not yet run on a PS Vita.** It builds with VitaSDK and
passes the host tests; everything below the "Host measurements" heading was
measured on a PC. On-console memory, speed and stability are what the
[checklist](#on-console-checklist) is for.

## What a recognition does

1. The kernel captures the selected region (single pass). The Shell reads each
   row once, converting it to grey (`(77 R + 150 G + 29 B) >> 8`, the luma both
   the finder and the recognizer use), so the kernel frees its capture buffer
   before recognition starts.
2. The polarity-agnostic line finder (`vocr_find_lines`, gradient/contrast
   projection) finds up to 16 lines. Unlike the PP-OCRv5 backend's bright-pixel
   finder it reads dark text on light boxes, outlined text and coloured names,
   and never rejects a region. A region with no line ends here: the weights
   file is not read.
3. The weights file is read from `ocr_model_dir`, its size and SHA-256 are
   checked against the pinned release asset, and the runtime validates it.
4. Each line is recognized (int8 weights, int32 sums, NEON); lines of dark
   text are inverted first, as the model was trained.
5. The workspace is freed before the dictionary lookup and before anything is
   shown. A failed or cancelled job shows no partial text.

All of a job's memory is one block sized exactly for its region:

```
weights + max(line finder arena, recognizer arena) + region width × height
+ the runtime's two structs (3,760 bytes on the Vita)
```

| model (`vocr_model`) | weights | recognizer arena | full 960×544 region | 880×144 dialogue box |
|---|---|---|---|---|
| `H15_w8.vocr` (default, recommended) | 1,155,968 B | 182,944 B | 1,864,912 B (1.78 MiB) | 1,469,392 B (1.40 MiB) |
| `FL10_w8.vocr` (smaller, less accurate) | 657,664 B | 124,832 B | 1,308,496 B (1.25 MiB) | 912,976 B (0.87 MiB) |
| `F20_w8.vocr` (larger) | 1,558,016 B | 241,056 B | 2,325,072 B (2.22 MiB) | 1,929,552 B (1.84 MiB) |

The line finder's arena (83,323 B at 960×544) is smaller than every
recognizer arena, so it costs nothing extra. The job first asks for a USER
memblock and, when SceShell has no free pages for it, falls back to ScePaf's
heap behind the same 1.5 MiB reserve guard the Lens workspace uses. If neither
succeeds the job reports "not enough memory"; the log records both attempts.

**Expect memory to be tight inside a game.** Physical tests on the
experimental branches found ScePaf's whole heap is 5 MiB with about 2.4 MiB
free while CLANNAD runs, and a 768 KiB USER memblock failed there with
`0x80024302` (no free physical page). After the Lens workspace and the 1.5 MiB
reserve, the H15 workspace probably does not fit beside that game; FL10 for a
tight dialogue box comes closest. The first on-console run answers this.

## Models

The weights are the int8 assets of the vita-vn-ocr
[v0.2.0 release](https://github.com/bee-san/vita-vn-ocr/releases) (draft at the
time of writing). On its hand-checked real screenshots (int8, C runtime, its
`eval/run.py`, with hand-drawn line boxes):

| model | vita-test CER / exact lines | psp-test CER / exact lines | est. Cortex-A9 time, 640-px line at 444 MHz |
|---|---|---|---|
| H15 | 1.56% / 83.5% | 2.63% / 82.4% | 166-302 ms |
| FL10 | 2.30% / 77.7% | 5.18% / 69.8% | 94-178 ms |
| F20 | 1.79% / 84.0% | 2.79% / 79.6% | 235-416 ms |
| for scale: JIS-trimmed int8 PP-OCRv5 mobile | 2.30% / 79.1% | 5.34% / 66.1% | |

The times are vita-vn-ocr's estimates from the Vita compiler's machine code
(`vita/a9_cycles.py`), not console measurements. The runtime itself reproduces
the PC bit for bit in Vita3K. Pretraining used NVIDIA
OCR-Synthetic-Multilingual-v1 (CC BY 4.0); see the release notes for the full
attribution. The weights are GPL-3.0-or-later, like this overlay.

## Build

The runtime is not copied into this repository. Check out vita-vn-ocr at the
pinned commit and point the build at it (the repository is private, so this
needs access to it):

```sh
git clone https://github.com/bee-san/vita-vn-ocr /path/to/vita-vn-ocr
git -C /path/to/vita-vn-ocr checkout 72cf4af0905091a0ee329d336889ccf0a8cd38c8

# Vita plugins and release zip (VitaJPOverlay-0.6-lens-vocr.zip)
cmake -S vita -B build/vita -DVJO_WITH_VOCR=ON -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr
cmake --build build/vita --target release

# Host tools and tests; VJO_VOCR_MODEL_DIR adds the real-model checks
cmake -S . -B build/host -G Ninja -DVJO_WITH_VOCR=ON -DVJO_VOCR_SOURCE_DIR=/path/to/vita-vn-ocr \
  -DVJO_VOCR_MODEL_DIR=$PWD/ocr
cmake --build build/host && ctest --test-dir build/host --output-on-failure
```

`cmake/VocrOCR.cmake` checks the four runtime files (`runtime/vocr.c`,
`vocr.h`, `vocr_lines.c`, `vocr_lines.h`) against their SHA-256 at that commit
and refuses others unless `-DVJO_VOCR_ALLOW_UNPINNED=ON`. Copying those files to
`third_party/vita-vn-ocr/runtime/` works too and needs no source directory.
Without `VJO_WITH_VOCR` the Shell is the Lens-only build; `ocr_backend = vocr`
then reports that the backend is missing instead of uploading anything.

## Install

1. Install the vocr build's plugins as in the [README](../README.md#installing).
2. Download and check the weights on a computer:

   ```sh
   python3 tools/prepare_ocr.py --backend vocr --output ocr          # H15_w8.vocr
   python3 tools/prepare_ocr.py --backend vocr --model FL10_w8.vocr --output ocr
   ```

   While vita-vn-ocr is private this uses the GitHub CLI (`gh auth login`
   with access to it). `--from FILE` checks a copy downloaded another way.
3. Copy the file to `ux0:data/VitaJPOverlay/ocr/`, or let the FTP installer do
   it: `tools/install_ftp.sh --vocr-model ocr/H15_w8.vocr --set ocr_backend=vocr VITA_IP`.
4. In `ux0:data/VitaJPOverlay/config.ini`:

   ```ini
   ocr_backend = vocr
   ocr_model_dir = ux0:data/VitaJPOverlay/ocr
   vocr_model = H15_w8.vocr
   ocr_mode = on_press
   log_file = on
   ```

   `ocr_mode = auto` also works: each settled dialogue screen is read in the
   background, which costs a model load and recognition per screen.
5. In the game, open the overlay, press □ and draw a box around the dialogue
   text (the whole text box is fine, name plate included), then × to keep it.

## Host measurements

`tools/benchmark_local_ocr.py` runs the host `vjo-ocr` tool (the same
finder, workspace and model checks as the Shell) once per region, in a fresh
process, on vita-vn-ocr's hand-checked regions: vita-test (84 regions from 16
Vita games, 960×544 screenshots) and psp-test (93 regions from 14 PSP games, at
the PSP's native 480×272). Accuracy is eval's region-level concat CER: all
lines a backend read, top to bottom, against all lines of the region; a refused
region counts all its characters as errors. The ncnn row is this repository's
own PP-OCRv5 mobile backend (`-DVJO_WITH_NCNN=ON`, the pinned 8,242,276-byte
model) with its bright-pixel line finder. Details:
[benchmarks/vocr-vs-ncnn-20261010.json](benchmarks/vocr-vs-ncnn-20261010.json).

| backend | set | regions refused | concat CER | workspace peak, max (median) | process peak RSS, max |
|---|---|---|---|---|---|
| ncnn PP-OCRv5 mobile | vita-test | 25 of 84 | 37.55% | 42.41 MiB (32.38) | 56.3 MiB |
| vocr H15 | vita-test | 0 | **6.78%** | **1.40 MiB (1.33)** | 6.1 MiB |
| vocr FL10 | vita-test | 0 | 8.66% | 0.87 MiB (0.80) | 5.5 MiB |
| ncnn PP-OCRv5 mobile | psp-test | 41 of 93 | 60.72% | 38.36 MiB (31.95) | 45.3 MiB |
| vocr H15 | psp-test | 0 | **14.70%** | **1.32 MiB (1.30)** | 5.7 MiB |
| vocr FL10 | psp-test | 0 | 17.65% | 0.79 MiB (0.77) | 5.1 MiB |

- The workspace peak is what the job allocates: ncnn's tensor heap inside its
  64 MiB block, vocr's whole exact workspace. The RSS is the tool process's
  own VmHWM and includes its 4.3 MiB baseline (code, libc, the RGBA input).
- The ncnn backend refuses regions with dark text on a light box, light boxes
  and very tall lines (`VJO_E_OCR_REGION`). The vocr finder refuses none.
- Most of the remaining vocr error is the line finder's: with hand-drawn line
  boxes the same model reads 1.56% / 2.63%. The finder found 95.2% of the Vita
  lines and 90.2% of the PSP lines (`eval/run.py --finder console --polarity
  console`, which gives the same 6.78% / 14.70% concat CER as the overlay).
- Host time per region (process start, model load and check, recognition) was
  62 ms for H15 and 204 ms for ncnn, median. This says nothing about the Vita.

## On-console checklist

For a PS Vita on firmware 3.60-3.74 with the vocr build installed, the model
copied, and the settings above. Close other apps first.

1. **Boot.** Reboot and start a VN (CLANNAD, PCSG00415, is the game the
   previous memory tests used). `status.txt` should end with
   `bootstrap: worker started`.
2. **Region.** Open the overlay (L+R), press □, draw a box around the dialogue
   and press ×.
3. **One recognition.** Reopen the overlay on a dialogue line. Note what it
   shows: the text, or an error message.
4. **Log.** Fetch `ux0:data/VitaJPOverlay/log.txt` (`tools/install_ftp.sh
   --status VITA_IP` prints the logs) and find, in order:
   - `vocr: USER memblock (N KiB) failed 0x...`, or nothing if it succeeded;
   - `vocr Paf allocation: free=... requested=... reserve=... success=...` or
     `vocr Paf reserve guard: ...` when the memblock failed;
   - `vocr rc=... H15_w8.vocr: WxH region, N lines (D dark), F frames,
     workspace B bytes (USER memblock | Paf heap | none), find X ms, load Y ms,
     recognize Z ms, total T ms`.
   Please post these lines, the shown text and a photo of the screen on
   vita-vn-ocr issue #6.
5. **If it says "not enough memory".** Set `vocr_model = FL10_w8.vocr` (copy
   that file too), draw the tightest box around the text, and retry. Report the
   `free=` figures either way: they decide whether the workspace needs a
   smaller reserve, smaller result arenas or the game-process worker the Meiki
   branch uses.
6. **Speed.** With `rc=0`, repeat on five lines and note `load` and
   `recognize` ms. The estimate for H15 is 166-302 ms per 640-px line at
   444 MHz and about 1.33× that at the default 333 MHz.
7. **Polarity.** Try one game with dark text on a light box (for example
   VARIABLE BARRICADE or 偽りの仮面) and one with outlined text.
8. **Stability.** Leave `ocr_mode = auto` with subtitles on (Select+R) for ten
   minutes of reading. Watch for game slowdown, audio stutter, or a SceShell
   crash (`tools/install_ftp.sh --dumps VITA_IP` fetches crash dumps).
9. **Back to Lens.** Set `ocr_backend = lens`: nothing else changes.

## Limits

- Horizontal text only; vertical text is not read.
- Adjacent lines 1-2 px apart can merge on PSP-sized text, and decorated
  frames can add a stray line; tight regions read best.
- Recognition runs on the Shell's network thread, on whichever core SceShell
  runs, at the game's clock. The overlay does not change the CPU clock.
- The pinned table accepts only the three v0.2.0 int8 files; a new release
  needs its sizes and hashes added (`core/vocr_ocr.c`, `tools/prepare_ocr.py`).
