cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(gdupe VERSION 1.0.0 LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "gdupe is intentionally a Windows-only application")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_definitions(
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
)

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
file(READ "${minimp4_source_SOURCE_DIR}/minimp4.h" GDUPE_MINIMP4_HEADER)
set(GDUPE_MINIMP4_HEVC_NEEDLE "#define MP4D_HEVC_SUPPORTED       1")
string(FIND "${GDUPE_MINIMP4_HEADER}" "${GDUPE_MINIMP4_HEVC_NEEDLE}" GDUPE_MINIMP4_HEVC_POSITION)
if(GDUPE_MINIMP4_HEVC_POSITION EQUAL -1)
  message(FATAL_ERROR "Pinned minimp4 HEVC switch changed; review gdupe's local demux patch")
endif()
string(REPLACE "${GDUPE_MINIMP4_HEVC_NEEDLE}" "#define MP4D_HEVC_SUPPORTED       0" GDUPE_MINIMP4_HEADER "${GDUPE_MINIMP4_HEADER}")
set(GDUPE_MINIMP4_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/minimp4")
file(MAKE_DIRECTORY "${GDUPE_MINIMP4_INCLUDE_DIR}")
file(WRITE "${GDUPE_MINIMP4_INCLUDE_DIR}/minimp4.h" "${GDUPE_MINIMP4_HEADER}")
add_library(gdupe_minimp4 INTERFACE)
target_include_directories(gdupe_minimp4 INTERFACE "${GDUPE_MINIMP4_INCLUDE_DIR}")

# Header-only NVIDIA codec API definitions. gdupe does not bundle the CUDA
# toolkit or Video Codec SDK runtime: nvdec_decode.cpp dynamically resolves
# nvcuda.dll and nvcuvid.dll from the installed NVIDIA display driver.
FetchContent_Declare(nvcodec_headers_source
  GIT_REPOSITORY https://github.com/FFmpeg/nv-codec-headers.git
  GIT_TAG 0a6fba9a2820628b8103464f4c8753ee05838baa
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(nvcodec_headers_source)
add_library(gdupe_nvcodec_headers INTERFACE)
target_include_directories(gdupe_nvcodec_headers INTERFACE
  "${nvcodec_headers_source_SOURCE_DIR}/include"
)

add_library(gdupe_core
  src/config.cpp
  src/database.cpp
  src/b2_client.cpp
  src/crypto_hash.cpp
  src/image_decode.cpp
  src/wic_gif.cpp
  src/gif_decode.cpp
  src/nvdec_decode.cpp
  src/media_decode.cpp
  src/mp4_decode.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/engine.cpp
)
target_include_directories(gdupe_core PUBLIC src)
target_link_libraries(gdupe_core PUBLIC
  JPEG::JPEG WebP::webpdecoder CURL::libcurl unofficial::sqlite3::sqlite3
  nlohmann_json::nlohmann_json gdupe_minimp4 gdupe_nvcodec_headers
  unofficial::libwebm::libwebm
)
if(MSVC)
  target_compile_definitions(gdupe_core PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(gdupe_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(gdupe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
target_link_libraries(gdupe_core PUBLIC bcrypt windowscodecs ole32)

add_executable(gdupe WIN32
  src/main.cpp
  src/main_window.cpp
)
target_link_libraries(gdupe PRIVATE gdupe_core)
target_sources(gdupe PRIVATE resources/gdupe.rc resources/gdupe.manifest)
target_link_libraries(gdupe PRIVATE
  d2d1 dwrite dwmapi mfplay mfplat mfuuid ole32 shell32 shlwapi user32
)
if(MSVC)
  target_link_options(gdupe PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
endif()

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gdupe application directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gdupe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION .)
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES "${minimp4_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/minimp4 RENAME LICENSE.txt)
install(FILES "${nvcodec_headers_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/nv-codec-headers RENAME LICENSE.txt)
install(FILES licenses/libjpeg-turbo/LICENSE.md licenses/libjpeg-turbo/README.ijg DESTINATION licenses/libjpeg-turbo)
install(FILES licenses/libwebm/PATENTS.txt DESTINATION licenses/libwebm)
install(FILES licenses/sqlite3/PUBLIC-DOMAIN.txt DESTINATION licenses/sqlite3)
foreach(PACKAGE curl libwebm libwebp nlohmann-json zlib)
  install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${PACKAGE}/copyright"
    DESTINATION "licenses/${PACKAGE}" RENAME LICENSE.txt)
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(gdupe_tests tests/test_core.cpp)
  target_link_libraries(gdupe_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_core_tests COMMAND gdupe_tests)
  # The H.264/NVDEC assertion is deliberately last in test_core.cpp. On a
  # hosted runner with no NVIDIA driver all preceding CPU-only assertions have
  # already completed; mark only that terminal missing-driver condition as a
  # skip. NVIDIA-equipped machines must continue through and pass H.264.
  set_tests_properties(gdupe_core_tests PROPERTIES
    SKIP_REGULAR_EXPRESSION "NVIDIA CUDA driver runtime .* is not available")

  add_executable(gdupe_static_media_tests tests/test_static_media.cpp)
  target_link_libraries(gdupe_static_media_tests PRIVATE gdupe_core)
  add_executable(gdupe_nvdec_test_gate tests/nvdec_test_gate.cpp)
  target_link_libraries(gdupe_nvdec_test_gate PRIVATE gdupe_core)
  add_test(NAME gdupe_static_media_tests
    COMMAND gdupe_nvdec_test_gate $<TARGET_FILE:gdupe_static_media_tests>)
  set_tests_properties(gdupe_static_media_tests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(gdupe_main10_tests tests/test_main10.cpp)
  target_link_libraries(gdupe_main10_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_main10_tests COMMAND gdupe_main10_tests)
  set_tests_properties(gdupe_main10_tests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(gdupe_nvdec_webm_tests tests/test_nvdec_webm.cpp)
  target_link_libraries(gdupe_nvdec_webm_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_nvdec_webm_tests COMMAND gdupe_nvdec_webm_tests)
  set_tests_properties(gdupe_nvdec_webm_tests PROPERTIES SKIP_RETURN_CODE 77)

  add_executable(gdupe_gif_tests tests/test_gif.cpp)
  target_link_libraries(gdupe_gif_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_gif_tests COMMAND gdupe_gif_tests)

  if(MSVC)
    foreach(TEST_TARGET IN ITEMS gdupe_tests gdupe_static_media_tests gdupe_nvdec_test_gate gdupe_main10_tests gdupe_nvdec_webm_tests gdupe_gif_tests)
      target_link_options(${TEST_TARGET} PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
    endforeach()
  endif()
endif()
