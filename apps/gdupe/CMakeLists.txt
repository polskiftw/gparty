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
find_package(PNG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-libvpx CONFIG REQUIRED)
find_package(unofficial-libwebm CONFIG REQUIRED)

# vcpkg's dav1d port exposes pkg-config metadata only, but requiring an external
# pkg-config executable on Windows adds needless tool discovery. Resolve the
# header and static archive directly through the active vcpkg toolchain.
find_path(DAV1D_INCLUDE_DIR NAMES dav1d/dav1d.h REQUIRED)
find_library(DAV1D_LIBRARY NAMES dav1d REQUIRED)
add_library(Dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(Dav1d::dav1d PROPERTIES
  IMPORTED_LOCATION "${DAV1D_LIBRARY}"
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

# Tiny ISO Base Media demuxer. Header-only, pinned to a concrete upstream
# commit so malformed-input behavior cannot drift under an existing database.
FetchContent_Declare(minimp4_source
  GIT_REPOSITORY https://github.com/lieff/minimp4.git
  GIT_TAG 5a212a18dba7dca09543bbc7d65619274fd2931a
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(minimp4_source)
add_library(gdupe_minimp4 INTERFACE)
target_include_directories(gdupe_minimp4 INTERFACE "${minimp4_source_SOURCE_DIR}")

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

# Temporary transition dependency. The final static-media branch deletes this
# entire FFmpeg block once media_decode.cpp owns all moving-media decoding.
set(GDUPE_FFMPEG_DIR "" CACHE PATH "Directory containing gdupe's pinned minimal FFmpeg SDK")
if(NOT GDUPE_FFMPEG_DIR)
  message(FATAL_ERROR "GDUPE_FFMPEG_DIR must point to the pinned minimal FFmpeg SDK during transition")
endif()

function(gdupe_import_ffmpeg NAME DLL_NAME)
  set(IMPLIB "${GDUPE_FFMPEG_DIR}/lib/${NAME}.lib")
  set(DLL "${GDUPE_FFMPEG_DIR}/${DLL_NAME}")
  if(NOT EXISTS "${IMPLIB}")
    message(FATAL_ERROR "Pinned minimal FFmpeg SDK is missing ${IMPLIB}")
  endif()
  if(NOT EXISTS "${DLL}")
    message(FATAL_ERROR "Pinned minimal FFmpeg runtime is missing ${DLL}")
  endif()
  add_library(FFmpeg::${NAME} SHARED IMPORTED GLOBAL)
  set_target_properties(FFmpeg::${NAME} PROPERTIES
    IMPORTED_IMPLIB "${IMPLIB}"
    IMPORTED_LOCATION "${DLL}"
    INTERFACE_INCLUDE_DIRECTORIES "${GDUPE_FFMPEG_DIR}/include"
  )
endfunction()

gdupe_import_ffmpeg(avcodec avcodec-63.dll)
gdupe_import_ffmpeg(avformat avformat-63.dll)
gdupe_import_ffmpeg(avutil avutil-61.dll)
gdupe_import_ffmpeg(swscale swscale-10.dll)

add_library(gdupe_core
  src/config.cpp
  src/database.cpp
  src/b2_client.cpp
  src/crypto_hash.cpp
  src/image_decode.cpp
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
    SQLite3::SQLite3
    nlohmann_json::nlohmann_json
    gdupe_minimp4
    Dav1d::dav1d
    unofficial::libvpx::libvpx
    unofficial::libwebm::libwebm
    AOSP::libavcdec
    AOSP::libhevcdec
    FFmpeg::avformat
    FFmpeg::avcodec
    FFmpeg::avutil
    FFmpeg::swscale
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
target_link_libraries(gdupe PRIVATE gdupe_core fltk::fltk fltk::images)
if(WIN32)
  target_link_libraries(gdupe PRIVATE mfplay mfplat mfuuid ole32 shlwapi)
endif()

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gdupe portable runtime directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gdupe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES config/gdupe.example.json DESTINATION config)
# Temporary transition package content; final static-media packaging removes
# all four DLLs and this FFmpeg license directory.
install(FILES
  "${GDUPE_FFMPEG_DIR}/avcodec-63.dll"
  "${GDUPE_FFMPEG_DIR}/avformat-63.dll"
  "${GDUPE_FFMPEG_DIR}/avutil-61.dll"
  "${GDUPE_FFMPEG_DIR}/swscale-10.dll"
  DESTINATION .
)
install(FILES
  "${GDUPE_FFMPEG_DIR}/LICENSE.txt"
  "${GDUPE_FFMPEG_DIR}/LICENSE-NOTICE.md"
  "${GDUPE_FFMPEG_DIR}/SOURCE.txt"
  "${GDUPE_FFMPEG_DIR}/BUILD-CONFIG.txt"
  DESTINATION licenses/ffmpeg
)
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
  set_tests_properties(gdupe_core_tests PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${GDUPE_FFMPEG_DIR}"
  )
endif()
