cmake_minimum_required(VERSION 3.24)
project(quickfv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)  # slang requires C++20
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_PYLIB OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(slang
  GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
  GIT_TAG v11.0
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(slang)

add_library(qfv_core
  src/session.cpp
  src/lint.cpp
  src/json.cpp
  src/sva/seq_ir.cpp
  src/sva/lower.cpp
  src/sva/monitor.cpp)
target_include_directories(qfv_core PUBLIC src)
target_link_libraries(qfv_core PUBLIC slang::slang)
target_compile_options(qfv_core PRIVATE -Wall -Wextra)

add_executable(qfv tools_src/qfv_main.cpp)
target_link_libraries(qfv PRIVATE qfv_core)
