cmake_minimum_required(VERSION 3.24)
project(dpdfcomp VERSION 1.0.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(BUILD_TESTING "Build the development test suite (not included in source releases)" OFF)
include(CTest)
include(GNUInstallDirs)

option(DPDFCOMP_BUILD_CLI "Build the command-line consumer" ON)
# Object code is reused by the library and internal developer tests.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include(cmake/Dependencies.cmake)

include(cmake/ThirdParty.cmake)
add_library(dpdfcomp_core OBJECT
  src/document.cpp
  src/image_pdf.cpp src/jbig2_decoder.cpp src/reconstruction.cpp src/bilevel.cpp src/raster.cpp src/image_codecs.cpp src/png_codec.cpp src/jpeg_codec.cpp src/jp2_codec.cpp
  src/pdf.cpp src/image_forms.cpp src/compression.cpp src/images.cpp src/jbig2.cpp src/font_program.cpp
  src/fonts.cpp src/optimizer.cpp)
target_include_directories(dpdfcomp_core PUBLIC include "${PROJECT_BINARY_DIR}/include" PRIVATE src)
target_link_libraries(dpdfcomp_core PUBLIC ${DPDFCOMP_DEPENDENCIES}
  PRIVATE afdko jbig2enc pdfium_jbig2 zopflipng)
target_compile_options(dpdfcomp_core PRIVATE
  $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wall;-Wextra;-Wpedantic>)

if(DPDFCOMP_STATIC)
  set(dpdfcomp_library_type STATIC)
else()
  set(dpdfcomp_library_type SHARED)
endif()
add_library(libdpdfcomp ${dpdfcomp_library_type})
add_library(dpdfcomp::dpdfcomp ALIAS libdpdfcomp)
set_target_properties(libdpdfcomp PROPERTIES OUTPUT_NAME dpdfcomp EXPORT_NAME dpdfcomp
  VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR})
foreach(part dpdfcomp_core afdko jbig2enc pdfium_jbig2 zopflipng)
  set_target_properties(${part} PROPERTIES C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
  target_sources(libdpdfcomp PRIVATE $<TARGET_OBJECTS:${part}>)
endforeach()
target_link_libraries(libdpdfcomp PRIVATE "$<BUILD_INTERFACE:${DPDFCOMP_DEPENDENCIES}>")
target_compile_features(libdpdfcomp PUBLIC cxx_std_20)
target_include_directories(libdpdfcomp PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
if(DPDFCOMP_STATIC)
  target_compile_definitions(dpdfcomp_core PUBLIC DPDFCOMP_STATIC_DEFINE)
  target_compile_definitions(libdpdfcomp PUBLIC DPDFCOMP_STATIC_DEFINE)
  target_link_libraries(libdpdfcomp INTERFACE
    $<INSTALL_INTERFACE:dpdfcomp::dependencies>)
endif()
include(GenerateExportHeader)
generate_export_header(libdpdfcomp BASE_NAME DPDFCOMP
  EXPORT_FILE_NAME "${PROJECT_BINARY_DIR}/include/dpdfcomp/export.hpp")
# The implementation is compiled in the object target rather than this target.
target_compile_definitions(dpdfcomp_core PRIVATE libdpdfcomp_EXPORTS)

if(DPDFCOMP_BUILD_CLI)
  add_executable(dpdfcomp src/main.cpp)
  target_compile_definitions(dpdfcomp PRIVATE DPDFCOMP_VERSION="${PROJECT_VERSION}")
  target_link_libraries(dpdfcomp PRIVATE dpdfcomp::dpdfcomp)
  if(MINGW)
    target_link_options(dpdfcomp PRIVATE -municode)
  endif()
  if(APPLE)
    set_target_properties(dpdfcomp PROPERTIES INSTALL_RPATH "@loader_path/../${CMAKE_INSTALL_LIBDIR}")
  elseif(UNIX)
    set_target_properties(dpdfcomp PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
  endif()
  install(TARGETS dpdfcomp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
if(APPLE)
  set_target_properties(libdpdfcomp PROPERTIES INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set_target_properties(libdpdfcomp PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
install(TARGETS libdpdfcomp EXPORT dpdfcompTargets
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/dpdfcomp/optimizer.hpp
  "${PROJECT_BINARY_DIR}/include/dpdfcomp/export.hpp"
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/dpdfcomp)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/dpdfcompConfig.cmake.in
  "${PROJECT_BINARY_DIR}/dpdfcompConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpdfcomp)
write_basic_package_version_file("${PROJECT_BINARY_DIR}/dpdfcompConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(EXPORT dpdfcompTargets NAMESPACE dpdfcomp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpdfcomp)
install(FILES "${PROJECT_BINARY_DIR}/dpdfcompConfig.cmake"
  "${PROJECT_BINARY_DIR}/dpdfcompConfigVersion.cmake" cmake/Dependencies.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/dpdfcomp)

if(BUILD_TESTING)
  if(NOT DPDFCOMP_BUILD_CLI)
    message(FATAL_ERROR "The development suite requires DPDFCOMP_BUILD_CLI=ON")
  endif()
  # Internal tests can exercise implementation details without exporting them.
  add_library(dpdfcomp_test_support INTERFACE)
  foreach(part dpdfcomp_core afdko jbig2enc pdfium_jbig2 zopflipng)
    target_sources(dpdfcomp_test_support INTERFACE $<TARGET_OBJECTS:${part}>)
  endforeach()
  target_link_libraries(dpdfcomp_test_support INTERFACE ${DPDFCOMP_DEPENDENCIES}
    afdko jbig2enc pdfium_jbig2 zopflipng)
  target_include_directories(dpdfcomp_test_support INTERFACE include "${PROJECT_BINARY_DIR}/include")
  target_compile_definitions(dpdfcomp_test_support INTERFACE DPDFCOMP_STATIC_DEFINE)
  if(NOT EXISTS "${PROJECT_SOURCE_DIR}/tests/tests.cpp")
    message(FATAL_ERROR "Development tests are not included in the minimal source release")
  endif()
  add_executable(dpdfcomp_image_experiments tests/image_experiments.cpp)
  target_include_directories(dpdfcomp_image_experiments PRIVATE src)
  target_link_libraries(dpdfcomp_image_experiments PRIVATE dpdfcomp_test_support jbig2enc TBB::tbb)
  add_executable(dpdfcomp_pdf_stripes tests/pdf_stripes.cpp)
  target_include_directories(dpdfcomp_pdf_stripes PRIVATE src)
  target_link_libraries(dpdfcomp_pdf_stripes PRIVATE dpdfcomp_test_support)
  add_executable(dpdfcomp_font_audit tests/font_audit.cpp)
  target_link_libraries(dpdfcomp_font_audit PRIVATE ${DPDFCOMP_QPDF} ${DPDFCOMP_FREETYPE})
  add_executable(dpdfcomp_tests tests/tests.cpp)
  target_include_directories(dpdfcomp_tests PRIVATE src)
  target_link_libraries(dpdfcomp_tests PRIVATE dpdfcomp_test_support ZLIB::ZLIB)
  add_test(NAME dpdfcomp_tests COMMAND dpdfcomp_tests)
  add_executable(dpdfcomp_byteorder_test tests/byteorder_test.cpp)
  target_link_libraries(dpdfcomp_byteorder_test PRIVATE pdfium_jbig2)
  add_test(NAME byteorder COMMAND dpdfcomp_byteorder_test)
  find_package(Python3 COMPONENTS Interpreter QUIET)
  find_program(PDFTOPPM_EXECUTABLE pdftoppm)
  find_program(PDFTOTEXT_EXECUTABLE pdftotext)
  find_program(QPDF_EXECUTABLE qpdf)
  if(Python3_FOUND AND PDFTOPPM_EXECUTABLE AND PDFTOTEXT_EXECUTABLE AND QPDF_EXECUTABLE)
    add_test(NAME image_options
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/image_options_test.py $<TARGET_FILE:dpdfcomp>)
    add_test(NAME jp2_palette
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/jp2_palette_test.py $<TARGET_FILE:dpdfcomp>)
    add_test(NAME pdf_repair
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/pdf_repair_test.py $<TARGET_FILE:dpdfcomp>)
    add_test(NAME font_regressions
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/font_regression_test.py
        $<TARGET_FILE:dpdfcomp> $<TARGET_FILE:dpdfcomp_font_audit> ${PDFTOPPM_EXECUTABLE})
    add_test(NAME rendering_and_text
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/render_test.py
        $<TARGET_FILE:dpdfcomp_tests> $<TARGET_FILE:dpdfcomp>
        ${PDFTOPPM_EXECUTABLE} ${PDFTOTEXT_EXECUTABLE} ${QPDF_EXECUTABLE}
        $<TARGET_FILE:dpdfcomp_font_audit>)
  endif()
endif()
