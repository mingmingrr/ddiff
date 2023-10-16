cmake_minimum_required (VERSION 3.12)

project(ddiff LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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

set(BOOST_INCLUDE_LIBRARIES
  json program_options filesystem system coroutine)
set(BOOST_ENABLE_CMAKE ON)

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
