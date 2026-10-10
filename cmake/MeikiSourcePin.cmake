# Evaluated source pin. Archive checkouts work without .git metadata.
set(VJO_MEIKI_SOURCE_REVISION "2eaa0592a96583cd73fac7e21374cfc63dc4009e")
set(_meiki_source_pins
  "arena.c|30f3b7b6c95dd2310959ed4bfdc7f80240368b31e1572d9142b71d568561d1d1"
  "arena.h|510699fbd3778cfdb9de33b6280f6f1d415be2499ba63c121204979c0c2f0e7b"
  "decode.c|7a2e654b207b260306982a6e266ac0240cc38c20b437a370320aef6ef5e54a79"
  "decode.h|27ad16a639f5d0f77db1f0eb4fe86a4a2272c9987eb5e4faf44ff54d8ffc204c"
  "runtime.cpp|04f1d7aa1e763bbbe88ec9e9168f8c2d4eae7cce30668183731c0a1ac977eec2"
  "runtime.h|0a37260d0e33b3c2f7ca098d89f8a6a6f1a2b37fa1b17983e6d4edfb36aadcbb"
  "preprocess.c|02793943ccfcaf374eb4484e07fc8e892215a947b2654748a06b5007935e1c53"
  "preprocess.h|88f772d141a4c7db4c8622578fa2b28642114fd8161e2b16c59b32007a82be50"
  "detect_runtime.cpp|1278b79db2e7e84061ff9d0fa5c0835a2b5b4190cfb86a0510e5fe6f64319e3c"
  "detect_runtime.h|67188a2fb1dd2db82bc968420be52b483c6fd58aec42b48c3689cfddbbc65210"
  "detect_preprocess.c|694871a751bf63a5f11fe4234efaeac760131f996c8fca06f1b2db96289eae02"
  "detect_preprocess.h|f6a8899c2df44ee4e2479fd26549f7ca0c7b1c6268e11f477793e301efcb3f47"
  "module/CMakeLists.txt|9ad7bc0d774f7dca6674119ce5dc377e972b2f691332f41883d0ada0e17b6770"
  "module/api.h|c5fd4885502e3d44dd27925e367d85a8c91d231a53d06332d8ca60280db0ff2b"
  "module/bridge.c|b8e1b610bc6814c8810060e98eb34a1f9a26f6b3083f8ab81d2cbe29c328d208"
  "module/metadata.c|5d015b078996a3c0e11f13f0531bcea04da642089364777055c14537809e9a00"
  "module/engine.cpp|f504f8d79bfb2bf7e02ab2e7804351d67caf4f26533d859ff7e4940b711a98c1"
  "module/exports.yml|9c4b3ff5388eefea166a94988ead82c50c9cae335ca1a75dbe9a51ea594c51f4"
  "module/internal.h|ce6c66f00ef23504348ecaabd32210c1365d389cbc35236f67eb37093fef2e36"
)
foreach(_pin IN LISTS _meiki_source_pins)
  string(REPLACE "|" ";" _parts "${_pin}")
  list(GET _parts 0 _file)
  list(GET _parts 1 _expected)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${VJO_MEIKI_SOURCE_DIR}/${_file}")
  if(NOT EXISTS "${VJO_MEIKI_SOURCE_DIR}/${_file}")
    message(FATAL_ERROR "Missing pinned Meiki source: ${_file}")
  endif()
  file(SHA256 "${VJO_MEIKI_SOURCE_DIR}/${_file}" _actual)
  if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "Meiki source differs: ${_file}. Use vita-vn-ocr ${VJO_MEIKI_SOURCE_REVISION}")
  endif()
endforeach()
