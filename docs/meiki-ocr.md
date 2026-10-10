# Experimental Meiki OCR

Meiki is optional local OCR for selected horizontal dialogue.
Select `ocr_backend = meiki` explicitly. It runs only on request, including
when an older config says `ocr_mode = auto`, and errors never upload an image
to Lens. Lens remains the default; the existing ncnn backend remains available.

Set `text_source = ocr` to use Meiki directly. Tap Square, select one horizontal
line, and save it with Cross; then reopen with L+R to recognize it. Keep a narrow
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
`ux0:data/VitaJPOverlay/meiki/`. The selected worker checks its size and SHA-256 before MNN
parses it. Dialogue-box mode also requires the 4,119,220-byte
`meiki-detect-int8.mnn` in that directory, checked against its own SHA-256.
Other model files are rejected before the inference buffers are allocated.

```ini
text_source = ocr
ocr_backend = meiki
ocr_model_dir = ux0:data/VitaJPOverlay/meiki
ocr_mode = on_press
meiki_layout = single_line
```

For an OCR-only test, enable the Kernel and Shell plugins and leave
`VitaJPOverlay_Text.suprx` registrations disabled. The Text plugin is optional
and unnecessary for this configuration.

For local OCR and dictionary lookup, configure `dictionary = local` and
install a [local dictionary](local-dictionaries.md). Other dictionaries and
Anki retain their own network requirements.

## Memory and validation boundary

The original Shell execution path borrows a 16 MiB neural workspace, a 4 MiB private newlib/C++ heap,
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
fragmentation are additional. The raw capture uses multiple passes and
retains its selected-region buffer until the next capture or game exit; JPEG
capture retains its separate single-pass release policy. Allocation failure
returns an OCR error.
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
memory, latency or stability beside a game on a physical Vita. Test this optional overlay build manually; emulator free-memory counters do
not establish the physical game budget.

A physical in-game test on 2026-10-10 reported 2,498 KiB free from Paf and
refused the 768 KiB result allocation before capture or Meiki started. The
result guard alone requires 2,816 KiB including its reserve. The complete
single-line path currently requires 24,237,184 bytes (23.114 MiB) of free Paf
space, plus module code, stacks and capture allocations outside that total.
Reducing only the result buffers cannot close that gap. The USER counter
reported a negative value; that value is not a usable free-memory budget.

The subsequent physical diagnostic established that Paf's whole mapped heap
is 5,242,880 bytes (5 MiB), with roughly 2.4 MiB free. The recognizer model file
alone exceeds that entire heap by 149,976 bytes. Virtual/direct free readings
and `QueryInfo` agreed; a guarded 128 KiB allocation was mapped inside the
heap and released. A 4 KiB owned USER allocation succeeded despite the negative
USER counter. The PHYCONT probe returned `0x80024A00` (address-space error),
and CDRAM returned `0x80024309` (no free CDRAM physical page). Neither alternate
pool passed its small probe, so no full workspace was allocated there.

## Optional game-process OCR worker

Build with `-DVJO_MEIKI_GAME_WORKER=ON` in addition to `VJO_WITH_MEIKI=ON` to
move preprocessing and the isolated engine into `VitaJPOverlay_OCR.suprx`.
Keep `VJO_MEMORY_DIAGNOSTICS=OFF` for this build. Install the matching API 9
Kernel and Shell plugins, the frozen engine, and the OCR worker. Enable the
OCR worker only under each game title you intend to test, for example:

```ini
*PCSG00415
ur0:tai/VitaJPOverlay_OCR.suprx
```

Leave Text plugin registrations disabled. This worker installs no import hooks,
reads no game text or executable addresses, and receives only the selected raw
capture through bounded kernel copies. Do not register it under `*ALL`, `*main`
or a system application. Kernel, Shell and worker must have matching API versions;
unresolved weak imports refuse startup before executing them.

The game worker creates one 64 KiB SDK thread and allocates its three owned,
page-aligned USER blocks only on request. Single-line preprocessing requires
385,024 bytes after page rounding; dialogue-box mode requires 753,664 bytes.
Alongside the 16 MiB neural and 4 MiB metadata blocks, preflight requires an
additional 2 MiB allowance for the engine module and 4 MiB reserve for the game.
It requires a successful, positive USER free-memory report and refuses negative,
implausible or insufficient budgets. Every block is checked for its exact mapping,
normal cached USER type, read/write access and 64-byte alignment. It neither
borrows game allocations nor changes memory quotas.

Shell retains the UI and local dictionary lookup, using two 160 KiB result arenas
instead of two 384 KiB arenas for this optional build. Model/session shutdown,
engine unload and successful owned-block release precede result publication.
Failed cleanup keeps allocation identity and refuses further work or unload.
Allocation failure publishes no partial OCR text and uses no Lens or Text fallback.

The kernel ties each request to its game PID, foreground epoch and exact capture
sequence. It pins the raw buffer while queued or claimed, including after a Shell
timeout, game suspension or Shell restart. A successful drained completion or
actual game process exit releases the claim. Closing the overlay or a 60-second
Shell timeout signals cancellation; it cannot free a buffer still read by the
worker. Request sequences are not reused during the kernel module's lifetime.
The copied UTF-8 result is capped at 4,096 bytes including its terminator.

This architecture still requires physical validation beside the selected game.
Host and ARM emulator tests exercise ownership, transport, preprocessing and the
frozen engine; they cannot prove physical allocation success or game stability.
The installed 81,117,034-byte JMdict passes 12 invented-text cases three times
through the production filter and dictionary pipeline in a 160 KiB host arena,
with the 4,120-byte OCR result retained. Peak host arena use is 126,312 bytes,
leaving 37,528 bytes, including a maximum-size successful OCR payload, the
64-token boundary and recovery after a bounded overflow. This is a host ABI
measurement, not a physical Paf allocation result.

The actual ARM worker completes two start/stop cycles and twelve requests in
isolated Vita3K: eight matching UTF-8 results, three budget refusals with zero
allocations, and one cancellation. Twenty prepared inference tensors match
their pinned references; ten cold-load raw detector/recognizer outputs repeat.
Nine engine loads/unloads, 27 released owned blocks, completion retry and both
worker thread deletions are verified. This harness uses a fake API 9 transport
and private compatibility adapters for Vita3K's missing byte-search, formatting
and mapping-query behavior. It does not exercise the physical kernel transport,
prove physical USER budgets, or bypass any production allocation guard.

`ux0:data/VitaJPOverlay/ocr-worker.log` records raw budgets, allocation identities,
peaks and cleanup status without dialogue text. Return to VitaShell FTP after a
manual trigger to retrieve that log and the Shell log.

The first physical game-worker test allocated Shell's 320 KiB result block,
passed the game budget guard, and returned a model-validation error before any
neural or metadata allocation. Both installed model files were then downloaded
over FTP and matched their pinned sizes and SHA-256 values. This establishes a
different failure from the earlier Shell heap refusal; it does not establish
that a retail game process can read those files or load the engine.

Game-worker file sizing uses read-only Open plus Lseek, and model reads loop to
complete bounded short reads. Native Open/Lseek/Read failures return
`VJO_E_OCR_MODEL_IO` separately from successfully opened size/hash mismatches.
The result carries a strict 28-character metadata tag, preserving the API 9
wire size and containing only model identity, failed operation and raw code.
On such a failure, a one-byte read of `app0:/sce_sys/param.sfo` tests ordinary
game-file access in that same process. Shell validates this tag, logs both
outcomes, and shows a file-access error instead of requesting a model reinstall.
The game process may be unable to write its own log; this diagnostic reaches
the existing Shell log without that dependency. Cancellation or failed cleanup
suppresses the tag, and every other failed result still clears its text.

For physical diagnosis, optionally build with `-DVJO_MEMORY_DIAGNOSTICS=ON`.
The first explicit pure-Meiki request compares virtual and direct Paf free
counts with `QueryInfo` and records the heap range. A 128 KiB Paf test is
allowed only if all those readings support its reserve and margin.
Separately, it probes owned USER, PHYCONT and CDRAM blocks, verifies their
mapping and 128 bytes of scalar writes, and frees each block immediately.
Only a successful small pool test and a positive free counter with 2 MiB
headroom permit an engine-sized test. That test excludes result buffers,
module code and thread stacks; CPU scalar access does not validate MNN or
SIMD execution. No workspace is relocated by this diagnostic build.

Diagnostics run before capture and OCR, with the optional Anki worker
disabled in this build. Failed mapping checks prevent writes and larger
probes in that pool. Canary or cleanup failures stop the request;
cleanup retains ownership and prevents Shell unload until release succeeds.
The option defaults to off. Keep `log_file = on`, reboot after installation,
launch the game manually, and press the configured overlay trigger once.
Return to VitaShell FTP to retrieve `ux0:data/VitaJPOverlay/log.txt`.

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

Set `-DVJO_MEIKI_GAME_WORKER=ON` in the host build to also test Shell dispatch
and its reduced arenas. Set `VJO_MEIKI_TEST_MODEL`,
`VJO_MEIKI_TEST_DETECT_MODEL` and `VJO_JMDICT_FILE` to the corresponding private
fixtures for full model-validation and dictionary checks. The optional dictionary
integration test explicitly skips when its fixture is absent.
