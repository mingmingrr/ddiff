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
  "libs/algorithm;"
  "libs/align;"
  "libs/any;"
  "libs/array;"
  "libs/asio;"
  "libs/assert;"
  "libs/atomic;"
  "libs/bind;"
  "libs/chrono;"
  "libs/concept_check;"
  "libs/config;"
  "libs/container;"
  "libs/container_hash;"
  "libs/context;"
  "libs/conversion;"
  "libs/core;"
  "libs/coroutine;"
  "libs/date_time;"
  "libs/describe;"
  "libs/detail;"
  "libs/dynamic_bitset;"
  "libs/exception;"
  "libs/filesystem;"
  "libs/function;"
  "libs/function_types;"
  "libs/functional;"
  "libs/fusion;"
  "libs/integer;"
  "libs/intrusive;"
  "libs/io;"
  "libs/iterator;"
  "libs/json;"
  "libs/lexical_cast;"
  "libs/math;"
  "libs/move;"
  "libs/mp11;"
  "libs/mpl;"
  "libs/multiprecision;"
  "libs/numeric;"
  "libs/optional;"
  "libs/pool;"
  "libs/predef;"
  "libs/preprocessor;"
  "libs/process;"
  "libs/program_options;"
  "libs/random;"
  "libs/range;"
  "libs/ratio;"
  "libs/rational;"
  "libs/regex;"
  "libs/smart_ptr;"
  "libs/static_assert;"
  "libs/system;"
  "libs/throw_exception;"
  "libs/tokenizer;"
  "libs/tuple;"
  "libs/type_index;"
  "libs/type_traits;"
  "libs/typeof;"
  "libs/unordered;"
  "libs/utility;"
  "libs/variant2;"
  "libs/winapi;"
  "tools/cmake"
)

set(BOOST_INCLUDE_LIBRARIES
  algorithm asio container_hash multiprecision process
  json program_options filesystem system coroutine)
set(BOOST_ENABLE_CMAKE ON)
FetchContent_Declare(boost
  GIT_REPOSITORY https://github.com/boostorg/boost.git
  GIT_TAG boost-1.81.0
  GIT_SUBMODULES ${BOOST_SUBMODULES}
)
FetchContent_MakeAvailable(boost)

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
  PRIVATE Boost::json
  PRIVATE Boost::program_options
  PRIVATE Boost::filesystem
  PRIVATE Boost::system
  PRIVATE Boost::process
  PRIVATE Boost::coroutine
  PRIVATE Boost::multiprecision
  PRIVATE Boost::algorithm
)
