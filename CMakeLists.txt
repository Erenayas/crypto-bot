cmake_minimum_required(VERSION 3.20)
project(hftbot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to an optimised build with symbols. Trading code is profiled far more
# often than it is stepped through in a debugger, and -O0 numbers are meaningless.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Export compile_commands.json so clangd / your editor understands the project.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Homebrew keeps OpenSSL keg-only, so CMake needs a hint on macOS.
if(APPLE AND EXISTS /opt/homebrew/opt/openssl@3)
  set(OPENSSL_ROOT_DIR /opt/homebrew/opt/openssl@3)
endif()

find_package(OpenSSL REQUIRED)
find_package(Boost REQUIRED)
find_package(simdjson REQUIRED)
find_package(ZLIB REQUIRED)

# Pure logic: order book, sync state machine, fixed-point. No I/O, no deps.
# This is the part that must be unit-testable, so it depends on nothing.
add_library(hft_core INTERFACE)
target_include_directories(hft_core INTERFACE include)
target_compile_options(hft_core INTERFACE -Wall -Wextra -Wpedantic)

# Transport. Boost lives behind this target and never leaks into hft_core.
add_library(hft_net src/binance_net.cpp)
target_link_libraries(hft_net PUBLIC hft_core PRIVATE Boost::headers OpenSSL::SSL OpenSSL::Crypto)

# Recording I/O. zlib lives behind this target.
add_library(hft_io src/gzfile.cpp)
target_link_libraries(hft_io PUBLIC hft_core PRIVATE ZLIB::ZLIB)

add_executable(hftbot src/main.cpp)
target_link_libraries(hftbot PRIVATE hft_core hft_net hft_io simdjson::simdjson)

add_executable(hftreplay src/replay_main.cpp)
target_link_libraries(hftreplay PRIVATE hft_core hft_io simdjson::simdjson)

add_executable(hftbacktest src/backtest_main.cpp)
target_link_libraries(hftbacktest PRIVATE hft_core hft_io simdjson::simdjson)

enable_testing()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE hft_core)
add_test(NAME core COMMAND test_core)

add_executable(test_recording tests/test_recording.cpp)
target_link_libraries(test_recording PRIVATE hft_core hft_io simdjson::simdjson)
add_test(NAME recording COMMAND test_recording)

# Debugging tool: dump raw WebSocket messages from any stream path.
add_executable(wsdump tools/wsdump.cpp)
target_link_libraries(wsdump PRIVATE hft_core hft_net)
