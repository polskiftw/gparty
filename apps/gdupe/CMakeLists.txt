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

# Source-pinned FLTK, static /MT. Its CMake feature switches are not sufficient
# source-pruning controls on Windows, so gdupe freezes the exact source closure
# required by its UI/GIF surface.
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

if(TARGET fltk)
  target_compile_definitions(fltk PRIVATE FL_NO_PRINT_SUPPORT)
  set(GDUPE_FLTK_CORE_SOURCE_STEMS
    case
    filename_absolute
    filename_ext
    filename_isdir
    filename_list
    filename_match
    Fl
    Fl_abort
    fl_arc
    Fl_arg
    fl_ask
    Fl_Bitmap
    Fl_Box
    fl_boxtype
    Fl_Browser
    Fl_Browser_
    Fl_Button
    fl_call_main
    fl_color
    Fl_compose
    fl_contrast
    Fl_Copy_Surface
    fl_cursor
    fl_curve
    Fl_Device
    Fl_Double_Window
    fl_dnd_win32
    fl_draw
    fl_draw_arrow
    fl_draw_pixmap
    fl_encoding_latin1
    fl_encoding_mac_roman
    fl_font
    Fl_GDI_Copy_Surface_Driver
    Fl_GDI_Graphics_Driver
    Fl_GDI_Graphics_Driver_arci
    Fl_GDI_Graphics_Driver_color
    Fl_GDI_Graphics_Driver_font
    Fl_GDI_Graphics_Driver_image
    Fl_GDI_Graphics_Driver_line_style
    Fl_GDI_Graphics_Driver_rect
    Fl_GDI_Graphics_Driver_vertex
    Fl_GDI_Image_Surface_Driver
    Fl_get_key_win32
    Fl_get_system_colors
    fl_gleam
    Fl_grab
    Fl_Graphics_Driver
    Fl_Group
    fl_gtk
    Fl_Image
    Fl_Image_Surface
    Fl_Input
    Fl_Input_
    fl_labeltype
    Fl_lock
    Fl_Menu
    Fl_Menu_Window
    Fl_Message
    numericsort
    fl_oxy
    Fl_Pixmap
    fl_plastic
    Fl_Preferences
    Fl_Progress
    fl_read_image
    fl_rect
    Fl_Return_Button
    fl_round_box
    fl_rounded_box
    Fl_Screen_Driver
    Fl_Scrollbar
    fl_set_font
    Fl_Shared_Image
    fl_shortcut
    Fl_Single_Window
    Fl_Slider
    Fl_String
    fl_string_functions
    fl_symbols
    Fl_System_Driver
    Fl_Text_Buffer
    Fl_Text_Display
    Fl_Text_Editor
    Fl_Tiled_Image
    Fl_Timeout
    Fl_Tooltip
    Fl_Tree_Prefs
    fl_utf8
    Fl_Valuator
    fl_vertex
    Fl_Widget
    Fl_Widget_Surface
    Fl_win32
    fl_WinAPI_platform_init
    Fl_WinAPI_Screen_Driver
    Fl_WinAPI_System_Driver
    Fl_WinAPI_Window_Driver
    Fl_Window
    Fl_Window_Driver
    Fl_Window_fullscreen
    Fl_Window_hotspot
    Fl_Wizard
    Fl_XBM_Image
    Fl_XPM_Image
    flstring
    is_spacing
    scandir_win32
    screen_xywh
    vsnprintf
  )
  get_target_property(GDUPE_FLTK_CORE_SOURCES fltk SOURCES)
  set(GDUPE_FLTK_CORE_SOURCES_TRIMMED "")
  set(GDUPE_FLTK_CORE_STEMS_FOUND "")
  foreach(GDUPE_FLTK_SOURCE IN LISTS GDUPE_FLTK_CORE_SOURCES)
    get_filename_component(GDUPE_FLTK_SOURCE_STEM "${GDUPE_FLTK_SOURCE}" NAME_WE)
    if(GDUPE_FLTK_SOURCE_STEM IN_LIST GDUPE_FLTK_CORE_SOURCE_STEMS)
      list(APPEND GDUPE_FLTK_CORE_SOURCES_TRIMMED "${GDUPE_FLTK_SOURCE}")
      list(APPEND GDUPE_FLTK_CORE_STEMS_FOUND "${GDUPE_FLTK_SOURCE_STEM}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES GDUPE_FLTK_CORE_STEMS_FOUND)
  foreach(GDUPE_FLTK_REQUIRED_STEM IN LISTS GDUPE_FLTK_CORE_SOURCE_STEMS)
    if(NOT GDUPE_FLTK_REQUIRED_STEM IN_LIST GDUPE_FLTK_CORE_STEMS_FOUND)
      message(FATAL_ERROR "Pinned FLTK source closure changed; missing ${GDUPE_FLTK_REQUIRED_STEM}")
    endif()
  endforeach()
  set_property(TARGET fltk PROPERTY SOURCES "${GDUPE_FLTK_CORE_SOURCES_TRIMMED}")
  target_sources(fltk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/fltk_print_stub.cpp")
endif()

if(TARGET fltk_images)
  get_target_property(GDUPE_FLTK_IMAGE_SOURCES fltk_images SOURCES)
  set(GDUPE_FLTK_IMAGE_SOURCE_NAMES Fl_Anim_GIF_Image.cxx Fl_GIF_Image.cxx Fl_Image_Reader.cxx)
  set(GDUPE_FLTK_IMAGE_SOURCES_TRIMMED "")
  set(GDUPE_FLTK_IMAGE_NAMES_FOUND "")
  foreach(GDUPE_FLTK_SOURCE IN LISTS GDUPE_FLTK_IMAGE_SOURCES)
    get_filename_component(GDUPE_FLTK_SOURCE_NAME "${GDUPE_FLTK_SOURCE}" NAME)
    if(GDUPE_FLTK_SOURCE_NAME IN_LIST GDUPE_FLTK_IMAGE_SOURCE_NAMES)
      list(APPEND GDUPE_FLTK_IMAGE_SOURCES_TRIMMED "${GDUPE_FLTK_SOURCE}")
      list(APPEND GDUPE_FLTK_IMAGE_NAMES_FOUND "${GDUPE_FLTK_SOURCE_NAME}")
    endif()
  endforeach()
  foreach(GDUPE_FLTK_REQUIRED_IMAGE IN LISTS GDUPE_FLTK_IMAGE_SOURCE_NAMES)
    if(NOT GDUPE_FLTK_REQUIRED_IMAGE IN_LIST GDUPE_FLTK_IMAGE_NAMES_FOUND)
      message(FATAL_ERROR "Pinned FLTK GIF surface changed; missing ${GDUPE_FLTK_REQUIRED_IMAGE}")
    endif()
  endforeach()
  set_property(TARGET fltk_images PROPERTY SOURCES "${GDUPE_FLTK_IMAGE_SOURCES_TRIMMED}")
  set_property(TARGET fltk_images PROPERTY LINK_LIBRARIES "fltk::fltk")
  set_property(TARGET fltk_images PROPERTY INTERFACE_LINK_LIBRARIES "fltk::fltk")
endif()

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
  src/gif_decode.cpp
  src/media_decode.cpp
  src/mp4_decode.cpp
  src/fingerprint.cpp
  src/matcher.cpp
  src/engine.cpp
)
target_include_directories(gdupe_core PUBLIC src)
target_link_libraries(gdupe_core PUBLIC
  JPEG::JPEG PNG::PNG WebP::webpdecoder CURL::libcurl unofficial::sqlite3::sqlite3
  nlohmann_json::nlohmann_json fltk::fltk fltk::images gdupe_minimp4 Dav1d::dav1d
  unofficial::libvpx::libvpx unofficial::libwebm::libwebm gdupe_avc_adapter gdupe_hevc_adapter
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
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE" DESTINATION .)
install(FILES config/gdupe.example.json DESTINATION config)
install(FILES "${GDUPE_LIBAVC_DIR}/license/LICENSE" "${GDUPE_LIBAVC_DIR}/SOURCE.txt" DESTINATION licenses/libavc)
install(FILES "${GDUPE_LIBHEVC_DIR}/license/LICENSE" "${GDUPE_LIBHEVC_DIR}/SOURCE.txt" DESTINATION licenses/libhevc)
install(FILES "${minimp4_source_SOURCE_DIR}/LICENSE" DESTINATION licenses/minimp4)
install(FILES "${fltk_SOURCE_DIR}/COPYING" FLTK-SOURCE.txt DESTINATION licenses/fltk)
install(FILES RUNTIME-SURFACE.md DESTINATION .)
foreach(PACKAGE curl dav1d libjpeg-turbo libpng libvpx libwebm libwebp nlohmann-json sqlite3 zlib)
  install(FILES "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share/${PACKAGE}/copyright" DESTINATION "licenses/vcpkg/${PACKAGE}")
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
