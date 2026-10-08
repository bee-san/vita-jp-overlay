# CPU-only, pinned ncnn; no OpenCV, ONNX, pthread/OpenMP or GPU dependencies.
include(FetchContent)
FetchContent_Declare(ncnn
  URL https://github.com/Tencent/ncnn/archive/3798be64cfeeebc0afcfcebfb43ed5c9e1b72e82.tar.gz
  URL_HASH SHA256=276ed0523e7a80048957c2bff89f37242cadd8858ed026a5538222af338e9fde)
FetchContent_GetProperties(ncnn)
if(NOT ncnn_POPULATED)
  # Populate before add_subdirectory so only the required operators are built.
  if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
  endif()
  FetchContent_Populate(ncnn)
endif()
set(_ocr_layers BinaryOp HardSwish Convolution ConvolutionDepthWise Split Swish LayerNorm Gemm
  Reshape Pooling Permute HardSigmoid MultiHeadAttention Input Concat Squeeze Softmax
  Padding Packing Cast Flatten InnerProduct ReLU Clip Sigmoid Mish TanH)
file(STRINGS "${ncnn_SOURCE_DIR}/src/CMakeLists.txt" _ncnn_layers REGEX "^ncnn_add_layer\\(")
foreach(_line IN LISTS _ncnn_layers)
  string(REGEX REPLACE "^ncnn_add_layer\\(([A-Za-z0-9]+).*" "\\1" _layer "${_line}")
  string(TOLOWER "${_layer}" _lower)
  if(_layer IN_LIST _ocr_layers)
    set(WITH_LAYER_${_lower} ON CACHE BOOL "" FORCE)
  else()
    set(WITH_LAYER_${_lower} OFF CACHE BOOL "" FORCE)
  endif()
endforeach()
foreach(_off VULKAN OPENMP THREADS STDIO STRING BENCHMARK C_API PLATFORM_API BATCH
    PIXEL_ROTATE PIXEL_AFFINE PIXEL_DRAWING BUILD_TESTS BUILD_TOOLS BUILD_EXAMPLES
    BUILD_BENCHMARK PYTHON INSTALL_SDK RUNTIME_CPU INT8 WEIGHT_QUANT BF16 VFPV4
    AVX AVX2 AVX512 AVXVNNI AVXVNNIINT8 AVXVNNIINT16 AVXNECONVERT
    AVX512VNNI AVX512BF16 AVX512FP16 FMA FMA4 F16C XOP)
  set(NCNN_${_off} OFF CACHE BOOL "" FORCE)
endforeach()
foreach(_on SIMPLESTL SIMPLEMATH PIXEL DISABLE_RTTI DISABLE_EXCEPTION VALIDATION)
  set(NCNN_${_on} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${ncnn_SOURCE_DIR}" "${ncnn_BINARY_DIR}" EXCLUDE_FROM_ALL)
get_target_property(_ncnn_interface_options ncnn INTERFACE_COMPILE_OPTIONS)
list(TRANSFORM _ncnn_interface_options REPLACE "^(-fno-rtti|-fno-exceptions)$" "$<$<COMPILE_LANGUAGE:CXX>:\\1>")
set_property(TARGET ncnn PROPERTY INTERFACE_COMPILE_OPTIONS "${_ncnn_interface_options}")
# Reuse the platform's small-object C++ allocation operators. In SceShell
# those belong to Paf; defining ncnn's global operators would replace them.
get_target_property(_ncnn_sources ncnn SOURCES)
list(FILTER _ncnn_sources EXCLUDE REGEX "(^|/)simplestl\\.cpp$")
list(FILTER _ncnn_sources EXCLUDE REGEX "(^|/)expression\\.cpp$")
set_property(TARGET ncnn PROPERTY SOURCES "${_ncnn_sources}")
target_sources(ncnn PRIVATE "${VJO_ROOT}/core/ncnn_placement.cpp" "${VJO_ROOT}/core/ncnn_expression.cpp")
target_compile_options(ncnn PRIVATE -ffunction-sections -fdata-sections
  -include "${VJO_ROOT}/core/ncnn_alloc.h")
set_target_properties(ncnn PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED YES)
if(VJO_BUILD_VITA)
  set_target_properties(ncnn PROPERTIES POSITION_INDEPENDENT_CODE OFF INTERFACE_POSITION_INDEPENDENT_CODE OFF)
  target_compile_options(ncnn PRIVATE -fno-builtin -fshort-wchar -mfpu=neon)
endif()

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(VJO_OCR_ASSETS "${CMAKE_BINARY_DIR}/ocr-model")
add_custom_command(OUTPUT "${VJO_OCR_ASSETS}/vjo_ppocr_data.h"
  COMMAND ${Python3_EXECUTABLE} "${VJO_ROOT}/tools/prepare_ocr.py"
    --output "${VJO_OCR_ASSETS}" --ncnn-source "${ncnn_SOURCE_DIR}"
  DEPENDS "${VJO_ROOT}/tools/prepare_ocr.py"
  COMMENT "Preparing checksum-verified PP-OCRv5 mobile assets")
add_library(vjo_ocr STATIC "${VJO_ROOT}/core/local_ocr.cpp" "${VJO_ROOT}/core/ocr_heap.c"
  "${VJO_OCR_ASSETS}/vjo_ppocr_data.h")
target_include_directories(vjo_ocr PRIVATE "${VJO_OCR_ASSETS}" "${VJO_ROOT}/third_party/bearssl/inc")
# core/net.h and ncnn/net.h have the same basename. Quoted local includes
# resolve beside the source; angle includes must resolve to ncnn first.
target_include_directories(vjo_ocr BEFORE PRIVATE "${ncnn_SOURCE_DIR}/src" "${ncnn_BINARY_DIR}/src")
target_link_libraries(vjo_ocr PUBLIC ncnn)
target_compile_features(vjo_ocr PRIVATE cxx_std_11)
target_compile_options(vjo_ocr PRIVATE -ffunction-sections -fdata-sections
  "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions;-include;${VJO_ROOT}/core/ncnn_alloc.h>")
if(VJO_BUILD_VITA)
  target_compile_options(vjo_ocr PRIVATE -fno-builtin -fshort-wchar -mfpu=neon)
endif()
