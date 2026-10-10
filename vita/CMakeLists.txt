# Vita build: kernel plugin (.skprx), SceShell plugin (.suprx), RCO and the
# release zip. Configure with:
#   cmake -S vita -B build/vita -DCMAKE_TOOLCHAIN_FILE=$VITASDK/share/vita.toolchain.cmake
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaJPOverlay C CXX ASM)
# taihen's TAI_CONTINUE relies on K&R-style `()` casts, which C23 (GCC 15 default) rejects.
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(VJO_ELF_FLAGS "${VITA_ELF_CREATE_FLAGS}")
set(VJO_SELF_FLAGS "${VITA_MAKE_FSELF_FLAGS}")

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(VJO_ROOT ${ROOT})
set(VJO_BUILD_VITA ON)
option(VJO_WITH_NCNN "Build the CPU-only local mobile OCR recognizer" OFF)
option(VJO_WITH_MEIKI "Build the isolated, bounded Meiki OCR engine" OFF)
option(VJO_MEMORY_DIAGNOSTICS "Enable bounded, idle-only physical memory probes" OFF)
option(VJO_MEIKI_GAME_WORKER "Run Meiki in an optional OCR-only game-process plugin" OFF)
if(VJO_MEIKI_GAME_WORKER AND NOT VJO_WITH_MEIKI)
  message(FATAL_ERROR "VJO_MEIKI_GAME_WORKER requires VJO_WITH_MEIKI")
endif()
set(VJO_VERSION "0.6-meiki")

# ---------------- kernel ----------------
add_executable(VitaJPOverlay_Kernel
  ${ROOT}/kernel/main.c
  ${ROOT}/kernel/log.c
  ${ROOT}/kernel/buffers.c
  ${ROOT}/kernel/capture.c
  ${ROOT}/kernel/capture_request.c
  ${ROOT}/kernel/display.c
  ${ROOT}/kernel/input.c
  ${ROOT}/kernel/lifecycle.c
  ${ROOT}/kernel/foreground.c
  ${ROOT}/kernel/triggers.c
  ${ROOT}/kernel/game_ocr.c
  ${ROOT}/core/utf.c)
target_compile_options(VitaJPOverlay_Kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2 -std=gnu99 -fno-builtin
  -mgeneral-regs-only)
target_link_options(VitaJPOverlay_Kernel PRIVATE -nostdlib -Wl,-q)
target_link_libraries(VitaJPOverlay_Kernel
  gcc
  taihenForKernel_stub
  SceThreadmgrForDriver_stub
  SceSysclibForDriver_stub
  SceSysmemForDriver_stub
  SceCpuForDriver_stub
  SceDisplayForDriver_stub
  SceCtrlForDriver_stub
  SceTouchForDriver_stub
  SceIofilemgrForDriver_stub)
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_custom_command(TARGET VitaJPOverlay_Kernel POST_BUILD
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_kernel_instructions.py
    --objdump ${CMAKE_OBJDUMP} $<TARGET_FILE:VitaJPOverlay_Kernel>
  COMMENT "Checking kernel code contains no FPU or NEON instructions")
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Kernel.skprx VitaJPOverlay_Kernel
  UNSAFE
  CONFIG ${ROOT}/kernel/exports.yml)
vita_create_stubs(vjo_kernel_stubs VitaJPOverlay_Kernel ${ROOT}/kernel/exports.yml KERNEL)

# ---------------- shell ----------------
# BearSSL and the portable core, built for SceShell (no newlib: see
# shell/libc_shim.c; no OS entropy/time sources: both are injected).
file(GLOB_RECURSE BEARSSL_SRC ${ROOT}/third_party/bearssl/src/*.c)
add_library(vjo_bearssl STATIC ${BEARSSL_SRC})
target_include_directories(vjo_bearssl PUBLIC ${ROOT}/third_party/bearssl/inc PRIVATE ${ROOT}/third_party/bearssl/src)
target_compile_definitions(vjo_bearssl PRIVATE BR_USE_URANDOM=0 BR_USE_GETENTROPY=0 BR_USE_UNIX_TIME=0
  BR_USE_WIN32_RAND=0 BR_USE_WIN32_TIME=0 BR_RDRAND=0)
target_compile_options(vjo_bearssl PRIVATE -O2 -w -fno-builtin -fshort-wchar -ffunction-sections -fdata-sections)

add_library(vjo_core_vita STATIC
  ${ROOT}/core/arena.c ${ROOT}/core/pb.c ${ROOT}/core/utf.c ${ROOT}/core/lens.c ${ROOT}/core/json.c
  ${ROOT}/core/jpdb.c ${ROOT}/core/textfilter.c ${ROOT}/core/entries.c ${ROOT}/core/jiten.c
  ${ROOT}/core/config.c ${ROOT}/core/regions.c ${ROOT}/core/http.c ${ROOT}/core/tls.c
  ${ROOT}/core/tls_roots.c ${ROOT}/core/client.c ${ROOT}/core/styled.c ${ROOT}/core/jpegsw.c
  ${ROOT}/core/base64.c ${ROOT}/core/anki.c ${ROOT}/core/anki_queue.c ${ROOT}/core/net.c ${ROOT}/core/hachidori.c ${ROOT}/core/local_dict.c
  ${ROOT}/core/ocr_lines.c)
target_include_directories(vjo_core_vita PUBLIC ${ROOT}/core ${ROOT}/include)
target_link_libraries(vjo_core_vita PUBLIC vjo_bearssl)
target_compile_options(vjo_core_vita PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter -fno-builtin -fshort-wchar
  -ffunction-sections -fdata-sections)
if(VJO_WITH_NCNN)
  include(${ROOT}/cmake/LocalOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_NCNN=1)
  target_link_libraries(vjo_ocr PUBLIC vjo_core_vita)
endif()

add_executable(VitaJPOverlay_Shell
  ${ROOT}/shell/boot.c
  ${ROOT}/shell/main.cpp
  ${ROOT}/shell/memory.cpp
  ${ROOT}/shell/overlay.cpp
  ${ROOT}/shell/strip.cpp
  ${ROOT}/shell/worker.c
  ${ROOT}/shell/anki.c
  ${ROOT}/shell/anki_queue.c
  ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/log_vita.c
  ${ROOT}/shell/files.c
  ${ROOT}/shell/libc_shim.c)
add_dependencies(VitaJPOverlay_Shell vjo_kernel_stubs)
# Startup runs before Paf is ready; it only needs integer operations. Avoid
# compiler-generated VFP/NEON copies in the early bootstrap and status logger.
set_source_files_properties(${ROOT}/shell/boot.c ${ROOT}/shell/log_vita.c
  PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_PAF_ALLOC=1)
if(VJO_MEMORY_DIAGNOSTICS)
  target_sources(VitaJPOverlay_Shell PRIVATE ${ROOT}/shell/memory_probe.c)
  target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_MEMORY_DIAGNOSTICS=1)
  set_source_files_properties(${ROOT}/shell/memory_probe.c
    PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
  # The optional range-query import must not prevent Shell from loading on
  # firmware that lacks it. Probes guard every Sysmem stub before any call.
  set(VJO_SHELL_SYSMEM_STUB SceSysmem_stub_weak)
else()
  set(VJO_SHELL_SYSMEM_STUB SceSysmem_stub)
endif()
if(VJO_WITH_MEIKI)
  include(${ROOT}/cmake/MeikiOCR.cmake)
  add_subdirectory(${VJO_MEIKI_SOURCE_DIR}/module ${CMAKE_CURRENT_BINARY_DIR}/meiki-module)
  if(VJO_MEIKI_GAME_WORKER)
    target_sources(VitaJPOverlay_Shell PRIVATE ${ROOT}/shell/game_ocr_client.c)
    target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_MEIKI_GAME_WORKER=1)
  else()
    target_sources(VitaJPOverlay_Shell PRIVATE ${ROOT}/shell/meiki_bridge.c)
  endif()
  target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_WITH_MEIKI=1)
  target_link_libraries(VitaJPOverlay_Shell vjo_meiki SceKernelModulemgr_stub)
endif()

# A lazy OCR worker: SDK-owned USER blocks, no newlib allocator, Paf or text
# hooks. The frozen engine is loaded only for an explicitly submitted job.
if(VJO_MEIKI_GAME_WORKER)
  add_executable(VitaJPOverlay_OCR ${ROOT}/game_ocr/main.c
    ${ROOT}/game_ocr/libc_math.c ${ROOT}/shell/meiki_bridge.c ${ROOT}/shell/libc_shim.c)
  add_dependencies(VitaJPOverlay_OCR vjo_kernel_stubs)
  target_compile_definitions(VitaJPOverlay_OCR PRIVATE VJO_GAME_OCR_WORKER=1)
  target_compile_options(VitaJPOverlay_OCR PRIVATE -O2 -Wall -Wextra -Wno-unused-parameter
    -fno-builtin -fshort-wchar -ffunction-sections -fdata-sections)
  target_link_options(VitaJPOverlay_OCR PRIVATE -nostdlib -nostartfiles -Wl,-q
    -Wl,--gc-sections -Wl,--undefined=module_start -Wl,--undefined=module_stop -Wl,--no-wchar-size-warning)
  target_link_directories(VitaJPOverlay_OCR PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
  target_link_libraries(VitaJPOverlay_OCR vjo_meiki vjo_core_vita
    VitaJPOverlay_Kernel_stub_weak SceKernelModulemgr_stub SceLibKernel_stub
    SceKernelThreadMgr_stub SceSysmem_stub_weak SceProcessmgr_stub SceIofilemgr_stub
    vjo_bearssl gcc)
  set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
  set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
  vita_create_self(VitaJPOverlay_OCR.suprx VitaJPOverlay_OCR UNSAFE
    CONFIG ${ROOT}/game_ocr/exports.yml)
endif()
if(VJO_WITH_NCNN)
  target_link_libraries(VitaJPOverlay_Shell vjo_ocr ncnn)
endif()
target_compile_options(VitaJPOverlay_Shell PRIVATE -O2 -Wall -Wno-unused-parameter -fno-builtin -fshort-wchar
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-Wno-sign-compare;-Wno-unused-function>"
  "-D__declspec(x)=")
target_link_directories(VitaJPOverlay_Shell PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
# The converter appends import/export metadata to the RO segment. Preserve
# enough linker headroom before the SDK's 64 KiB-aligned RW segment.
target_link_options(VitaJPOverlay_Shell PRIVATE -nostdlib -nostartfiles -Wl,-q
  -Wl,--no-wchar-size-warning -Wl,--defsym=__sce_headroom=4096)
target_link_libraries(VitaJPOverlay_Shell
  vjo_core_vita
  VitaJPOverlay_Kernel_stub_weak  # weak: the shell still starts (and reports) without the kernel plugin
  taihen_stub
  ScePaf_stub_weak
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  ${VJO_SHELL_SYSMEM_STUB}
  SceProcessmgr_stub
  SceIofilemgr_stub
  SceVshBridge_stub
  # Weak: SceShell loads these after *main plugins start; the imports are
  # bound once the libraries load (as with ScePaf).
  SceNet_stub_weak
  SceNetCtl_stub_weak
  SceRtc_stub_weak
  SceAppMgr_stub_weak
  vjo_bearssl  # again after the core: its integer division comes from libgcc
  gcc)
set(VITA_ELF_CREATE_FLAGS "${VJO_ELF_FLAGS}")
set(VITA_MAKE_FSELF_FLAGS "${VJO_SELF_FLAGS}")
vita_create_self(VitaJPOverlay_Shell.suprx VitaJPOverlay_Shell
  UNSAFE
  CONFIG ${ROOT}/shell/exports.yml)
set(VJO_GAME_ABI_ARGS)
set(VJO_GAME_ABI_DEPENDENCIES)
if(VJO_MEIKI_GAME_WORKER)
  set(VJO_GAME_ABI_ARGS --game ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx)
  set(VJO_GAME_ABI_DEPENDENCIES VitaJPOverlay_OCR.suprx-self)
endif()
add_custom_target(vjo_syscall_abi ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
    --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
    ${VJO_GAME_ABI_ARGS}
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self ${VJO_GAME_ABI_DEPENDENCIES}
  COMMENT "Checking packaged syscall ABI and firmware-independent kernel imports")

# ---------------- RCO ----------------
find_program(PSP2CXML psp2cxml-tool HINTS ${ROOT}/.refs/toolchain/psp2cxml-tool/mbuild ${ROOT}/.refs/psp2cxml-tool/mbuild ENV PATH REQUIRED)
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/shell/rco/vitajpoverlay.xml ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  COMMAND ${PSP2CXML} ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.xml
  DEPENDS ${ROOT}/shell/rco/vitajpoverlay.xml
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_custom_target(vjo_rco ALL DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco)

# ---------------- release zip ----------------
# config.ini sits at the zip's top level (the user edits it and copies it to
# ux0:data/VitaJPOverlay/), so extracting an update over ur0 never overwrites
# the user's API key.
set(STAGE ${CMAKE_CURRENT_BINARY_DIR}/release)
set(MEIKI_RELEASE_COMMAND)
set(GAME_OCR_RELEASE_COMMAND)
if(VJO_MEIKI_GAME_WORKER)
  set(GAME_OCR_RELEASE_COMMAND COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_OCR.suprx ${STAGE}/ur0/tai/)
endif()
if(VJO_WITH_MEIKI)
  set(MEIKI_RELEASE_COMMAND COMMAND ${CMAKE_COMMAND} -E copy
    ${CMAKE_CURRENT_BINARY_DIR}/meiki-module/meiki-engine.suprx ${STAGE}/ur0/data/VitaJPOverlay/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/docs/benchmarks
    COMMAND ${CMAKE_COMMAND} -E copy
      ${ROOT}/docs/benchmarks/meiki-projection-baseline.json
      ${ROOT}/docs/benchmarks/meiki-single-line.json
      ${ROOT}/docs/benchmarks/meiki-single-line-margin2.json
      ${ROOT}/docs/benchmarks/meiki-dialogue-box.json
      ${STAGE}/docs/benchmarks/)
endif()
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ur0/tai ${STAGE}/ur0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco ${STAGE}/ur0/data/VitaJPOverlay/
  ${MEIKI_RELEASE_COMMAND}
  ${GAME_OCR_RELEASE_COMMAND}
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/README.md ${ROOT}/LICENSE ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/config.example.ini ${STAGE}/config.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools ${STAGE}/docs/benchmarks ${STAGE}/third_party
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/convert_dictionary.py ${ROOT}/tools/install_dictionary.py ${ROOT}/tools/prepare_ocr.py ${STAGE}/tools/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/local-dictionaries.md ${ROOT}/docs/local-ocr.md ${ROOT}/docs/lookup-performance.md ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/local-lookup-jitendex.json ${ROOT}/docs/benchmarks/kernel-startup-20261010.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/meiki-ocr.md ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/meiki-dependencies.json ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/README.md ${STAGE}/third_party/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/third_party/licenses ${STAGE}/third_party/licenses
  COMMAND ${CMAKE_COMMAND} -E chdir ${STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay-${VJO_VERSION}.zip --format=zip ur0 config.ini README.md LICENSE tools docs third_party
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self vjo_rco vjo_syscall_abi
  COMMENT "Packaging VitaJPOverlay-${VJO_VERSION}.zip")
if(VJO_WITH_MEIKI)
  add_dependencies(release meiki-engine.suprx-self)
endif()
if(VJO_MEIKI_GAME_WORKER)
  add_dependencies(release VitaJPOverlay_OCR.suprx-self)
endif()
