cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(gdupe VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

find_package(JPEG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(PNG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-libvpx CONFIG REQUIRED)
find_package(unofficial-libwebm CONFIG REQUIRED)

# vcpkg's dav1d port exposes pkg-config metadata only. Resolve its header and
# static archives directly, but keep Release and Debug paths distinct: a plain
# find_library() under a multi-config vcpkg tree can otherwise pick debug/lib
# first and silently inject LIBCMTD into a Release executable.
set(GDUPE_VCPKG_TRIPLET_ROOT
  "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(DAV1D_INCLUDE_DIR NAMES dav1d/dav1d.h
  PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/include"
  NO_DEFAULT_PATH REQUIRED)
find_library(DAV1D_LIBRARY_RELEASE NAMES dav1d
  PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/lib"
  NO_DEFAULT_PATH REQUIRED)
find_library(DAV1D_LIBRARY_DEBUG NAMES dav1d
  PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/debug/lib"
  NO_DEFAULT_PATH REQUIRED)
add_library(Dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(Dav1d::dav1d PROPERTIES
  IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
  IMPORTED_LOCATION_DEBUG "${DAV1D_LIBRARY_DEBUG}"
  IMPORTED_LOCATION_RELEASE "${DAV1D_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_RELWITHDEBINFO "${DAV1D_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_MINSIZEREL "${DAV1D_LIBRARY_RELEASE}"
  INTERFACE_INCLUDE_DIRECTORIES "${DAV1D_INCLUDE_DIR}"
)

# Source-pinned FLTK, built as a static /MT library with unused subsystems off.
set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_HTML_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_PDF_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_INSTALL_HTML_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_INSTALL_PDF_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_INSTALL_FLUID_DOCS OFF CACHE BOOL "" FORCE)
set(FLTK_INSTALL_LINKS OFF CACHE BOOL "" FORCE)
set(FLTK_GRAPHICS_GDIPLUS OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_CAIRO_EXT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_CAIRO_WINDOW OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_PRINT_SUPPORT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_FILESYSTEM_SUPPORT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_LARGE_FILE OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_SVG OFF CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_LIBJPEG ON CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_LIBPNG ON CACHE BOOL "" FORCE)
set(FLTK_USE_SYSTEM_ZLIB ON CACHE BOOL "" FORCE)
include(FetchContent)
FetchContent_Declare(fltk
  GIT_REPOSITORY https://github.com/fltk/fltk.git
  GIT_TAG a9b1113516ffd15fc7602a6d425a317df30f4720
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(fltk)

# FLTK 1.4 stores FindJPEG/FindPNG's old-style optimized/debug list in the
# fltk_images interface. Under multi-config MSVC that can leak the debug image
# archive into a Release consumer and request LIBCMTD. Keep FLTK itself intact,
# but replace only that dependency interface with modern config-aware targets.
if(TARGET fltk_images)
  set_property(TARGET fltk_images PROPERTY LINK_LIBRARIES
    "fltk::fltk;JPEG::JPEG;PNG::PNG;ZLIB::ZLIB")
  set_property(TARGET fltk_images PROPERTY INTERFACE_LINK_LIBRARIES
    "fltk::fltk;$<LINK_ONLY:JPEG::JPEG>;$<LINK_ONLY:PNG::PNG>;$<LINK_ONLY:ZLIB::ZLIB>")
endif()

# Tiny ISO Base Media sample-table demuxer. gdupe parses avcC/hvcC itself
# because minimp4 discards AVC's NAL-length field and its HEVC sample-entry
# traversal is incomplete. Disable that HEVC traversal in our generated copy;
# minimp4 remains responsible only for the generic MP4 sample tables.
FetchContent_Declare(minimp4_source
  GIT_REPOSITORY https://github.com/lieff/minimp4.git
  GIT_TAG 5a212a18dba7dca09543bbc7d65619274fd2931a
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(minimp4_source)
file(READ "${minimp4_source_SOURCE_DIR}/minimp4.h" GDUPE_MINIMP4_HEADER)
set(GDUPE_MINIMP4_HEVC_NEEDLE "#define MP4D_HEVC_SUPPORTED       1")
string(FIND "${GDUPE_MINIMP4_HEADER}" "${GDUPE_MINIMP4_HEVC_NEEDLE}"
  GDUPE_MINIMP4_HEVC_POSITION)
if(GDUPE_MINIMP4_HEVC_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned minimp4 HEVC switch changed; review gdupe's local demux patch")
endif()
string(REPLACE
  "${GDUPE_MINIMP4_HEVC_NEEDLE}"
  "#define MP4D_HEVC_SUPPORTED       0"
  GDUPE_MINIMP4_HEADER
  "${GDUPE_MINIMP4_HEADER}"
)
set(GDUPE_MINIMP4_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/minimp4")
file(MAKE_DIRECTORY "${GDUPE_MINIMP4_INCLUDE_DIR}")
file(WRITE "${GDUPE_MINIMP4_INCLUDE_DIR}/minimp4.h" "${GDUPE_MINIMP4_HEADER}")
add_library(gdupe_minimp4 INTERFACE)
target_include_directories(gdupe_minimp4 INTERFACE "${GDUPE_MINIMP4_INCLUDE_DIR}")

# Source-pinned AOSP H.264/HEVC decoders built by CI as static /MT SDKs.
set(GDUPE_LIBAVC_DIR "" CACHE PATH "Directory containing gdupe's static AOSP libavc SDK")
set(GDUPE_LIBHEVC_DIR "" CACHE PATH "Directory containing gdupe's static AOSP libhevc SDK")
foreach(CODEC IN ITEMS LIBAVC LIBHEVC)
  if(NOT GDUPE_${CODEC}_DIR)
    message(FATAL_ERROR "GDUPE_${CODEC}_DIR must point to gdupe's static AOSP ${CODEC} SDK")
  endif()
endforeach()

add_library(AOSP::libavcdec STATIC IMPORTED GLOBAL)
set_target_properties(AOSP::libavcdec PROPERTIES
  IMPORTED_LOCATION "${GDUPE_LIBAVC_DIR}/lib/libavcdec.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${GDUPE_LIBAVC_DIR}/include/common;${GDUPE_LIBAVC_DIR}/include/decoder"
)
if(NOT EXISTS "${GDUPE_LIBAVC_DIR}/lib/libavcdec.lib")
  message(FATAL_ERROR "Static AOSP libavc SDK is missing libavcdec.lib")
endif()

add_library(AOSP::libhevcdec STATIC IMPORTED GLOBAL)
set_target_properties(AOSP::libhevcdec PROPERTIES
  IMPORTED_LOCATION "${GDUPE_LIBHEVC_DIR}/lib/libhevcdec.lib"
  INTERFACE_INCLUDE_DIRECTORIES "${GDUPE_LIBHEVC_DIR}/include/common;${GDUPE_LIBHEVC_DIR}/include/decoder"
)
if(NOT EXISTS "${GDUPE_LIBHEVC_DIR}/lib/libhevcdec.lib")
  message(FATAL_ERROR "Static AOSP libhevc SDK is missing libhevcdec.lib")
endif()

# Compile the AOSP adapters in isolation so libavc/libhevc's same-named common
# headers never bleed into one another. Only annexb_decoder.hpp crosses this
# boundary into the rest of gdupe.
add_library(gdupe_avc_adapter STATIC src/aosp_avc_decode.cpp)
target_include_directories(gdupe_avc_adapter PUBLIC src)
target_link_libraries(gdupe_avc_adapter PRIVATE AOSP::libavcdec)
add_library(gdupe_hevc_adapter STATIC src/aosp_hevc_decode.cpp)
target_include_directories(gdupe_hevc_adapter PUBLIC src)
target_link_libraries(gdupe_hevc_adapter PRIVATE AOSP::libhevcdec)
if(MSVC)
  target_compile_definitions(gdupe_avc_adapter PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_definitions(gdupe_hevc_adapter PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(gdupe_avc_adapter PRIVATE /W4 /permissive- /EHsc /utf-8)
  target_compile_options(gdupe_hevc_adapter PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

add_library(gdupe_core
  src/config.cpp
  src/database.cpp
  src/b2_client.cpp
  src/crypto_hash.cpp
  src/image_decode.cpp
  src/gif_decode.cpp
  src/media_decode.cpp
  src/mp4_decode.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/engine.cpp
)
target_include_directories(gdupe_core PUBLIC src)
target_link_libraries(gdupe_core
  PUBLIC
    JPEG::JPEG
    PNG::PNG
    WebP::webpdecoder
    CURL::libcurl
    unofficial::sqlite3::sqlite3
    nlohmann_json::nlohmann_json
    fltk::fltk
    fltk::images
    gdupe_minimp4
    Dav1d::dav1d
    unofficial::libvpx::libvpx
    unofficial::libwebm::libwebm
    gdupe_avc_adapter
    gdupe_hevc_adapter
)
if(MSVC)
  target_compile_definitions(gdupe_core PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(gdupe_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(gdupe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  target_link_libraries(gdupe_core PUBLIC bcrypt)
endif()

add_executable(gdupe WIN32 src/main.cpp src/main_window.cpp)
target_link_libraries(gdupe PRIVATE gdupe_core)
if(WIN32)
  target_link_libraries(gdupe PRIVATE mfplay mfplat mfuuid ole32 shlwapi)
endif()

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gdupe portable runtime directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gdupe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES
  "${GDUPE_LIBAVC_DIR}/license/LICENSE"
  "${GDUPE_LIBAVC_DIR}/SOURCE.txt"
  DESTINATION licenses/libavc
)
install(FILES
  "${GDUPE_LIBHEVC_DIR}/license/LICENSE"
  "${GDUPE_LIBHEVC_DIR}/SOURCE.txt"
  DESTINATION licenses/libhevc
)
install(FILES "${minimp4_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/minimp4)
install(FILES "${fltk_SOURCE_DIR}/COPYING" FLTK-SOURCE.txt
  DESTINATION licenses/fltk)
install(FILES RUNTIME-SURFACE.md DESTINATION .)
foreach(PACKAGE curl dav1d libjpeg-turbo libpng libvpx libwebm libwebp nlohmann-json sqlite3 zlib)
  install(FILES
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${PACKAGE}/copyright"
    DESTINATION "licenses/vcpkg/${PACKAGE}"
  )
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(gdupe_tests tests/test_core.cpp)
  target_link_libraries(gdupe_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_core_tests COMMAND gdupe_tests)

  add_executable(gdupe_static_media_tests tests/test_static_media.cpp)
  target_link_libraries(gdupe_static_media_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_static_media_tests COMMAND gdupe_static_media_tests)
endif()
