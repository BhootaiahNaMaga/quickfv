cmake_minimum_required(VERSION 3.24)
project(quickfv LANGUAGES C CXX)

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

# CaDiCaL (MIT): SAT solver. It ships a configure/make build, so its library
# sources are compiled directly (NBUILD skips the generated build.hpp).
FetchContent_Declare(cadical
  GIT_REPOSITORY https://github.com/arminbiere/cadical.git
  GIT_TAG rel-3.0.1
  GIT_SHALLOW ON)
FetchContent_MakeAvailable(cadical)
file(GLOB CADICAL_SOURCES ${cadical_SOURCE_DIR}/src/*.cpp ${cadical_SOURCE_DIR}/src/*.c)
list(FILTER CADICAL_SOURCES EXCLUDE REGEX ".*/(cadical|mobical)\\.cpp$")
add_library(cadical STATIC ${CADICAL_SOURCES})
target_compile_definitions(cadical PRIVATE NBUILD NDEBUG
  $<$<PLATFORM_ID:Darwin>:NCLOSEFROM>)  # macOS has no closefrom()
target_include_directories(cadical PUBLIC ${cadical_SOURCE_DIR}/src)
target_compile_options(cadical PRIVATE -w)

# Btor2Tools (MIT): BTOR2 parser only.
FetchContent_Declare(btor2tools
  GIT_REPOSITORY https://github.com/hwmcc/btor2tools.git
  GIT_TAG ad94e9c61b
  SOURCE_SUBDIR no-cmake-build)  # sources only; skip its CMake project
FetchContent_MakeAvailable(btor2tools)
add_library(btor2parser STATIC ${btor2tools_SOURCE_DIR}/src/btor2parser/btor2parser.c)
target_include_directories(btor2parser PUBLIC ${btor2tools_SOURCE_DIR}/src)
set_target_properties(btor2parser PROPERTIES LINKER_LANGUAGE C)
target_compile_options(btor2parser PRIVATE -w)

add_library(qfv_core
  src/session.cpp
  src/lint.cpp
  src/json.cpp
  src/sva/seq_ir.cpp
  src/sva/lower.cpp
  src/sva/monitor.cpp
  src/model/aig.cpp
  src/model/btor2_load.cpp
  src/engine/bmc.cpp
  src/engine/sim.cpp)
target_include_directories(qfv_core PUBLIC src)
target_link_libraries(qfv_core PUBLIC slang::slang cadical btor2parser)
target_compile_options(qfv_core PRIVATE -Wall -Wextra)

add_executable(qfv tools_src/qfv_main.cpp)
target_link_libraries(qfv PRIVATE qfv_core)
