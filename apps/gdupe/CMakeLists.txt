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

set(GDUPE_FFMPEG_DIR "" CACHE PATH "Directory containing gdupe's pinned minimal FFmpeg SDK")
if(NOT GDUPE_FFMPEG_DIR)
  message(FATAL_ERROR "GDUPE_FFMPEG_DIR must point to the pinned minimal FFmpeg SDK")
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
    PRE_EXCLUDE_REGEXES
      "api-ms-.*"
      "ext-ms-.*"
      "avcodec-[0-9]+\\.dll"
      "avformat-[0-9]+\\.dll"
      "avutil-[0-9]+\\.dll"
      "swscale-[0-9]+\\.dll"
    POST_EXCLUDE_REGEXES ".*[Ww]indows[/\\\\][Ss]ystem32[/\\\\].*\\.dll"
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES qt.conf DESTINATION .)
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
  set_tests_properties(gdupe_core_tests PROPERTIES ENVIRONMENT_MODIFICATION
    "PATH=path_list_prepend:${GDUPE_FFMPEG_DIR}"
  )
endif()
