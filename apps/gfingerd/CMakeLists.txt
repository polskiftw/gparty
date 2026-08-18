cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(gfingerd VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "gfingerd is intentionally a Windows-only application")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
)

function(gfingerd_enable_strict_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /WX /utf-8
      "$<$<COMPILE_LANGUAGE:CXX>:/permissive->"
      "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
    )
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

find_package(JPEG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-libwebm CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(minimp4_source
  GIT_REPOSITORY https://github.com/lieff/minimp4.git
  GIT_TAG 5a212a18dba7dca09543bbc7d65619274fd2931a
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(minimp4_source)
file(READ "${minimp4_source_SOURCE_DIR}/minimp4.h" GFINGERD_MINIMP4_HEADER)
set(GFINGERD_MINIMP4_HEVC_NEEDLE "#define MP4D_HEVC_SUPPORTED       1")
string(FIND "${GFINGERD_MINIMP4_HEADER}" "${GFINGERD_MINIMP4_HEVC_NEEDLE}" GFINGERD_MINIMP4_HEVC_POSITION)
if(GFINGERD_MINIMP4_HEVC_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned minimp4 HEVC switch changed; review the native demux patch")
endif()
string(REPLACE "${GFINGERD_MINIMP4_HEVC_NEEDLE}" "#define MP4D_HEVC_SUPPORTED       0" GFINGERD_MINIMP4_HEADER "${GFINGERD_MINIMP4_HEADER}")
set(GFINGERD_MINIMP4_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/minimp4")
file(MAKE_DIRECTORY "${GFINGERD_MINIMP4_INCLUDE_DIR}")
file(WRITE "${GFINGERD_MINIMP4_INCLUDE_DIR}/minimp4.h" "${GFINGERD_MINIMP4_HEADER}")
add_library(gfingerd_minimp4 INTERFACE)
target_include_directories(gfingerd_minimp4 SYSTEM INTERFACE
  "${GFINGERD_MINIMP4_INCLUDE_DIR}"
)

FetchContent_Declare(nvcodec_headers_source
  GIT_REPOSITORY https://github.com/FFmpeg/nv-codec-headers.git
  GIT_TAG 0a6fba9a2820628b8103464f4c8753ee05838baa
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nvcodec_headers_source)
add_library(gfingerd_nvcodec_headers INTERFACE)
target_include_directories(gfingerd_nvcodec_headers SYSTEM INTERFACE
  "${nvcodec_headers_source_SOURCE_DIR}/include"
)

set(GDUPE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/src")

# gfingerd intentionally compiles the current gdupe fingerprint/decode sources
# unchanged. gdupe owns those algorithms today; this target consumes them
# without modifying or packaging gdupe itself.
add_library(gfingerd_media_fingerprint
  "${GDUPE_SOURCE_DIR}/crypto_hash.cpp"
  "${GDUPE_SOURCE_DIR}/image_decode.cpp"
  "${GDUPE_SOURCE_DIR}/jpeg_decode.c"
  "${GDUPE_SOURCE_DIR}/wic_gif.cpp"
  "${GDUPE_SOURCE_DIR}/gif_decode.cpp"
  "${GDUPE_SOURCE_DIR}/preview_color.cpp"
  "${GDUPE_SOURCE_DIR}/nvdec_decode.cpp"
  "${GDUPE_SOURCE_DIR}/video_demux.cpp"
  "${GDUPE_SOURCE_DIR}/mp4_demux.cpp"
  "${GDUPE_SOURCE_DIR}/webm_demux.cpp"
  "${GDUPE_SOURCE_DIR}/media_decode.cpp"
  "${GDUPE_SOURCE_DIR}/fingerprint.cpp"
)
target_include_directories(gfingerd_media_fingerprint PUBLIC
  "${GDUPE_SOURCE_DIR}"
)
target_link_libraries(gfingerd_media_fingerprint PUBLIC
  JPEG::JPEG WebP::webpdecoder gfingerd_minimp4 gfingerd_nvcodec_headers
  unofficial::libwebm::libwebm bcrypt windowscodecs ole32
)
if(MSVC)
  target_compile_definitions(gfingerd_media_fingerprint PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
gfingerd_enable_strict_warnings(gfingerd_media_fingerprint)

# The store adapter uses gdupe's existing Database class and schema directly.
# Only gfingerd_* operational tables are appended to the same SQLite file.
add_library(gfingerd_gdupe_store
  src/gdupe_store.cpp
  "${GDUPE_SOURCE_DIR}/database.cpp"
)
target_include_directories(gfingerd_gdupe_store PUBLIC
  src
  "${GDUPE_SOURCE_DIR}"
)
target_link_libraries(gfingerd_gdupe_store PUBLIC unofficial::sqlite3::sqlite3)
gfingerd_enable_strict_warnings(gfingerd_gdupe_store)

add_executable(gfingerd
  src/boot_install.cpp
  src/config.cpp
  src/control_window.cpp
  src/credentials.cpp
  src/readonly_b2.cpp
  src/runtime_stats.cpp
  src/main.cpp
)
target_include_directories(gfingerd PRIVATE
  src
  "${GDUPE_SOURCE_DIR}"
)
target_link_libraries(gfingerd PRIVATE
  gfingerd_media_fingerprint gfingerd_gdupe_store CURL::libcurl
  nlohmann_json::nlohmann_json advapi32 crypt32 shell32 user32
)
gfingerd_enable_strict_warnings(gfingerd)
if(MSVC)
  target_compile_definitions(gfingerd PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_options(gfingerd PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
endif()

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gfingerd application directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gfingerd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION .)
install(FILES config/gfingerd.example.json DESTINATION config)
install(FILES "${minimp4_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/minimp4 RENAME LICENSE.txt)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/licenses/nv-codec-headers/LICENSE.txt" DESTINATION licenses/nv-codec-headers)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/licenses/libjpeg-turbo/LICENSE.md" "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/licenses/libjpeg-turbo/README.ijg" DESTINATION licenses/libjpeg-turbo)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/licenses/libwebm/PATENTS.txt" DESTINATION licenses/libwebm)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../gdupe/licenses/sqlite3/PUBLIC-DOMAIN.txt" DESTINATION licenses/sqlite3)
foreach(PACKAGE curl libwebm libwebp nlohmann-json zlib)
  install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${PACKAGE}/copyright"
    DESTINATION "licenses/${PACKAGE}" RENAME LICENSE.txt)
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(gfingerd_gdupe_store_tests tests/test_gdupe_store.cpp)
  target_link_libraries(gfingerd_gdupe_store_tests PRIVATE gfingerd_gdupe_store)
  gfingerd_enable_strict_warnings(gfingerd_gdupe_store_tests)
  if(MSVC)
    target_link_options(gfingerd_gdupe_store_tests PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
  endif()
  add_test(NAME gfingerd_gdupe_store_tests COMMAND gfingerd_gdupe_store_tests)
endif()
