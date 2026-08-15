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
if(WIN32)
  add_compile_definitions(
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
  )
endif()

find_package(JPEG REQUIRED)
find_package(WebP CONFIG REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-libvpx CONFIG REQUIRED)
find_package(unofficial-libwebm CONFIG REQUIRED)

set(GDUPE_VCPKG_TRIPLET_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
find_path(DAV1D_INCLUDE_DIR NAMES dav1d/dav1d.h PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/include" NO_DEFAULT_PATH REQUIRED)
find_library(DAV1D_LIBRARY_RELEASE NAMES dav1d PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/lib" NO_DEFAULT_PATH REQUIRED)
find_library(DAV1D_LIBRARY_DEBUG NAMES dav1d PATHS "${GDUPE_VCPKG_TRIPLET_ROOT}/debug/lib" NO_DEFAULT_PATH REQUIRED)
add_library(Dav1d::dav1d STATIC IMPORTED GLOBAL)
set_target_properties(Dav1d::dav1d PROPERTIES
  IMPORTED_CONFIGURATIONS "Debug;Release;RelWithDebInfo;MinSizeRel"
  IMPORTED_LOCATION_DEBUG "${DAV1D_LIBRARY_DEBUG}"
  IMPORTED_LOCATION_RELEASE "${DAV1D_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_RELWITHDEBINFO "${DAV1D_LIBRARY_RELEASE}"
  IMPORTED_LOCATION_MINSIZEREL "${DAV1D_LIBRARY_RELEASE}"
  INTERFACE_INCLUDE_DIRECTORIES "${DAV1D_INCLUDE_DIR}"
)

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
  src/wic_gif.cpp
  src/gif_decode.cpp
  src/media_decode.cpp
  src/mp4_decode.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/engine.cpp
)
target_include_directories(gdupe_core PUBLIC src)
target_link_libraries(gdupe_core PUBLIC
  JPEG::JPEG WebP::webpdecoder CURL::libcurl unofficial::sqlite3::sqlite3
  nlohmann_json::nlohmann_json gdupe_minimp4 Dav1d::dav1d
  unofficial::libvpx::libvpx unofficial::libwebm::libwebm gdupe_avc_adapter gdupe_hevc_adapter
)
if(MSVC)
  target_compile_definitions(gdupe_core PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(gdupe_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(gdupe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
if(WIN32)
  target_link_libraries(gdupe_core PUBLIC bcrypt windowscodecs ole32)
endif()

add_executable(gdupe WIN32
  src/main.cpp
  src/main_window.cpp
)
target_link_libraries(gdupe PRIVATE gdupe_core)
if(WIN32)
  target_sources(gdupe PRIVATE resources/gdupe.rc resources/gdupe.manifest)
  target_link_libraries(gdupe PRIVATE
    d2d1 dwrite dwmapi mfplay mfplat mfuuid ole32 shell32 shlwapi user32
  )
endif()
if(MSVC)
  target_link_options(gdupe PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
endif()

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gdupe application directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gdupe RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES README.md "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION .)
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES "${GDUPE_LIBAVC_DIR}/license/LICENSE" DESTINATION licenses/libavc RENAME LICENSE.txt)
install(FILES "${GDUPE_LIBAVC_DIR}/license/NOTICE" DESTINATION licenses/libavc RENAME NOTICE.txt)
install(FILES "${GDUPE_LIBAVC_DIR}/SOURCE.txt" DESTINATION licenses/libavc)
install(FILES "${GDUPE_LIBHEVC_DIR}/license/LICENSE" DESTINATION licenses/libhevc RENAME LICENSE.txt)
install(FILES "${GDUPE_LIBHEVC_DIR}/license/NOTICE" DESTINATION licenses/libhevc RENAME NOTICE.txt)
install(FILES "${GDUPE_LIBHEVC_DIR}/SOURCE.txt" DESTINATION licenses/libhevc)
install(FILES "${minimp4_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/minimp4 RENAME LICENSE.txt)
install(FILES licenses/libjpeg-turbo/LICENSE.md licenses/libjpeg-turbo/README.ijg DESTINATION licenses/libjpeg-turbo)
install(FILES licenses/libvpx/PATENTS.txt licenses/libvpx/X86INC-ISC.txt DESTINATION licenses/libvpx)
install(FILES licenses/libwebm/PATENTS.txt DESTINATION licenses/libwebm)
install(FILES licenses/sqlite3/PUBLIC-DOMAIN.txt DESTINATION licenses/sqlite3)
foreach(PACKAGE curl dav1d libvpx libwebm libwebp nlohmann-json zlib)
  install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${PACKAGE}/copyright"
    DESTINATION "licenses/${PACKAGE}" RENAME LICENSE.txt)
endforeach()

include(CTest)
if(BUILD_TESTING)
  add_executable(gdupe_tests tests/test_core.cpp)
  target_link_libraries(gdupe_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_core_tests COMMAND gdupe_tests)
  add_executable(gdupe_static_media_tests tests/test_static_media.cpp)
  target_link_libraries(gdupe_static_media_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_static_media_tests COMMAND gdupe_static_media_tests)
  add_executable(gdupe_main10_tests tests/test_main10.cpp)
  target_link_libraries(gdupe_main10_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_main10_tests COMMAND gdupe_main10_tests)
  add_executable(gdupe_gif_tests tests/test_gif.cpp)
  target_link_libraries(gdupe_gif_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_gif_tests COMMAND gdupe_gif_tests)
  if(MSVC)
    target_link_options(gdupe_tests PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
    target_link_options(gdupe_static_media_tests PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
    target_link_options(gdupe_main10_tests PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
    target_link_options(gdupe_gif_tests PRIVATE "$<$<CONFIG:Release>:/NODEFAULTLIB:LIBCMTD>")
  endif()
endif()