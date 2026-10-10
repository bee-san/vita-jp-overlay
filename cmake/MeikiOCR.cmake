# The C line adapter is portable. MNN/newlib live only in meiki-engine.suprx.
set(VJO_MEIKI_SOURCE_DIR "" CACHE PATH "Pinned vita-vn-ocr checkout containing ports/meiki")
if(NOT EXISTS "${VJO_MEIKI_SOURCE_DIR}/preprocess.c" OR
   NOT EXISTS "${VJO_MEIKI_SOURCE_DIR}/module/api.h")
  message(FATAL_ERROR "Set VJO_MEIKI_SOURCE_DIR to the pinned vita-vn-ocr ports/meiki directory")
endif()
include(${CMAKE_CURRENT_LIST_DIR}/MeikiSourcePin.cmake)
add_library(vjo_meiki STATIC
  ${VJO_ROOT}/core/meiki_ocr.c
  ${VJO_MEIKI_SOURCE_DIR}/preprocess.c
  ${VJO_MEIKI_SOURCE_DIR}/detect_preprocess.c
  ${VJO_MEIKI_SOURCE_DIR}/decode.c)
target_include_directories(vjo_meiki PUBLIC ${VJO_ROOT}/core ${VJO_MEIKI_SOURCE_DIR})
target_compile_options(vjo_meiki PRIVATE -O2 -Wall -Wextra -ffp-contract=off
  -ffunction-sections -fdata-sections)
if(VJO_BUILD_VITA)
  target_compile_options(vjo_meiki PRIVATE -fno-builtin -fshort-wchar)
  target_link_libraries(vjo_meiki PUBLIC vjo_core_vita)
else()
  target_link_libraries(vjo_meiki PUBLIC vjo_core m)
endif()
