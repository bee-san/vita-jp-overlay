# Host build: portable core, BearSSL, the vjo-cli tool and unit tests.
# The Vita build lives in vita/CMakeLists.txt.
cmake_minimum_required(VERSION 3.16)
project(vita_jp_overlay_host C CXX)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" ON)
option(VJO_WITH_MEIKI "Build the portable Meiki region adapter" OFF)
set(VJO_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

file(GLOB_RECURSE BEARSSL_SRC ${CMAKE_SOURCE_DIR}/third_party/bearssl/src/*.c)
add_library(bearssl STATIC ${BEARSSL_SRC})
target_include_directories(bearssl PUBLIC third_party/bearssl/inc PRIVATE third_party/bearssl/src)
target_compile_options(bearssl PRIVATE -w)

set(CORE_SRC
  core/arena.c core/pb.c core/utf.c core/lens.c core/json.c core/jpdb.c
  core/textfilter.c core/entries.c core/jiten.c core/config.c core/regions.c
  core/http.c core/tls.c core/tls_roots.c core/client.c core/styled.c core/jpegsw.c
  core/base64.c core/anki.c core/anki_queue.c core/net.c core/hachidori.c core/local_dict.c core/ocr_lines.c
  core/text_source.c core/text_match.c core/text_scan.c core/hook_profile.c)
add_library(vjo_core STATIC ${CORE_SRC})
target_include_directories(vjo_core PUBLIC core include)
target_link_libraries(vjo_core PUBLIC bearssl)
target_compile_definitions(vjo_core PUBLIC VJO_HOST=1)
target_compile_options(vjo_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
if(VJO_WITH_NCNN)
  include(cmake/LocalOCR.cmake)
  target_compile_definitions(vjo_core PUBLIC VJO_WITH_NCNN=1)
  target_link_libraries(vjo_ocr PUBLIC vjo_core)
  add_executable(vjo-ocr host/ocr_tool.c)
  target_link_libraries(vjo-ocr vjo_ocr vjo_host)
endif()

add_library(vjo_host STATIC host/net_posix.c host/render.c host/replay.c)
target_include_directories(vjo_host PUBLIC host)
target_link_libraries(vjo_host PUBLIC vjo_core)
target_compile_options(vjo_host PRIVATE -Wall -Wextra)

add_executable(vjo-cli host/cli.c)
target_link_libraries(vjo-cli vjo_host)

# Raw RGBA -> JPEG with the software encoder (tools/test_jpegsw.py).
add_executable(vjo-jpegsw host/jpegsw_tool.c)
target_link_libraries(vjo-jpegsw vjo_host)

enable_testing()
add_executable(vjo-text-tests host/tests/test_text_source.c text/tests/cases.c)
target_link_libraries(vjo-text-tests vjo_core)
target_compile_options(vjo-text-tests PRIVATE -Wall -Wextra)
add_test(NAME text_sources COMMAND vjo-text-tests)
add_executable(vjo-kernel-text-tests host/tests/test_kernel_text.c)
target_include_directories(vjo-kernel-text-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-kernel-text-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-kernel-text-tests vjo_core)
add_test(NAME kernel_text COMMAND vjo-kernel-text-tests)
if(VJO_WITH_MEIKI)
  include(cmake/MeikiOCR.cmake)
  add_executable(vjo-meiki-tests host/tests/test_meiki_ocr.c)
  target_include_directories(vjo-meiki-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-meiki-tests vjo_meiki)
  add_test(NAME meiki_adapter COMMAND vjo-meiki-tests)
  add_executable(vjo-meiki-bridge-tests host/tests/test_meiki_bridge.c)
  target_include_directories(vjo-meiki-bridge-tests PRIVATE third_party/acutest/include
    host/tests/vita_stubs third_party/bearssl/inc ${VJO_MEIKI_SOURCE_DIR})
  target_link_libraries(vjo-meiki-bridge-tests bearssl)
  target_compile_definitions(vjo-meiki-bridge-tests PRIVATE VJO_HOST=1)
  add_test(NAME meiki_bridge COMMAND vjo-meiki-bridge-tests)
  set(MNN_SOURCE "" CACHE PATH "Pinned, patched MNN source")
  set(MNN_LIBRARY "" CACHE FILEPATH "Exception-enabled host MNN static library")
  if(MNN_SOURCE AND MNN_LIBRARY)
    add_executable(vjo-meiki-ocr host/meiki_ocr_tool.c ${VJO_MEIKI_SOURCE_DIR}/runtime.cpp
      ${VJO_MEIKI_SOURCE_DIR}/detect_runtime.cpp
      ${VJO_MEIKI_SOURCE_DIR}/arena.c)
    target_include_directories(vjo-meiki-ocr PRIVATE ${MNN_SOURCE}/include)
    target_compile_features(vjo-meiki-ocr PRIVATE cxx_std_17)
    target_compile_options(vjo-meiki-ocr PRIVATE -ffp-contract=off -Wall -Wextra)
    target_link_options(vjo-meiki-ocr PRIVATE -Wl,--wrap=MNNMemoryAllocAlign
      -Wl,--wrap=MNNMemoryCallocAlign -Wl,--wrap=MNNMemoryFreeAlign)
    target_link_libraries(vjo-meiki-ocr vjo_meiki ${MNN_LIBRARY} pthread dl)
  endif()
endif()
# kernel/foreground.c, kernel/triggers.c: kernel state machines (no psp2kern).
add_executable(vjo-tests host/tests/test_core.c kernel/foreground.c kernel/triggers.c)
target_include_directories(vjo-tests PRIVATE third_party/acutest/include kernel)
target_link_libraries(vjo-tests vjo_host)
target_compile_definitions(vjo-tests PRIVATE VJO_FIXTURES="${CMAKE_SOURCE_DIR}/host/fixtures")
add_test(NAME core COMMAND vjo-tests)
add_executable(vjo-anki-tests host/tests/test_anki.c)
target_include_directories(vjo-anki-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-anki-tests vjo_host)
add_test(NAME anki COMMAND vjo-anki-tests)
add_executable(vjo-hachidori-tests host/tests/test_hachidori.c)
target_include_directories(vjo-hachidori-tests PRIVATE third_party/acutest/include)
target_link_libraries(vjo-hachidori-tests vjo_host)
add_test(NAME hachidori COMMAND vjo-hachidori-tests)
# Real shell control policy, with host-only substitutes for Vita SDK I/O.
add_executable(vjo-shell-worker-tests host/tests/test_shell_worker.c)
target_include_directories(vjo-shell-worker-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-worker-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-shell-worker-tests vjo_host)
if(VJO_WITH_NCNN)
  target_link_libraries(vjo-shell-worker-tests vjo_ocr)
endif()
add_test(NAME shell_worker COMMAND vjo-shell-worker-tests)
# Same control code with the SceShell/Paf result allocator used by the Vita build.
add_executable(vjo-shell-worker-paf-tests host/tests/test_shell_worker.c)
target_include_directories(vjo-shell-worker-paf-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-shell-worker-paf-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_compile_definitions(vjo-shell-worker-paf-tests PRIVATE VJO_PAF_ALLOC=1)
target_link_libraries(vjo-shell-worker-paf-tests vjo_host)
if(VJO_WITH_MEIKI)
  target_compile_definitions(vjo-shell-worker-paf-tests PRIVATE VJO_WITH_MEIKI=1)
  target_link_libraries(vjo-shell-worker-paf-tests vjo_meiki)
endif()
if(VJO_WITH_NCNN)
  target_link_libraries(vjo-shell-worker-paf-tests vjo_ocr)
endif()
add_test(NAME shell_worker_paf COMMAND vjo-shell-worker-paf-tests)
add_executable(vjo-anki-queue-tests host/tests/test_anki_queue.c)
target_include_directories(vjo-anki-queue-tests PRIVATE third_party/acutest/include host/tests/vita_stubs)
target_compile_options(vjo-anki-queue-tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(vjo-anki-queue-tests vjo_host)
add_test(NAME anki_queue COMMAND vjo-anki-queue-tests)

# End-to-end ZIP conversion and disk lookup, using the actual CLI/runtime.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME cp932_table COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/generate_cp932.py --check)
if(VJO_WITH_NCNN)
  add_executable(vjo-ocr-tests host/tests/test_ocr.c)
  target_include_directories(vjo-ocr-tests PRIVATE third_party/acutest/include)
  target_link_libraries(vjo-ocr-tests vjo_ocr)
  add_test(NAME local_ocr_unit COMMAND vjo-ocr-tests)
  add_test(NAME local_ocr_model COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_local_ocr.py
    --cli $<TARGET_FILE:vjo-ocr> --model ${VJO_OCR_ASSETS})
endif()
# Each action is a fresh process; the Anki HTTP double survives client restarts.
add_test(NAME anki_restart COMMAND ${Python3_EXECUTABLE}
  ${CMAKE_SOURCE_DIR}/host/tests/test_anki_restart.py $<TARGET_FILE:vjo-anki-queue-tests>)
add_test(NAME local_dictionary COMMAND ${CMAKE_COMMAND} -E env
  "VJO_CLI=$<TARGET_FILE:vjo-cli>" "VJO_BENCH=$<TARGET_FILE:vjo-local-bench>" ${Python3_EXECUTABLE} -m unittest discover
  -s ${CMAKE_SOURCE_DIR}/tools/tests -v)

# Counts production file reads and times lookup only (no OCR or process startup).
add_executable(vjo-local-bench host/local_bench.c)
target_link_libraries(vjo-local-bench vjo_host)
