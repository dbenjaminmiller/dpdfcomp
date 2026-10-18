# Only the font libraries we use; no AFDKO command-line tools or Python runtime.
set(AFDKO_ROOT ${PROJECT_SOURCE_DIR}/third_party/afdko)
file(GLOB_RECURSE AFDKO_SOURCES CONFIGURE_DEPENDS ${AFDKO_ROOT}/*.c ${AFDKO_ROOT}/*.cpp)
# Use upstream's C++ exception mode so parse failures return to the caller.
set_source_files_properties(${AFDKO_ROOT}/support/except.c PROPERTIES LANGUAGE CXX)
add_library(afdko OBJECT ${AFDKO_SOURCES})
target_compile_definitions(afdko PRIVATE $<$<COMPILE_LANGUAGE:CXX>:USE_CXX_EXCEPTION>)
target_include_directories(afdko PUBLIC ${AFDKO_ROOT}/include
  ${AFDKO_ROOT}/resource)
target_include_directories(afdko PUBLIC ${LIBXML2_INCLUDE_DIR})
if(UNIX)
  target_link_libraries(afdko PRIVATE m)
endif()

set(JBIG2_ROOT ${PROJECT_SOURCE_DIR}/third_party/jbig2enc)
add_library(jbig2enc OBJECT
  ${JBIG2_ROOT}/jbig2arith.cc ${JBIG2_ROOT}/jbig2comparator.cc
  ${JBIG2_ROOT}/jbig2enc.cc ${JBIG2_ROOT}/jbig2sym.cc)
target_compile_definitions(jbig2enc PRIVATE VERSION="0.32")
target_include_directories(jbig2enc PUBLIC ${JBIG2_ROOT})
find_path(LEPT_ROOT_INCLUDE leptonica/allheaders.h
  HINTS ${LEPT_INCLUDEDIR} ${LEPT_INCLUDE_DIRS} REQUIRED)
target_include_directories(jbig2enc PUBLIC ${LEPT_ROOT_INCLUDE})
target_link_libraries(jbig2enc PUBLIC ${DPDFCOMP_LEPT})
if(WIN32)
  target_compile_definitions(jbig2enc PUBLIC WIN32 PRIVATE _USE_MATH_DEFINES)
  target_link_libraries(jbig2enc PRIVATE ws2_32)
endif()

set(ZOPFLI_ROOT ${PROJECT_SOURCE_DIR}/third_party/zopfli/src)
file(GLOB ZOPFLI_SOURCES CONFIGURE_DEPENDS ${ZOPFLI_ROOT}/zopfli/*.c)
add_library(zopflipng OBJECT ${ZOPFLI_SOURCES}
  ${ZOPFLI_ROOT}/zopflipng/zopflipng_lib.cc
  ${ZOPFLI_ROOT}/zopflipng/lodepng/lodepng.cpp
  ${ZOPFLI_ROOT}/zopflipng/lodepng/lodepng_util.cpp)
target_include_directories(zopflipng PUBLIC ${ZOPFLI_ROOT}/zopflipng)
if(UNIX)
  target_link_libraries(zopflipng PRIVATE m)
endif()

set(PDFIUM_ROOT ${PROJECT_SOURCE_DIR}/third_party/pdfium)
add_library(pdfium_jbig2 OBJECT
  ${PDFIUM_ROOT}/core/fxcodec/fax/faxmodule.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_arith_decoder.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_arith_int_decoder.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_bit_stream.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_context.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_decoder.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_document_context.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_grd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_grrd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_htrd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_huffman_decoder.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_huffman_table.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_image.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_pattern_dict.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_pdd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_sdd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_segment.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_symbol_dict.cpp
  ${PDFIUM_ROOT}/core/fxcodec/jbig2/jbig2_trd_proc.cpp
  ${PDFIUM_ROOT}/core/fxcodec/scanlinedecoder.cpp
  ${PDFIUM_ROOT}/core/fxcrt/debug/alias.cc
  ${PDFIUM_ROOT}/core/fxcrt/fx_memory.cpp
  ${PDFIUM_ROOT}/core/fxcrt/fx_memory_malloc.cpp
  ${PDFIUM_ROOT}/core/fxge/calculate_pitch.cpp
)
target_include_directories(pdfium_jbig2 SYSTEM PUBLIC ${PDFIUM_ROOT})
# The Windows-only encoder would require PDFium's graphics subsystem.
target_compile_definitions(pdfium_jbig2 PUBLIC DPDFCOMP_DECODER_ONLY)
