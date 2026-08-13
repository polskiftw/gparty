cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0207)
  cmake_policy(SET CMP0207 NEW)
endif()
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(gdupe VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(WIN32)
  add_compile_definitions(NOMINMAX)
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets Concurrent)
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(OpenSSL REQUIRED)
find_package(CURL CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(gdupe_core
  src/config.cpp
  src/database.cpp
  src/b2_client.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/engine.cpp
)
target_include_directories(gdupe_core PUBLIC src)
target_link_libraries(gdupe_core
  PUBLIC
    ${OpenCV_LIBS}
    Qt6::Core
    OpenSSL::Crypto
    CURL::libcurl
    SQLite3::SQLite3
    nlohmann_json::nlohmann_json
)
if(MSVC)
  target_compile_definitions(gdupe_core PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_compile_options(gdupe_core PRIVATE /W4 /permissive- /EHsc /utf-8)
else()
  target_compile_options(gdupe_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

qt_add_executable(gdupe WIN32
  src/main.cpp
  src/main_window.cpp
)
target_link_libraries(gdupe PRIVATE
  gdupe_core
  Qt6::Gui
  Qt6::Widgets
  Qt6::Network
  Qt6::Multimedia
  Qt6::MultimediaWidgets
  Qt6::Concurrent
)

set(CMAKE_INSTALL_BINDIR "." CACHE PATH "gdupe portable runtime directory" FORCE)
include(GNUInstallDirs)
install(TARGETS gdupe
  RUNTIME_DEPENDENCIES
    DIRECTORIES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
    PRE_EXCLUDE_REGEXES "api-ms-.*" "ext-ms-.*"
    POST_EXCLUDE_REGEXES ".*[Ww]indows[/\\\\][Ss]ystem32[/\\\\].*\\.dll"
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES qt.conf DESTINATION .)
set(GDUPE_FFMPEG_DIR "" CACHE PATH "Directory containing the pinned minimal FFmpeg runtime")
if(GDUPE_FFMPEG_DIR)
  set(GDUPE_FFMPEG_RUNTIME_FILES
    ffmpeg.exe
    ffprobe.exe
    avcodec-63.dll
    avfilter-12.dll
    avformat-63.dll
    avutil-61.dll
    swscale-10.dll
  )
  foreach(GDUPE_FFMPEG_FILE IN LISTS GDUPE_FFMPEG_RUNTIME_FILES)
    if(NOT EXISTS "${GDUPE_FFMPEG_DIR}/${GDUPE_FFMPEG_FILE}")
      message(FATAL_ERROR "Pinned minimal FFmpeg runtime is missing ${GDUPE_FFMPEG_FILE}")
    endif()
  endforeach()
  install(PROGRAMS
    "${GDUPE_FFMPEG_DIR}/ffmpeg.exe"
    "${GDUPE_FFMPEG_DIR}/ffprobe.exe"
    DESTINATION tools
  )
  install(FILES
    "${GDUPE_FFMPEG_DIR}/avcodec-63.dll"
    "${GDUPE_FFMPEG_DIR}/avfilter-12.dll"
    "${GDUPE_FFMPEG_DIR}/avformat-63.dll"
    "${GDUPE_FFMPEG_DIR}/avutil-61.dll"
    "${GDUPE_FFMPEG_DIR}/swscale-10.dll"
    DESTINATION tools
  )
  install(FILES "${GDUPE_FFMPEG_DIR}/LICENSE.txt" DESTINATION tools RENAME FFmpeg-LICENSE.txt)
  install(FILES "${GDUPE_FFMPEG_DIR}/LICENSE-NOTICE.md" DESTINATION tools RENAME FFmpeg-LICENSE-NOTICE.md)
  install(FILES "${GDUPE_FFMPEG_DIR}/Zlib-LICENSE.txt" DESTINATION tools)
  install(FILES "${GDUPE_FFMPEG_DIR}/SOURCE.txt" DESTINATION tools RENAME FFmpeg-SOURCE.txt)
endif()
qt_generate_deploy_script(
  TARGET gdupe
  OUTPUT_SCRIPT deploy_script
  CONTENT "
qt_deploy_runtime_dependencies(
  EXECUTABLE \"$<TARGET_FILE_NAME:gdupe>\"
  BIN_DIR \".\"
  LIB_DIR \".\"
  PLUGINS_DIR \"plugins\"
  QML_DIR \"qml\"
  NO_TRANSLATIONS
)"
)
install(SCRIPT ${deploy_script})

include(CTest)
if(BUILD_TESTING)
  add_executable(gdupe_tests tests/test_core.cpp)
  target_link_libraries(gdupe_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_core_tests COMMAND gdupe_tests)
  if(GDUPE_FFMPEG_DIR)
    set_tests_properties(gdupe_core_tests PROPERTIES ENVIRONMENT
      "GDUPE_TEST_FFMPEG=${GDUPE_FFMPEG_DIR}/ffmpeg.exe;GDUPE_TEST_FFPROBE=${GDUPE_FFMPEG_DIR}/ffprobe.exe"
    )
  endif()
endif()
