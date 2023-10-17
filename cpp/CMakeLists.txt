cmake_minimum_required (VERSION 3.12)

project(ddiff LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Release)

include(FetchContent)

set(FETCHCONTENT_UPDATES_DISCONNECTED TRUE)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v5.0.0
)
FetchContent_GetProperties(ftxui)
if(NOT ftxui_POPULATED)
  FetchContent_Populate(ftxui)
  add_subdirectory(${ftxui_SOURCE_DIR} ${ftxui_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()

set(BOOST_SUBMODULES
  "libs/align;libs/any;libs/array;libs/assert;libs/atomic;libs/bind;"
  "libs/concept_check;libs/config;libs/container;libs/container_hash;"
  "libs/context;libs/conversion;libs/core;libs/coroutine;libs/describe;"
  "libs/detail;libs/exception;libs/filesystem;libs/function;"
  "libs/function_types;libs/functional;libs/fusion;libs/integer;"
  "libs/intrusive;libs/io;libs/iterator;libs/json;libs/lexical_cast;"
  "libs/move;libs/mp11;libs/mpl;libs/numeric;libs/optional;"
  "libs/pool;libs/predef;libs/preprocessor;libs/program_options;"
  "libs/range;libs/regex;libs/smart_ptr;libs/static_assert;libs/system;"
  "libs/throw_exception;libs/tokenizer;libs/tuple;libs/type_index;"
  "libs/type_traits;libs/typeof;libs/utility;libs/variant2;libs/winapi;"
  "tools/cmake"
)

set(BOOST_INCLUDE_LIBRARIES
  json program_options filesystem system coroutine)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(boost
  GIT_REPOSITORY https://github.com/boostorg/boost.git
  GIT_TAG boost-1.81.0
  GIT_SUBMODULES ${BOOST_SUBMODULES}
)
FetchContent_GetProperties(boost)
if(NOT boost_POPULATED)
  FetchContent_Populate(boost)
  add_subdirectory(
    ${boost_SOURCE_DIR}
    ${boost_BINARY_DIR}
    EXCLUDE_FROM_ALL
  )
endif()

find_package(Boost REQUIRED COMPONENTS
  json program_options filesystem system coroutine)
include_directories(${Boost_INCLUDE_DIR})
link_directories(${Boost_LIBRARY_DIRS})

add_executable(ddiff
  src/main.cpp
  src/fileio.cpp
  src/diff.cpp
  src/trace.cpp
  src/opts.cpp
  src/natkey.cpp
)
target_include_directories(ddiff PRIVATE src)

target_link_libraries(ddiff
  PRIVATE ftxui::screen
  PRIVATE ftxui::dom
  PRIVATE ftxui::component
  ${Boost_LIBRARIES}
)
