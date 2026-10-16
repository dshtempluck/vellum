cmake_minimum_required(VERSION 3.20)
project(vellum CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
  message(FATAL_ERROR "vellum targets Windows (Win32 + GDI)")
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
  src/doc/document.cpp
)
target_include_directories(vellum_core PUBLIC src)

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
  src/view/layout.cpp
  src/view/render_cache.cpp
  src/ui/main_window.cpp
  src/ui/toolbar.cpp
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
