cmake_minimum_required(VERSION 4.4)

project(spectral_lab VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

option(SPECTRAL_LAB_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

find_package(Threads REQUIRED)

function(enable_strict_warnings target)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${target} PRIVATE -g -Wall -Wextra -Wpedantic -Werror)
  elseif(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  endif()
endfunction()

if(SPECTRAL_LAB_SANITIZE)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
  else()
    message(WARNING "SPECTRAL_LAB_SANITIZE is only supported with Clang and GCC")
  endif()
endif()

find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/include/*.h"
    "${PROJECT_SOURCE_DIR}/src/*.cc"
    "${PROJECT_SOURCE_DIR}/src/*.h"
    "${PROJECT_SOURCE_DIR}/tests/*.cc"
    "${PROJECT_SOURCE_DIR}/tests/*.h"
  )
  add_custom_target(format
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${FORMAT_SOURCES}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "Formatting C++ source files"
    VERBATIM
  )
else()
  message(STATUS "clang-format was not found; the format target is unavailable")
endif()

add_subdirectory(src/ircore)
add_subdirectory(src/ircli)

if(BUILD_TESTING)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.18.0
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
    SYSTEM
  )
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
  add_subdirectory(tests)
endif()
