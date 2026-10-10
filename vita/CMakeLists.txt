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
option(VJO_WITH_VOCR "Build the vita-vn-ocr local OCR backend (needs its runtime sources)" OFF)
set(VJO_VERSION "0.6-lens")
if(VJO_WITH_VOCR)
  set(VJO_VERSION "${VJO_VERSION}-vocr")
endif()


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
  ${ROOT}/kernel/scene.c
  ${ROOT}/kernel/triggers.c)
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
if(VJO_WITH_VOCR)
  include(${ROOT}/cmake/VocrOCR.cmake)
  target_compile_definitions(vjo_core_vita PUBLIC VJO_WITH_VOCR=1)
  target_link_libraries(vjo_vocr PUBLIC vjo_core_vita)
endif()

add_executable(VitaJPOverlay_Shell
  ${ROOT}/shell/boot.c
  ${ROOT}/shell/main.cpp
  ${ROOT}/shell/memory.cpp
  ${ROOT}/shell/overlay.cpp
  ${ROOT}/shell/strip.cpp
  ${ROOT}/shell/worker.c
  ${ROOT}/shell/capture.c
  ${ROOT}/shell/game.c
  ${ROOT}/shell/anki.c
  ${ROOT}/shell/anki_queue.c
  ${ROOT}/shell/platform_vita.c
  ${ROOT}/shell/log_vita.c
  ${ROOT}/shell/files.c
  ${ROOT}/shell/libc_shim.c)
add_dependencies(VitaJPOverlay_Shell vjo_kernel_stubs)
target_compile_definitions(VitaJPOverlay_Shell PRIVATE VJO_PAF_ALLOC=1)
# Startup runs before Paf is ready; it only needs integer operations. Avoid
# compiler-generated VFP/NEON copies in the early bootstrap and status logger.
set_source_files_properties(${ROOT}/shell/boot.c ${ROOT}/shell/log_vita.c
  PROPERTIES COMPILE_OPTIONS "-mgeneral-regs-only")
if(VJO_WITH_NCNN)
  target_link_libraries(VitaJPOverlay_Shell vjo_ocr ncnn)
endif()
if(VJO_WITH_VOCR)
  target_link_libraries(VitaJPOverlay_Shell vjo_vocr)
endif()
target_compile_options(VitaJPOverlay_Shell PRIVATE -O2 -Wall -Wno-unused-parameter -fno-builtin -fshort-wchar
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-Wno-sign-compare;-Wno-unused-function>"
  "-D__declspec(x)=")
target_link_directories(VitaJPOverlay_Shell PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vjo_kernel_stubs)
target_link_options(VitaJPOverlay_Shell PRIVATE -nostdlib -nostartfiles -Wl,-q -Wl,--no-wchar-size-warning)
target_link_libraries(VitaJPOverlay_Shell
  vjo_core_vita
  VitaJPOverlay_Kernel_stub_weak  # weak: the shell still starts (and reports) without the kernel plugin
  taihen_stub
  ScePaf_stub_weak
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceSysmem_stub
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
add_custom_target(vjo_syscall_abi ALL
  COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/check_vita_syscall_abi.py
    --kernel ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx
    --shell ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self
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
add_custom_target(release
  COMMAND ${CMAKE_COMMAND} -E rm -rf ${STAGE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/ur0/tai ${STAGE}/ur0/data/VitaJPOverlay
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Kernel.skprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay_Shell.suprx ${STAGE}/ur0/tai/
  COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/vitajpoverlay.rco ${STAGE}/ur0/data/VitaJPOverlay/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/README.md ${ROOT}/LICENSE ${STAGE}/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/config.example.ini ${STAGE}/config.ini
  COMMAND ${CMAKE_COMMAND} -E make_directory ${STAGE}/tools ${STAGE}/docs/benchmarks ${STAGE}/third_party
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/tools/convert_dictionary.py ${ROOT}/tools/install_dictionary.py ${ROOT}/tools/prepare_ocr.py ${STAGE}/tools/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/local-dictionaries.md ${ROOT}/docs/local-ocr.md ${ROOT}/docs/vita-vn-ocr.md ${ROOT}/docs/lookup-performance.md ${ROOT}/docs/lens-performance.md ${STAGE}/docs/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/docs/benchmarks/local-lookup-jitendex.json ${ROOT}/docs/benchmarks/kernel-startup-20261010.json ${ROOT}/docs/benchmarks/daijirin-import-20261010.json ${ROOT}/docs/benchmarks/lens-connections-20261010.json ${STAGE}/docs/benchmarks/
  COMMAND ${CMAKE_COMMAND} -E copy ${ROOT}/third_party/README.md ${STAGE}/third_party/
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/third_party/licenses ${STAGE}/third_party/licenses
  COMMAND ${CMAKE_COMMAND} -E chdir ${STAGE} ${CMAKE_COMMAND} -E tar cf ${CMAKE_CURRENT_BINARY_DIR}/VitaJPOverlay-${VJO_VERSION}.zip --format=zip ur0 config.ini README.md LICENSE tools docs third_party
  DEPENDS VitaJPOverlay_Kernel.skprx-self VitaJPOverlay_Shell.suprx-self vjo_rco vjo_syscall_abi
  COMMENT "Packaging VitaJPOverlay-${VJO_VERSION}.zip")
