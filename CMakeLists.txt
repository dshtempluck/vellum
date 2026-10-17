cmake_minimum_required(VERSION 3.20)
project(vellum CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "vellum targets Windows (Win32 + GDI)")
endif()

# Source-based coverage, which needs clang-cl rather than cl.
option(VELLUM_COVERAGE "Instrument for llvm-cov coverage" OFF)
if(VELLUM_COVERAGE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "VELLUM_COVERAGE needs clang-cl")
  endif()
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /Zc:preprocessor /utf-8 /EHsc /MP)
  # Narrowing and signed/unsigned noise from the codec maths would drown
  # out real warnings; the conversions there are deliberate and checked.
  add_compile_options(/wd4244 /wd4267 /wd4100)
  # NOMINMAX only: WIN32_LEAN_AND_MEAN is set per translation unit, because
  # the UI needs the common-control and common-dialog headers it excludes.
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Format back ends and the document model.
# ---------------------------------------------------------------------------
add_library(vellum_core STATIC
  src/core/bytes.cpp
  src/core/bitmap.cpp
  src/djvu/zp.cpp
  src/djvu/bzz.cpp
  src/djvu/iff.cpp
  src/djvu/jb2.cpp
  src/djvu/iw44.cpp
  src/djvu/document.cpp
  src/image/document.cpp
  src/doc/document.cpp
  src/view/layout.cpp
  src/view/render_cache.cpp
)
target_include_directories(vellum_core PUBLIC src)
# Still images decode through the Windows Imaging Component.
target_link_libraries(vellum_core PUBLIC windowscodecs shlwapi ole32)

# ---------------------------------------------------------------------------
# PDF support via PDFium. Point PDFIUM_ROOT at a prebuilt SDK containing
# include/ and lib/. Without it the viewer still builds, DjVu-only.
# ---------------------------------------------------------------------------
set(PDFIUM_ROOT "" CACHE PATH "Prebuilt PDFium SDK (include/ + lib/ + bin/)")

if(PDFIUM_ROOT AND EXISTS "${PDFIUM_ROOT}/include/fpdfview.h")
  message(STATUS "PDF support: enabled (${PDFIUM_ROOT})")
  target_sources(vellum_core PRIVATE src/pdf/document.cpp)
  target_include_directories(vellum_core PUBLIC "${PDFIUM_ROOT}/include")
  target_link_libraries(vellum_core PUBLIC "${PDFIUM_ROOT}/lib/pdfium.dll.lib")
  target_compile_definitions(vellum_core PUBLIC VELLUM_HAVE_PDF=1)
  set(VELLUM_PDFIUM_DLL "${PDFIUM_ROOT}/bin/pdfium.dll")
else()
  message(STATUS "PDF support: disabled (set PDFIUM_ROOT to enable)")
  target_compile_definitions(vellum_core PUBLIC VELLUM_HAVE_PDF=0)
endif()

# ---------------------------------------------------------------------------
# Command-line harness: decodes documents without a UI, so the back ends
# can be tested and compared on their own.
# ---------------------------------------------------------------------------
add_executable(vellum_probe tools/probe.cpp)
target_link_libraries(vellum_probe PRIVATE vellum_core)

# ---------------------------------------------------------------------------
# The viewer.
# ---------------------------------------------------------------------------
add_executable(vellum WIN32
  src/ui/main_window.cpp
  src/ui/toolbar.cpp
  src/ui/tab_bar.cpp
  src/ui/nav_pane.cpp
  src/ui/page_view.cpp
  src/ui/find_bar.cpp
  src/ui/app.cpp
  src/ui/vellum.rc
)
target_link_libraries(vellum PRIVATE vellum_core comctl32 shlwapi)

# The manifest comes from vellum.rc, so the linker must not add its own.
if(MSVC)
  target_link_options(vellum PRIVATE /MANIFEST:NO)
endif()

if(DEFINED VELLUM_PDFIUM_DLL AND EXISTS "${VELLUM_PDFIUM_DLL}")
  # The viewer loads pdfium.dll from its own directory.
  add_custom_command(TARGET vellum POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VELLUM_PDFIUM_DLL}" "$<TARGET_FILE_DIR:vellum>"
    COMMENT "Staging pdfium.dll next to vellum.exe")
  add_custom_command(TARGET vellum_probe POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VELLUM_PDFIUM_DLL}" "$<TARGET_FILE_DIR:vellum_probe>"
    COMMENT "Staging pdfium.dll next to vellum_probe.exe")
endif()

# ---------------------------------------------------------------------------
# Tests. Everything except the Win32 windows is covered here; the UI needs a
# running message loop and is exercised by tools/screenshot.ps1 instead.
# ---------------------------------------------------------------------------
enable_testing()

add_executable(vellum_tests
  tests/main.cpp
  tests/fixtures.cpp
  tests/test_core.cpp
  tests/test_iff.cpp
  tests/test_zp_bzz.cpp
  tests/test_codecs.cpp
  tests/test_djvu_document.cpp
  tests/test_layout.cpp
  tests/test_render_cache.cpp
  tests/test_dispatch.cpp
  tests/test_image.cpp
  tests/test_pdf.cpp
  tests/test_robustness.cpp
)
target_link_libraries(vellum_tests PRIVATE vellum_core)
target_include_directories(vellum_tests PRIVATE third_party/doctest tests)

# doctest specialises std::tuple, which the current MSVC library objects
# to. It is vendored code, so the warning is silenced rather than patched.
if(MSVC)
  target_compile_options(vellum_tests PRIVATE /wd5285)
endif()

# The suite locates the sample corpus relative to the source tree.
target_compile_definitions(vellum_tests PRIVATE
  VELLUM_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

if(DEFINED VELLUM_PDFIUM_DLL AND EXISTS "${VELLUM_PDFIUM_DLL}")
  add_custom_command(TARGET vellum_tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VELLUM_PDFIUM_DLL}" "$<TARGET_FILE_DIR:vellum_tests>"
    COMMENT "Staging pdfium.dll next to vellum_tests.exe")
endif()

add_test(NAME vellum_tests COMMAND vellum_tests)
