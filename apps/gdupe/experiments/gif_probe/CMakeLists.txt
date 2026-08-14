cmake_minimum_required(VERSION 3.28)
if(POLICY CMP0091)
  cmake_policy(SET CMP0091 NEW)
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(gdupe_gif_probe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(FLTK_MSVC_RUNTIME_DLL OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FORMS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLUID OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_FLTK_OPTIONS OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_TEST OFF CACHE BOOL "" FORCE)
set(FLTK_BUILD_GL OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_CAIRO_EXT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_CAIRO_WINDOW OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_PRINT_SUPPORT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_FILESYSTEM_SUPPORT OFF CACHE BOOL "" FORCE)
set(FLTK_OPTION_SVG OFF CACHE BOOL "" FORCE)

include(FetchContent)
FetchContent_Declare(fltk
  GIT_REPOSITORY https://github.com/fltk/fltk.git
  GIT_TAG a9b1113516ffd15fc7602a6d425a317df30f4720
  EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(fltk)

add_executable(gif_probe
  ../gif_probe_main.cpp
  ../../src/gif_decode.cpp
)
target_include_directories(gif_probe PRIVATE ../../src)
target_link_libraries(gif_probe PRIVATE fltk::fltk fltk::images)
if(MSVC)
  target_compile_options(gif_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
