# Native Vita text sources

`text_source = auto` uses OCR once to choose a source of Japanese text inside
the current Vita game. A selected source feeds the existing overlay and
dictionaries directly, without OCR, screen coordinates, capture buffers or
framebuffer checksum polling. `text_source = hooks` opens the manual picker
without making an OCR request. `text_source = ocr` keeps the existing OCR flow.

This is an experimental native taiHEN implementation. LunaHook/Textractor's
Windows injection and emulator JIT code is not a Vita binary. This implementation
uses bounded memory discovery, common string imports and optional signed
ARM/Thumb register profiles. Custom engine encodings, ruby/control-code filters
and per-glyph accumulation need game-specific work. No commercial VN or physical
Vita compatibility is claimed by the synthetic self-test.

## Install

Install the matching kernel and shell plugins from the same build (API 8), and
copy `ur0:tai/VitaJPOverlay_Text.suprx` from its ZIP. Register the text plugin
under each native game's title ID in the active taiHEN `config.txt`:

```text
*PCSG00001
ur0:tai/VitaJPOverlay_Text.suprx
```

Replace the example title ID with your game's actual ID. Keep other plugins
in that section. The FTP installer can add entries without duplicates:

```sh
tools/install_ftp.sh --text-title PCSG00001 --text-title PCSG00002 VITA_IP
```

Reboot after installing plugins. Settings are reread when the overlay opens.
No additional plugin is needed for memory discovery alone, but import and
register hooks require the text plugin in the game process. Use per-title
entries so an untested interceptor does not load into every app.

## Choose a source

1. Show a dialogue line, then open the overlay with L+R. Automatic mode makes
   one OCR attempt and scans normal cached main-memory allocations in bounded
   chunks. If OCR fails, the manual picker remains available. Discovery may
   take time; the picker updates while it runs.
2. A unique best Japanese match of at least **80%** is selected automatically.
   Similarity is Unicode Levenshtein distance divided by the longer normalized
   string's length. Whitespace and punctuation are ignored; fullwidth ASCII is
   folded. At least four normalized characters are required. Equal best scores
   require a manual choice.
3. The picker shows at most five candidates. Japanese text ranks first, followed
   by OCR score, observed changes, Japanese fraction, length and stable ID.
   Use ▲/▼ and × to choose. A **hook** observes a call site; a **pointer** follows
   a changing string pointer; a **buffer** rereads the same address. A static
   script buffer can match perfectly yet never advance: choose a hook or pointer
   if that happens.
4. Advance dialogue to confirm that the selected source changes correctly.
   The overlay waits 300 ms after text changes before starting a new lookup.
   Select+□ reopens the picker. □ rescans; △ explicitly retries OCR in automatic
   mode. Select+□ inside the picker chooses an OCR calibration region.

Selections are scoped to a foreground process session and cleared on game
switch, suspend/resume or invalid allocation identity. A blank selected buffer
waits for text; a freed buffer returns to manual discovery. Losing a source
clears the old OCR scores and does not automatically make another OCR request.
Hooks mode does not upload screen crops. Dictionary and Anki network settings
still apply, and optional Anki screenshots can request a capture independently.

## Native register profiles

Place `TITLE_ID.vjhook` in `ux0:data/VitaJPOverlay/hooks/`. Profiles require the
**actual native module NID**, ELF segment offset, instruction mode and instruction
bytes for that exact game revision. A Luna emulator address alone is insufficient:
its guest-address normalization changed across emulator releases. Validate it
against a locally obtained decrypted executable and native module information.

Format (all numbers hexadecimal):

```text
VJOHOOK1 TITLE_ID MODULE_NID
segment offset thumb register encoding dereferences signed_padding instruction_signature
```

Up to eight sites are supported. `thumb` is 0/1, registers are r0–r12, SP (D),
LR (E), encodings are UTF-8 (1), UTF-16LE (2), CP932 (3), and dereferences are
0–4. Padding is applied after dereferencing. The signature is 8–16 bytes in
hex, covering at least 12 bytes for a Thumb site two bytes off word alignment.
The plugin refuses wrong title/module NIDs, out-of-range sites and mismatched
signatures. Do not hook inside a Thumb IT block or where branches enter the
instructions taiHEN replaces. Its relocator must support the selected site.

Simple register/string descriptors from
[Luna's pinned Vita table](https://github.com/HIllya51/LunaTranslator/blob/b0c0c2d2a03b330c388b42d7e8f0556b6d230980/src/NativeImpl/LunaHook/LunaHook/emulators/vita3k_1.cpp)
can be adapted this way. Engine-specific extraction/filter callbacks and
`USING_CHAR` descriptors are not automatically ported. Do not distribute game
executables, dialogue dumps, fonts or screenshots in this repository.

## Bounds and validation

The shared protocol has no framebuffer coordinates or platform-specific pointer
types. Vita mapping and taiHEN interception are separate from decoding, ranking
and matching, leaving an extension point for a future PSP/Adrenaline backend.
Adrenaline continues to use OCR in automatic mode; native hook mode is not
implemented for it.

There are 24 retained sources, five displayed choices, 1,024 UTF-8 output bytes
and 256 decoded code points per source. The CP932 table is generated from
Python's standard codec and checked reproducibly. Memory reads map the target
PID, retain the mapping while copying and verify allocation identity. Discovery
per worker tick is bounded to four 4 KiB pages or 64 hole probes; selection stops
the full scan. A busy collector drops a hook observation instead of blocking a
game thread. Actual capture overhead, scan duration, peak memory and text
accuracy on real games still require console measurements.

Build and run the real ARM observer and compiled game plugin in isolated Vita3K:

```sh
cmake -S vita -B build/vita -DVJO_TEXT_TEST_APP=ON
cmake --build build/vita -j4
python3 tools/test_vita3k_hooks.py --vpk build/vita/vjo-hook-test.vpk
```

The VPK exports a **test transport**, loads the production `.suprx`, exercises
all eight import hooks and signed ARM/Thumb profiles, rejects a bad signature,
checks original return values and unloads the hooks. Separate assembly probes
check r0–r12, SP/LR, APSR/GE, FPSCR and all 32 VFP registers, including a Thumb
site with a four-byte-aligned stack. Host tests use the production kernel mapping
and shell worker code with SDK substitutes to cover permissions, stale sessions,
allocation replacement, safe buffer release and OCR bypass.

This does not validate SceShell/Paf rendering, firmware kernel mappings, commercial
engines or real-screenshot CER. Recorded measurements are in
[the validation record](benchmarks/native-text.json); unmeasured fields are null.

## Boot recovery and kernel build checks

If the Vita shuts down during boot after an update, hold L throughout power-on
to bypass taiHEN plugins, then open VitaShell FTP for recovery. The existing
plugins and tai configuration should be backed up before any update.

Kernel code must not use FPU/NEON registers. Starting with `0.6-hooks.2`, the
kernel is compiled with `-mgeneral-regs-only`, and the build checks its complete
disassembly before packaging. The early shell bootstrap and status logger also
use general registers only. Startup stages are recorded in `status.txt` so a
failure can be located before the shell worker starts. The original build had
85 FPU/NEON instructions; the corrected build has zero. See the
[boot investigation record](benchmarks/kernel-startup-20261010.json).

The FPU correction alone did not fix boot on firmware 3.65. `0.6-hooks.3`
also replaces a firmware-specific `SceSysmemForKernel` import with the stable
`SceSysmemForDriver` memory-info API. The build checks the packaged imports and
relocations and rejects the incompatible library. Both user plugins inspect
their kernel import stubs before making the first call: a failed kernel start
now leaves the overlay unavailable instead of executing the loader's unresolved
call trap. Native text submission reuses its locked scratch buffer to fit the
kernel's 4 KiB syscall stack without increasing resident memory.
Normal boot and all three plugin starts were verified on the physical 3.65 Vita;
CLANNAD then exposed a separate overlay-allocation failure. See the
[firmware fix measurements](benchmarks/native-hooks-fw365-20261010.json) for the
installed hashes, validation commands, and remaining game-capture limitation.

`0.6-hooks.4` opens the native picker and publishes selected raw text/subtitles
without allocating the two large result arenas. Dictionary lookup allocates
lazily; a selected native source can use two bounded 128 KiB Paf arenas if the
768 KiB memblock fails. Allocation failure preserves the text and backs off
before retrying. The picker now opens in physical CLANNAD without the previous
memory error, but dialogue discovery still returns no candidates. This is not
a verified CLANNAD text hook. See the [memory measurements](benchmarks/native-text-low-memory-20261010.json).

`0.6-hooks.5` adds retail `SceLibc` imports for `memcpy`, `strcpy`, `strncpy`
and `memmove` alongside the existing `SceLibKernel` clib hooks. An offline
inspection of CLANNAD PCSG00415 found all four retail imports and none of the
four original clib imports. Running the unchanged production text plugin in
isolated Vita3K then captured two different visible narration pages through
the same retail `memcpy` call site, without a CLANNAD-specific profile.
`strcpy` results use a
kernel-bounded, NUL-terminated UTF-8/CP932 read rather than dereferencing the
result in the game plugin. Zero-length memory copies remain ignored.

The ARM self-test explicitly imports both libraries (avoiding newlib's local
implementations), exercises each interceptor, checks return values and an
overlapping `memmove`, and checks that unloading restores all four retail
imports. It passed 255 checks in isolated Vita3K. The real-game experiment used
an emulator-only user-space transport with the production decoder; it does not
validate kernel page mapping or the physical overlay. See the
[retail-hook measurements](benchmarks/native-libc-hooks-20261010.json).
Physical CLANNAD capture and hardware performance remain unverified.
