Vendored sources:
- bearssl  https://www.bearssl.org/git/BearSSL  7bea48e5e850ab4cafbe68d3765cdaba13a86d6f (MIT)
- jsmn     https://github.com/zserge/jsmn       25647e692c7906b96ffd2b05ca54c097948e879c (MIT)
- acutest  https://github.com/mity/acutest      31751b4089c93b46a9fd8a8183a695f772de66de (MIT)

Fetched for the optional/default-enabled local OCR build:

- ncnn: https://github.com/Tencent/ncnn at
  `3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82` (BSD-3-Clause plus bundled notices).
  The PP-OCRv5 character dictionary comes from the same revision's
  `examples/ppocrv5_dict.h` (Copyright 2025 Tencent, BSD-3-Clause).
  Full notices: [licenses/ncnn.txt](licenses/ncnn.txt).
- Converted PP-OCRv5 mobile recognizer graph/weights:
  https://github.com/nihui/ncnn-android-ppocrv5 at
  `671ac4a72299a86ddee160131ba88fed748df425`, under `app/src/main/assets/`.
  Model conversion credited to nihui; no Android application code is included.
- PP-OCRv5 model originates in https://github.com/PaddlePaddle/PaddleOCR
  (Apache-2.0): [licenses/PaddleOCR.txt](licenses/PaddleOCR.txt).

`tools/prepare_ocr.py` records expected asset sizes and SHA-256 hashes. The
model weights are downloaded, not committed. The generated graph/vocabulary
header is a build artifact. This port changes ncnn's build configuration,
replaces its global-allocation source with placement operators only, and
replaces shape-expression evaluation with a rejecting implementation; it
does not modify the checked-out upstream sources.
