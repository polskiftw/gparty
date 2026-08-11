cmake_minimum_required(VERSION 3.28)
project(gdupe VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia MultimediaWidgets Concurrent)
find_package(OpenCV CONFIG REQUIRED COMPONENTS core imgproc imgcodecs videoio)
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
    Qt6::Core
    ${OpenCV_LIBS}
    OpenSSL::Crypto
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
)
target_compile_definitions(gdupe_core PRIVATE CURL_STATICLIB)
if(MSVC)
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

include(GNUInstallDirs)
install(TARGETS gdupe RUNTIME DESTINATION .)
install(FILES config/gdupe.example.json DESTINATION config)
qt_generate_deploy_app_script(TARGET gdupe OUTPUT_SCRIPT deploy_script NO_UNSUPPORTED_PLATFORM_ERROR)
install(SCRIPT ${deploy_script})

include(CTest)
if(BUILD_TESTING)
  add_executable(gdupe_tests tests/test_core.cpp)
  target_link_libraries(gdupe_tests PRIVATE gdupe_core)
  add_test(NAME gdupe_core_tests COMMAND gdupe_tests)
endif()
